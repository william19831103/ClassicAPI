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

#include "Game.h"
#include "Offsets.h"
#include "dbc/Lookup.h"

#include <cstdint>

namespace Faction::Info {

namespace {

// __fastcall(ecx = 0-based displayed index) → factionID, or 0 for OOB.
// MSVC's free-function __fastcall puts the first int arg in ECX, matching
// the engine's calling convention for this helper.
using ResolveIndex_t = int(__fastcall *)(int idx);
using GetReactionBand_t = unsigned char(__fastcall *)(int factionID);
using GetStanding_t = int(__fastcall *)(int factionID);

ResolveIndex_t Resolver() {
    return reinterpret_cast<ResolveIndex_t>(Offsets::FUN_RESOLVE_FACTION_INDEX);
}

// Returns the Faction.dbc record pointer for `factionID`, or nullptr if
// the ID is out of range or the slot is empty.
const uint8_t *FactionRecord(int factionID) {
    if (factionID <= 0)
        return nullptr;
    return DBC::Record(Offsets::VAR_FACTION_DBC_RECORDS,
                       Offsets::VAR_FACTION_DBC_COUNT,
                       static_cast<uint32_t>(factionID));
}

// Reads `record[offset + locale*4]` as a localized C string pointer.
const char *LocalizedString(const uint8_t *record, int offset) {
    const int locale = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_LOCALE_INDEX));
    auto *strings = reinterpret_cast<const char *const *>(record + offset);
    return strings[locale];
}

// Pushes 1.0 for true and nil for false — the convention
// Script_GetFactionInfo uses for atWar/canToggleAtWar/etc. (engine
// pushes lua_pushnumber(1.0) or lua_pushnil at 0x004D65F2 /
// 0x004D6600 etc.). The engine has lua_pushboolean (we use it via
// Game::Lua::PushBoolean for C_* namespace functions); we match
// the number-or-nil shape here so encountered and unencountered
// factions return identically and addons can use a single
// comparison path.
void PushFlag(void *L, bool value) {
    if (value)
        Game::Lua::PushNumber(L, 1.0);
    else
        Game::Lua::PushNil(L);
}

// Resolves the local player's CGPlayer-side info sub-struct at
// `[player + 0xE68]`. Returns nullptr if "player" isn't resolvable
// or the sub-struct is uninitialized (pre-login / glue).
const uint8_t *PlayerInfo() {
    auto *player = static_cast<const uint8_t *>(Game::ResolveUnitToken("player"));
    if (player == nullptr)
        return nullptr;
    return *reinterpret_cast<const uint8_t *const *>(
        player + Offsets::OFF_CGPLAYER_INFO);
}

// Returns the player's RepListID for the currently-watched faction,
// or -1 if no faction is watched.
int WatchedRepListID(const uint8_t *playerInfo) {
    return *reinterpret_cast<const int *>(
        playerInfo + Offsets::OFF_CGPLAYER_INFO_WATCHED_REP_LIST_ID);
}

// Snapshot of everything `Script_GetFactionInfo` derives for one
// faction. All fields are populated engine-direct — no
// `Script_GetFactionInfo` round-trip, no `lua_pcall` into any other
// Lua-side accessor. Used by:
//   - `GetFactionInfoByID` (pushes positional 11 returns)
//   - `C_Reputation.GetWatchedFactionData` (builds a modern table)
//   - `C_Reputation.GetFactionDataByIndex` (builds a modern table)
struct FactionData {
    int factionID;
    int repListIndex; // -1 if the faction has no rep slot (header/etc.)
    const char *name;
    const char *description;
    int reaction; // 1..8 (Hated..Exalted)
    int currentReactionThreshold;
    int nextReactionThreshold;
    int currentStanding;
    bool atWarWith;
    bool canToggleAtWar;
    bool isHeader;
    bool isCollapsed;
    bool isWatched;
};

