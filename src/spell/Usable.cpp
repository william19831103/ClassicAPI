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
#include "item/CGItem.h"
#include "item/ID.h"
#include "item/Location.h"
#include "spell/Arg.h"
#include "spell/Lookup.h"

#include <cstdint>

namespace Spell::Usable {

namespace {

using GetSpellCost_t = uint32_t(__fastcall *)(int spellID, int unit);

// Spell.dbc record field offsets we read for usability. Fully documented
// in CLAUDE.md.

// Returns true if the player knows `spellID` per the engine's spell-
// knowledge bitmap at `[VAR_PLAYER_SPELL_BITMAP]`. Same check
// `IsPlayerSpell` (`src/spell/Info.cpp`) uses — covers trained
// abilities, talents, racials, profession recipes, etc. *Importantly*,
// covers spells that are **not** currently on an action bar — racials
// kept off the bar still register here.
bool PlayerKnowsSpell(int spellID) {
    if (spellID <= 0)
        return false;
    auto *bitmap = Game::Read<const uint32_t *>(
        static_cast<uintptr_t>(Offsets::VAR_PLAYER_SPELL_BITMAP));
    if (bitmap == nullptr)
        return false;
    const int spellCount = Game::Read<int>(
        static_cast<uintptr_t>(Offsets::VAR_SPELL_RECORD_COUNT));
    if (spellID > spellCount)
        return false;
    return (bitmap[spellID >> 5] & (1u << (spellID & 31))) != 0;
}

const uint8_t *PlayerDescriptor() {
    auto *player = static_cast<const uint8_t *>(Game::ResolveUnitToken("player"));
    if (player == nullptr)
        return nullptr;
    return Game::Read<const uint8_t *>(
        player, Offsets::OFF_UNIT_DESCRIPTOR);
}

// Calls the engine's cooldown helper at `FUN_SPELL_QUERY_COOLDOWN`
// for the player spellbook (bookType=0). Returns true if the spell
// has an active cooldown.
//
// `__fastcall(spellID, bookType, *duration, *start, *enable)` —
// duration is the engine's raw cooldown length in milliseconds (the
// Lua-side `Script_GetSpellCooldown` multiplies by 0.001 before
// pushing), start is the absolute engine tick count when the
// cooldown began. Both are 0 when no cooldown is active.
using QueryCooldown_t = void(__fastcall *)(int spellID, int bookType,
                                            int *outDuration,
                                            int *outStart,
                                            int *outEnable);

bool IsOnCooldown(int spellID) {
    auto fn = reinterpret_cast<QueryCooldown_t>(Offsets::FUN_SPELL_QUERY_COOLDOWN);
    int duration = 0, start = 0, enable = 0;
    fn(spellID, 0 /* bookType=player */, &duration, &start, &enable);
    return duration > 0;
}

// Walks player bags 0..4 counting items matching `targetItemID`.
// Returns the summed stack count. Same logic
// `Item::Count::CountInBag` uses; inlined here to avoid promoting
// it across modules just for one extra caller.
//
// Stomps the Lua stack — caller must own it. Stack contents on
// return are unspecified (caller should `SetTop` if needed).
int CountItemInBags(void *L, int targetItemID) {
    int total = 0;
    for (int bag = 0; bag <= 4; bag++) {
        const int slots = Item::Location::GetBagSlotCount(bag);
        for (int slot = 1; slot <= slots; slot++) {
            const uint8_t *item = Item::Location::ResolveBag(L, bag, slot);
            if (item == nullptr)
                continue;
            if (Item::ID::FromCGItem(item) != targetItemID)
                continue;
            auto *itemDesc = Item::ObjectFields(item);
            if (itemDesc == nullptr)
                continue;
            total += static_cast<int>(Game::Read<uint32_t>(
                itemDesc, Offsets::OFF_DESCRIPTOR_STACK_COUNT));
        }
    }
    return total;
}

// Returns true iff the player has enough of every reagent the spell
// requires (Reagent[i] > 0 with corresponding ReagentCount[i] > 0).
// Spells with zero reagents trivially pass.
bool HasReagents(void *L, const uint8_t *record) {
    for (int i = 0; i < Offsets::SPELL_MAX_REAGENTS; i++) {
        const int reagentItemID = static_cast<int>(Game::Read<int32_t>(
            record, Offsets::OFF_SPELL_REAGENT_ID + i * 4));
        const int reagentCount = static_cast<int>(Game::Read<int32_t>(
            record, Offsets::OFF_SPELL_REAGENT_COUNT + i * 4));
        if (reagentItemID <= 0 || reagentCount <= 0)
            continue;
        if (CountItemInBags(L, reagentItemID) < reagentCount)
            return false;
    }
    return true;
}

// Computes (usable, noMana) for the local player.
//
// Checks performed (in order, short-circuiting on first failure):
//   1. Spell is known (engine bitmap — covers all sources: trained,
//      talents, racials, profession recipes).
//   2. Spell record exists in Spell.dbc.
//   3. Player descriptor is reachable (post-login).
//   4. Player is alive (HEALTH > 0).
//   5. Spell is not on cooldown (engine cooldown helper).
//   6. Player has enough power for the spell's *effective* cost (talent
//      reductions applied) of its `PowerType`. Only this flips
//      `noMana=true`.
//   7. Player has all required reagents in bags (Reagent[8] /
//      ReagentCount[8] from the spell record).
//
// Not checked (deliberately — different concerns or post-vanilla
// concepts that don't apply): silence, GCD, stance/form, range,
// target type/validity, line-of-sight, casting state.
struct Usability {
    bool usable;
    bool noMana;
};

Usability ComputeUsability(void *L, int spellID) {
    Usability r{false, false};

    if (!PlayerKnowsSpell(spellID))
        return r;

    auto *record = Spell::Lookup::RecordForID(spellID);
    if (record == nullptr)
        return r;

    auto *desc = PlayerDescriptor();
    if (desc == nullptr)
        return r;

    const int health = Game::Read<int>(
        desc, Offsets::OFF_UNIT_FIELD_HEALTH);
    if (health <= 0)
        return r;

    if (IsOnCooldown(spellID))
        return r;

    // Mana check is the only one that flips noMana. Use the engine's
    // effective-cost helper (op-14 SpellMod + descriptor power-cost mods +
    // ManaCostPercent), so talent reductions like Frost Channeling count —
    // not just the base ManaCost. Falls back to base if the engine can't
    // resolve a cost (shouldn't happen here: the player descriptor
    // resolved above, so it has a player context).
    const int powerType = static_cast<int>(Game::Read<int8_t>(
        record, Offsets::OFF_SPELL_RECORD_POWER_TYPE));
    if (powerType >= 0 && powerType <= 4) {
        auto getCost = reinterpret_cast<GetSpellCost_t>(Offsets::FUN_GET_SPELL_COST);
        uint32_t cost = getCost(spellID, 0 /* local player */);
        if (cost == 0xFFFFFFFF)
            cost = Game::Read<uint32_t>(record, Offsets::OFF_SPELL_RECORD_MANA_COST);
        if (cost > 0) {
            const int currentPower = Game::Read<int>(
                desc, Offsets::OFF_UNIT_FIELD_POWER1 + powerType * 4);
            if (currentPower < static_cast<int>(cost)) {
                r.noMana = true;
                return r;
            }
        }
    }

    if (!HasReagents(L, record))
        return r;

    r.usable = true;
    return r;
}

// Same arg-shape resolver `Spell::Info::ResolveLuaArgsToSpellID` uses —
// duplicated here because it's file-static there. Accepts spellID
// (number) or (slot, bookType) for spellbook lookups.
int ResolveSpellArg(void *L) {
    if (!Game::Lua::IsNumber(L, 1))
        return 0;
    const int arg1 = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (Game::Lua::Type(L, 2) == Game::Lua::TYPE_STRING) {
        const char *book = Game::Lua::ToString(L, 2);
        const int bookType = static_cast<int>(
            book != nullptr &&
            (book[0] == 'p' || book[0] == 'P') &&
            (book[1] == 'e' || book[1] == 'E') &&
            (book[2] == 't' || book[2] == 'T') &&
            book[3] == 0);
        return Spell::Lookup::SpellbookSlotToID(arg1, bookType);
    }
    return arg1;
}

// `IsUsableSpell(spell)` / `IsUsableSpell(slot, bookType)` — vanilla-
// shaped legacy global. Returns `(usable, noMana)` as 1/nil pairs to
// match `Script_IsUsableAction`'s convention in stock 1.12.
int __fastcall Script_IsUsableSpell(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L, "Usage: IsUsableSpell(spellID) or IsUsableSpell(slot, bookType)");
        return 0;
    }
    const int spellID = ResolveSpellArg(L);
    Usability r = ComputeUsability(L, spellID);
    if (r.usable) {
        Game::Lua::PushNumber(L, 1.0);
        Game::Lua::PushNil(L);
    } else if (r.noMana) {
        Game::Lua::PushNil(L);
        Game::Lua::PushNumber(L, 1.0);
    } else {
        Game::Lua::PushNil(L);
        Game::Lua::PushNil(L);
    }
    return 2;
}

// `C_Spell.IsSpellUsable(spellID)` — modern table-namespace form.
// Returns proper booleans (`isUsable`, `insufficientPower`) per the
// `C_Spell.*` convention, not 1/nil pairs.
int __fastcall Script_C_Spell_IsSpellUsable(void *L) {
    const int spellID = Spell::Arg::ResolveSpellID(L, 1);
    Usability r = ComputeUsability(L, spellID);
    Game::Lua::PushBool(L, r.usable);
    Game::Lua::PushBool(L, r.noMana);
    return 2;
}

} // namespace

static void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("IsUsableSpell", &Script_IsUsableSpell);
    Game::Lua::RegisterTableFunction("C_Spell", "IsSpellUsable",
                                     &Script_C_Spell_IsSpellUsable);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Spell::Usable
