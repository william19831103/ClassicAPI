// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// ClassicAPI. If not, see <https://www.gnu.org/licenses/>.

#include "../Game.h"
#include "../Offsets.h"
#include "../guid/Guid.h"
#include "../object/Resolve.h"

#include <cstdint>

namespace Loot::Nearby {

namespace {

// Engine interact-range constants, read out of `.rdata` once and
// pinned here as `constexpr`. Both are referenced by `FUN_005EC110`
// (the master interact-state checker) and propagated through
// `FUN_005DF130` / `FUN_005DF2A0` (CMSG_LOOT senders). Yards units;
// values verified at runtime addresses `DAT_0080B058` and
// `DAT_0080A1E8`.
//
//   INTERACT_REACH (1.333…) is added to `target_radius + player_radius`
//   for the unclamped range; modeled as the "reach beyond your touch"
//   constant.
//   MIN_INTERACT_RANGE (5.0) clamps the result so even tiny
//   targets with near-zero bounding radius still get the standard
//   5-yard right-click reach.
constexpr float INTERACT_REACH = 1.3333333f;
constexpr float MIN_INTERACT_RANGE = 5.0f;

// `ClntObjMgrEnumVisibleObjects(callback, context)` — walks the engine's
// visible-object table. The callback shape's `void *unusedEdx` slot
// absorbs the second `__fastcall` register so MSVC lays out the trailing
// `uint64_t guid` on the stack exactly where the engine puts it. The
// callback returns non-zero to keep iterating; zero stops the walk early.
using ClntObjMgrEnumVisibleObjectsCallback_t = int(__fastcall *)(void *ctx,
                                                                  void *unusedEdx,
                                                                  uint64_t guid);
using ClntObjMgrEnumVisibleObjects_t =
    int(__fastcall *)(ClntObjMgrEnumVisibleObjectsCallback_t cb, void *ctx);

// `__thiscall(self, outBuf) → const C3Vector *` at vtable slot 5
// (byte offset `0x14`). Both CGUnit_C and CGPlayer_C populate the
// caller-provided 12-byte buffer with the unit's world position and
// return it. Same call shape `FUN_005EC110` uses to read the
// player / target positions when deciding interact range.
struct C3Vector {
    float x, y, z;
};
using GetPosition_t = const C3Vector *(__thiscall *)(const void *self,
                                                      C3Vector *outBuf);
constexpr int VTBL_GET_POSITION_OFFSET = 0x14;

// Reads `(unit.m_objectFields[+0x224] & UNIT_DYNFLAG_LOOTABLE) != 0`
// — the server-driven bit it sets when the local player has loot
// rights on this unit. Works uniformly for live and dead CGUnit
// subclasses; the bit is simply 0 for non-lootable targets.
//
// Don't call `FUN_CGCORPSE_IS_LOOTABLE` here even though the engine
// itself uses it in `FUN_005D6BF0`: that helper reads `+0x78` of
// m_objectFields, which is the CORPSE dynamic-flags slot for
// CGCorpse_C but the BYTES_0 (race / class / gender / power type)
// slot for CGUnit_C. Routing live units through it returns the low
// bit of *race* — yields false positives for any Human / Dwarf /
// Undead / Gnome player and false negatives for dead mobs.
bool IsLootable(const void *unit) {
    auto *fields = Game::Read<const uint8_t *>(unit, Offsets::OFF_UNIT_DESCRIPTOR);
    const uint32_t flags =
        Game::Read<uint32_t>(fields, Offsets::OFF_UNIT_FIELD_DYNAMIC_FLAGS);
    return (flags & Offsets::UNIT_DYNFLAG_LOOTABLE) != 0;
}

// Reads the bounding-radius float from a unit's m_objectFields. The
// engine uses this as the contribution from both sides (player + target)
// in its interact-range computation.
float BoundingRadius(const void *unit) {
    auto *fields = Game::Read<const uint8_t *>(unit, Offsets::OFF_UNIT_DESCRIPTOR);
    return Game::Read<float>(fields, Offsets::OFF_UNIT_FIELD_BOUNDING_RADIUS);
}

// Invokes the unit's virtual `GetPosition` (vtable slot at offset
// `0x14`). The engine's signature is `__thiscall(this, outBuf) →
// outBuf`; we pass our own 12-byte stack buffer and read x/y/z out
// of the returned pointer.
const C3Vector *GetPosition(const void *unit, C3Vector *outBuf) {
    auto vtable = *reinterpret_cast<void *const *const *>(unit);
    auto fn = reinterpret_cast<GetPosition_t>(
        *reinterpret_cast<const void *const *>(
            reinterpret_cast<const uint8_t *>(vtable) +
            VTBL_GET_POSITION_OFFSET));
    return fn(unit, outBuf);
}

// Mirrors the range portion of `FUN_005EC110`. We skip the engine's
// player-state checks (mounted / casting / prior-interaction GUID
// gate) because they fire intermittently during normal play and would
// make the enumerator return an empty list whenever the player has
// already clicked something — surprising for a "what's lootable
// nearby" reader. The pure distance test is what controls cursor
// highlight, which is the contract the user asked for.
bool InInteractRange(const void *player, const void *target) {
    C3Vector pBuf{};
    C3Vector tBuf{};
    const C3Vector *pPos = GetPosition(player, &pBuf);
    const C3Vector *tPos = GetPosition(target, &tBuf);
    if (pPos == nullptr || tPos == nullptr)
        return false;

    const float dx = pPos->x - tPos->x;
    const float dy = pPos->y - tPos->y;
    const float dz = pPos->z - tPos->z;
    const float distSq = dx * dx + dy * dy + dz * dz;

    float range = BoundingRadius(player) + BoundingRadius(target) + INTERACT_REACH;
    if (range < MIN_INTERACT_RANGE)
        range = MIN_INTERACT_RANGE;
    return distSq <= range * range;
}

// Only `TYPEMASK_UNIT` carries lootable-mob state in 1.12 — dead
// creatures stay as CGCreature_C objects with the lootable bit flipped
// on, rather than transitioning to a separate corpse object. Dead
// players become CGCorpse_C (`TYPEMASK_CORPSE`) but those aren't
// lootable by other players in vanilla — no reason to walk them here.

struct ScanCtx {
    void *L;
    const void *player; // resolved once outside the iterator
    int idx;            // 1-based slot of the next entry to append
};

int __fastcall ScanCallback(ScanCtx *ctx, void * /*unusedEdx*/, uint64_t guid) {
    void *obj = Object::ByGuid(Offsets::TYPEMASK_UNIT, guid, nullptr, 0);
    if (obj == nullptr)
        return 1; // GUID isn't a unit; continue

    if (!IsLootable(obj))
        return 1; // unit isn't lootable to us; continue

    if (!InInteractRange(ctx->player, obj))
        return 1; // out of click-loot range; continue

    char guidBuf[Guid::STRING_SIZE];
    Game::Lua::PushNumber(ctx->L, static_cast<double>(ctx->idx));
    Game::Lua::NewTable(ctx->L);
    Game::Lua::SetFieldString(ctx->L, "guid",
                              Guid::FormatAsString(guid, guidBuf, sizeof guidBuf));
    Game::Lua::SetTable(ctx->L, -3);
    ++ctx->idx;
    return 1;
}

// `C_Loot.GetNearbyLootableUnits()` — returns a 1-indexed array of
// `{ guid = "0x..." }` subtables, one per visible unit that is (a)
// flagged DYNFLAG_LOOTABLE by the server and (b) inside the engine's
// own interact range (the distance threshold that lights up the
// right-click-loot cursor). Empty table pre-login, when no lootable
// units are in range, or when we don't have loot rights on visible
// corpses (the server only sets the bit for clients with rights).
int __fastcall Script_GetNearbyLootableUnits(void *L) {
    Game::Lua::SetTop(L, 0);
    Game::Lua::NewTable(L);

    // The engine derefs the object manager unconditionally on iterator
    // entry. NULL during glue / character-select; bail with an empty
    // table rather than crashing.
    if (*reinterpret_cast<void *volatile *>(Offsets::VAR_LOCAL_PLAYER_PTR) == nullptr)
        return 1;

    // Resolve the local player once — the canonical CGPlayer_C the
    // engine's own interact-range checks use, distinct from the
    // `VAR_LOCAL_PLAYER_PTR` global. Bail if unavailable.
    const void *player = Game::ResolveUnitToken("player");
    if (player == nullptr)
        return 1;

    auto Enum = reinterpret_cast<ClntObjMgrEnumVisibleObjects_t>(
        Offsets::FUN_CLNT_OBJ_MGR_ENUM_VISIBLE_OBJECTS);
    ScanCtx ctx{L, player, 1};
    Enum(reinterpret_cast<ClntObjMgrEnumVisibleObjectsCallback_t>(&ScanCallback),
         &ctx);
    return 1;
}

void Register() {
    Game::Lua::RegisterTableFunction("C_Loot", "GetNearbyLootableUnits",
                                     &Script_GetNearbyLootableUnits);
}

} // namespace

static const Game::ModuleAutoRegister _autoreg{&Register};

} // namespace Loot::Nearby