// Fills `out` with the engine's view of `factionID`. Returns false
// only when the factionID doesn't resolve to a `Faction.dbc` record
// (out-of-range or empty slot) — callers should treat that as "no
// such faction". Unencountered factions (record exists, no rep slot)
// fill cleanly with currentStanding=0, atWar=false, etc.
//
// Reads, in order:
//   - Faction.dbc record  → name, description, repListIndex
//   - FUN_REPUTATION_GET_REACTION_BAND → reaction band (0..7)
//   - VAR_REACTION_MIN/MAX_TABLE       → bar thresholds for that band
//   - FUN_REPUTATION_GET_STANDING      → currentStanding
//   - rep slot flags (when repListIndex >= 0) → atWar, canToggleAtWar
//   - VAR_FACTION_HEADER_LIST / COLLAPSED_BITMASK → isHeader, isCollapsed
//   - player's watched repListID       → isWatched
//
// `canToggleAtWar` mirrors `Script_GetFactionInfo`'s logic exactly:
// false when `currentStanding < -3000` OR the rep slot's bit `0x10`
// is set (the "peace-forced" / "permanent allegiance" flag — true
// for your own faction's leader cities, false for togglable factions
// like the Goblin cartels).
bool ReadFactionData(int factionID, FactionData *out) {
    *out = {};
    if (factionID <= 0)
        return false;

    const uint8_t *record = FactionRecord(factionID);
    if (record == nullptr)
        return false;

    out->factionID = factionID;
    out->repListIndex = *reinterpret_cast<const int32_t *>(
        record + Offsets::OFF_FACTION_REP_LIST_INDEX);
    out->name = LocalizedString(record, Offsets::OFF_FACTION_NAMES);
    out->description = LocalizedString(record, Offsets::OFF_FACTION_DESCRIPTIONS);
    if (out->description == nullptr)
        out->description = "";

    auto getBand = reinterpret_cast<GetReactionBand_t>(
        Offsets::FUN_REPUTATION_GET_REACTION_BAND);
    auto getStanding = reinterpret_cast<GetStanding_t>(
        Offsets::FUN_REPUTATION_GET_STANDING);

    const int band = getBand(factionID);
    out->reaction = band + 1;
    out->currentReactionThreshold = *reinterpret_cast<const int32_t *>(
        static_cast<uintptr_t>(Offsets::VAR_REACTION_MIN_TABLE) +
        static_cast<uintptr_t>(band) * 4);
    out->nextReactionThreshold = *reinterpret_cast<const int32_t *>(
        static_cast<uintptr_t>(Offsets::VAR_REACTION_MAX_TABLE) +
        static_cast<uintptr_t>(band) * 4);
    out->currentStanding = getStanding(factionID);

    if (out->repListIndex >= 0 && out->repListIndex < Offsets::MAX_REP_SLOTS) {
        auto *slot = reinterpret_cast<const uint8_t *>(
            static_cast<uintptr_t>(Offsets::VAR_PLAYER_REP_SLOTS) +
            static_cast<uintptr_t>(out->repListIndex) * Offsets::REP_SLOT_STRIDE);
        const uint8_t flags = *(slot + Offsets::OFF_REP_SLOT_FLAGS);
        out->atWarWith = (flags & Offsets::REP_SLOT_FLAG_AT_WAR) != 0;
        // canToggleAtWar matches `Script_GetFactionInfo`'s composite
        // check: standing not below -3000 AND not peace-forced.
        out->canToggleAtWar = (out->currentStanding >= -3000) &&
                              (flags & Offsets::REP_SLOT_FLAG_PEACE_FORCED) == 0;
    }

    // Header / collapsed: factionID's position in the displayed-list
    // header array determines both. Same loop `Script_GetFactionInfo`
    // walks at `0x004D6638`.
    const int headerCount = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_FACTION_HEADER_COUNT));
    auto *headerList = reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_FACTION_HEADER_LIST));
    const int cap = headerCount < Offsets::MAX_FACTION_HEADERS
                        ? headerCount : Offsets::MAX_FACTION_HEADERS;
    for (int i = 0; i < cap; ++i) {
        if (headerList[i] == factionID) {
            out->isHeader = true;
            const uint32_t mask = *reinterpret_cast<const uint32_t *>(
                static_cast<uintptr_t>(Offsets::VAR_FACTION_COLLAPSED_BITMASK));
            // Bit SET = expanded, bit CLEAR = collapsed.
            out->isCollapsed = (mask & (1u << i)) == 0;
            break;
        }
    }

    // isWatched: faction's repListIndex matches player's watched slot.
    if (out->repListIndex >= 0) {
        if (const uint8_t *info = PlayerInfo()) {
            out->isWatched = (out->repListIndex == WatchedRepListID(info));
        }
    }

    return true;
}

// Builds the modern `FactionData`-shape table on the Lua stack from
// a populated `FactionData`. Caller can override `isWatched` (e.g.
// `GetWatchedFactionData` forces it true; nothing else does).
void PushFactionDataTable(void *L, const FactionData &d) {
    Game::Lua::NewTable(L);
    Game::Lua::SetFieldNumber(L, "factionID", static_cast<double>(d.factionID));
    Game::Lua::SetFieldString(L, "name", d.name);
    Game::Lua::SetFieldString(L, "description", d.description);
    Game::Lua::SetFieldNumber(L, "reaction", static_cast<double>(d.reaction));
    Game::Lua::SetFieldNumber(L, "currentReactionThreshold",
        static_cast<double>(d.currentReactionThreshold));
    Game::Lua::SetFieldNumber(L, "nextReactionThreshold",
        static_cast<double>(d.nextReactionThreshold));
    Game::Lua::SetFieldNumber(L, "currentStanding",
        static_cast<double>(d.currentStanding));
    Game::Lua::SetFieldBool(L, "atWarWith", d.atWarWith);
    Game::Lua::SetFieldBool(L, "canToggleAtWar", d.canToggleAtWar);
    Game::Lua::SetFieldBool(L, "isHeader", d.isHeader);
    // Vanilla doesn't have parent factions with their own rep
    // aggregation, so a header never has rep. Always false.
    Game::Lua::SetFieldBool(L, "isHeaderWithRep", false);
    Game::Lua::SetFieldBool(L, "isCollapsed", d.isCollapsed);
    Game::Lua::SetFieldBool(L, "isWatched", d.isWatched);
    // canSetInactive: vanilla has the SetFactionInactive /
    // SetFactionActive Lua surface and the engine's underlying
    // SetInactiveFlag at `0x004D60F0`. It accepts any real factionID
    // (not pseudo-rows, not headers), so the predicate is "this
    // faction has a real rep slot and isn't a category header".
    Game::Lua::SetFieldBool(L, "canSetInactive",
                            !d.isHeader && d.repListIndex >= 0);
    // Modern flags with no vanilla source — stubbed for API parity.
    // isChild (Cataclysm+), hasBonusRepGain (MoP+), isAccountWide
    // (Dragonflight+).
    Game::Lua::SetFieldBool(L, "isChild", false);
    Game::Lua::SetFieldBool(L, "hasBonusRepGain", false);
    Game::Lua::SetFieldBool(L, "isAccountWide", false);
}

} // namespace

static int __fastcall Script_GetFactionIDByIndex(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L, "Usage: GetFactionIDByIndex(factionIndex)");
        return 0;
    }
    const int idx = static_cast<int>(Game::Lua::ToNumber(L, 1)) - 1;
    if (idx < 0)
        return 0; // nil for non-positive indices

    // Match Script_GetFactionInfo's acceptance: we bound against
    // `[VAR_FACTION_VISIBLE_MAX_INDEX]` (the same value the resolver
    // checks internally), which is wider than `GetNumFactions()` because
    // it also covers the "Inactive" / collapsed-category rows.
    const int maxIdx = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_FACTION_VISIBLE_MAX_INDEX));
    if (idx > maxIdx)
        return 0; // nil for out-of-range

    // Resolver returns the factionID for real entries; for header /
    // category rows it returns 0 ("Other") or -1 ("Inactive"-style
    // pseudo-row). Modern WoW (Classic Era 1.15.x) normalizes both to 0
    // in the `factionID` slot of `GetFactionInfo`, so we do the same.
    // Final convention: 0 for any header, nil for OOB, real factionID
    // for real factions. Matches GetQuestIDForLogIndex's headers-are-0
    // convention and modern WoW's GetFactionInfo[14] semantics.
    const int factionID = Resolver()(idx);
    Game::Lua::PushNumber(L, static_cast<double>(factionID > 0 ? factionID : 0));
    return 1;
}

// `GetFactionInfoByID(factionID)` — vanilla-positional 11-tuple
// matching `GetFactionInfo(index)`'s return shape. Built engine-direct
// via `ReadFactionData` — no Lua-side round-trip through
// `Script_GetFactionInfo`.
//
// 11th return matches what `Script_GetFactionInfo` pushes (the engine
// pushes 1.0 when the faction is the currently-watched one and nil
// otherwise — i.e. `isWatched`, not `hasRep` as some older docs
// describe it). For unencountered factions this is always nil.
static int __fastcall Script_GetFactionInfoByID(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L, "Usage: GetFactionInfoByID(factionID)");
        return 0;
    }
    const int factionID = static_cast<int>(Game::Lua::ToNumber(L, 1));
    FactionData d;
    if (!ReadFactionData(factionID, &d))
        return 0;
    if (d.name == nullptr || *d.name == '\0')
        return 0;

    Game::Lua::SetTop(L, 0);
    Game::Lua::PushString(L, d.name);                              // 1
    Game::Lua::PushString(L, d.description);                       // 2
    Game::Lua::PushNumber(L, static_cast<double>(d.reaction));     // 3
    Game::Lua::PushNumber(L,
        static_cast<double>(d.currentReactionThreshold));          // 4
    Game::Lua::PushNumber(L,
        static_cast<double>(d.nextReactionThreshold));             // 5
    Game::Lua::PushNumber(L,
        static_cast<double>(d.currentStanding));                   // 6
    PushFlag(L, d.atWarWith);                                       // 7
    PushFlag(L, d.canToggleAtWar);                                  // 8
    PushFlag(L, d.isHeader);                                        // 9
    PushFlag(L, d.isCollapsed);                                     // 10
    PushFlag(L, d.isWatched);                                       // 11
    return 11;
}

// `GetFactionParentID(factionID)` — returns the parent factionID for a
// faction in a hierarchy (e.g. Stormwind's parent is Alliance Forces;
// The Defilers's parent is Horde Forces). Returns `0` if the faction
// is top-level (no parent), or `nil` if the factionID is invalid.
//
// Reads `Faction.dbc` `ParentFactionID` at `+0x48`. Modern WoW returns
// this as the 13th value of `GetFactionInfoByID`; we expose it as its
// own getter since 1.12's `GetFactionInfo` doesn't have the slot.
static int __fastcall Script_GetFactionParentID(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L, "Usage: GetFactionParentID(factionID)");
        return 0;
    }
    const int factionID = static_cast<int>(Game::Lua::ToNumber(L, 1));
    const uint8_t *record = FactionRecord(factionID);
    if (record == nullptr)
        return 0;
    const int parent = *reinterpret_cast<const int *>(
        record + Offsets::OFF_FACTION_PARENT_ID);
    Game::Lua::PushNumber(L, static_cast<double>(parent));
    return 1;
}

// Returns the rep-slot pointer for `repListID`, or nullptr if out of
// range. Slot layout is documented at `VAR_PLAYER_REP_SLOTS` in
// `Offsets.h`. Used by `GetFactionStandings`'s direct slot-array walk.
const uint8_t *RepSlot(int repListID) {
    if (repListID < 0 || repListID >= Offsets::MAX_REP_SLOTS)
        return nullptr;
    return reinterpret_cast<const uint8_t *>(
        static_cast<uintptr_t>(Offsets::VAR_PLAYER_REP_SLOTS) +
        static_cast<uintptr_t>(repListID) * Offsets::REP_SLOT_STRIDE);
}

// `C_Reputation.GetFactionStandings()` — returns a flat
// `{ [factionID] = currentStanding }` table covering every faction
// in the player's reputation list (i.e. every populated rep slot).
//
// `currentStanding` is `base + delta` from the rep slot — same value
// `FUN_REPUTATION_GET_STANDING` returns, same as the `barValue` /
// `currentStanding` field in `GetFactionInfo` / `GetWatchedFactionData`.
//
// Unlike a displayed-list walk (`GetNumFactions` + `GetFactionInfo`),
// this skips the header rows entirely and doesn't depend on the
// player having opened the reputation pane recently — it reads
// straight out of the per-faction rep-slot array, which the engine
// keeps populated for every faction the player has rep with.
//
// Always returns a table (possibly empty); never nil.
static int __fastcall Script_C_Reputation_GetFactionStandings(void *L) {
    Game::Lua::SetTop(L, 0);
    Game::Lua::NewTable(L);

    for (int i = 0; i < Offsets::MAX_REP_SLOTS; i++) {
        const uint8_t *slot = RepSlot(i);
        if (slot == nullptr)
            continue;
        const int factionID = *reinterpret_cast<const int *>(
            slot + Offsets::OFF_REP_SLOT_FACTION_ID);
        if (factionID <= 0)
            continue;
        const int base = *reinterpret_cast<const int *>(
            slot + Offsets::OFF_REP_SLOT_BASE_STANDING);
        const int delta = *reinterpret_cast<const int *>(
            slot + Offsets::OFF_REP_SLOT_DELTA_STANDING);
        Game::Lua::PushNumber(L, static_cast<double>(factionID));
        Game::Lua::PushNumber(L, static_cast<double>(base + delta));
        Game::Lua::SetTable(L, -3);
    }
    return 1;
}

// `C_Reputation.GetWatchedFactionData()` — modern-style table for
// the faction shown above the XP bar, or nil when no faction is
// watched. Engine-direct: resolves the watched repListID off the
// player, finds the slot's factionID, and runs the shared
// `ReadFactionData` chain. `isWatched` is forced true since this
// IS the watched faction by definition.
static int __fastcall Script_C_Reputation_GetWatchedFactionData(void *L) {
    const uint8_t *info = PlayerInfo();
    if (info == nullptr)
        return 0;
    const uint8_t *slot = RepSlot(WatchedRepListID(info));
    if (slot == nullptr)
        return 0;
    const int factionID = *reinterpret_cast<const int *>(
        slot + Offsets::OFF_REP_SLOT_FACTION_ID);

    FactionData d;
    if (!ReadFactionData(factionID, &d))
        return 0;
    d.isWatched = true;

    Game::Lua::SetTop(L, 0);
    PushFactionDataTable(L, d);
    return 1;
}

// `C_Reputation.GetFactionDataByIndex(factionSortIndex)` — modern
// table-shaped accessor over the displayed reputation list. 1-based
// index covering the same range as vanilla's `GetFactionInfo(index)`
// (real factions + category header rows). Returns nil for OOB or
// for the "Other"/"Inactive" pseudo-rows that don't have a
// `Faction.dbc` record.
//
// Engine-direct: resolves index → factionID, then runs the shared
// `ReadFactionData` chain (no Lua round-trip through
// `Script_GetFactionInfo`).
static int __fastcall Script_C_Reputation_GetFactionDataByIndex(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L,
            "Usage: C_Reputation.GetFactionDataByIndex(factionSortIndex)");
        return 0;
    }
    const int idx = static_cast<int>(Game::Lua::ToNumber(L, 1)) - 1;
    if (idx < 0)
        return 0;
    const int maxIdx = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_FACTION_VISIBLE_MAX_INDEX));
    if (idx > maxIdx)
        return 0;

    FactionData d;
    if (!ReadFactionData(Resolver()(idx), &d))
        return 0;

    Game::Lua::SetTop(L, 0);
    PushFactionDataTable(L, d);
    return 1;
}

// `C_Reputation.SetWatchedFactionByID(factionID)` — sets the faction
// shown above the XP bar by ID rather than by displayed-list index.
// Modern API; vanilla 1.12 only exposes `SetWatchedFactionIndex(idx)`,
// which forces addons to walk the index list themselves.
//
// Calls the engine's inner watched-faction setter at
// `FUN_PLAYER_SET_WATCHED_FACTION` directly, bypassing the
// engine's index-based wrapper (which round-trips through the
// resolver and rejects unencountered factions). Passing factionID
// 0 clears the watched faction.
//
// Negative IDs are silently ignored.
static int __fastcall Script_C_Reputation_SetWatchedFactionByID(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L,
            "Usage: C_Reputation.SetWatchedFactionByID(factionID)");
        return 0;
    }
    const int factionID = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (factionID < 0)
        return 0;

    using SetWatched_t = void(__fastcall *)(int factionID);
    auto fn = reinterpret_cast<SetWatched_t>(
        Offsets::FUN_PLAYER_SET_WATCHED_FACTION);
    fn(factionID);
    return 0;
}

static void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("GetFactionIDByIndex", &Script_GetFactionIDByIndex);
    Game::Lua::RegisterGlobalFunction("GetFactionInfoByID", &Script_GetFactionInfoByID);
    Game::Lua::RegisterGlobalFunction("GetFactionParentID", &Script_GetFactionParentID);
    Game::Lua::RegisterTableFunction("C_Reputation", "SetWatchedFactionByID",
                                     &Script_C_Reputation_SetWatchedFactionByID);
    Game::Lua::RegisterTableFunction("C_Reputation", "GetWatchedFactionData",
                                     &Script_C_Reputation_GetWatchedFactionData);
    Game::Lua::RegisterTableFunction("C_Reputation", "GetFactionStandings",
                                     &Script_C_Reputation_GetFactionStandings);
    Game::Lua::RegisterTableFunction("C_Reputation", "GetFactionDataByIndex",
                                     &Script_C_Reputation_GetFactionDataByIndex);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Faction::Info
