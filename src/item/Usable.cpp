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

// `IsUsableItem(item)` / `C_Item.IsUsableItem(item)` — is the item's
// *on-use* ability usable by the local player right now, returning
// `(usable, noMana)`. This is the item analog of `IsUsableSpell`; it has
// nothing to do with whether the item can be *equipped* (armor
// proficiency / class-restricted gear is not this function's concern —
// that's the tooltip's red requirement lines, evaluated separately).
//
// Semantics (mirrors 3.3.5's `Script_IsUsableItem` structure, rebuilt on
// 1.12 primitives — vanilla ships no `IsUsableItem` and its item-action
// usability path never computed on-use power/noMana):
//   1. Item must have an on-use spell (a `SPELL_TRIGGER[i] == ON_USE(0)`
//      slot with a non-zero `SPELL_ID[i]`). No on-use ability → not
//      usable (there's nothing to "use").
//   2. The item's own *use* requirements must be met: RequiredLevel, and
//      the AllowableClass / AllowableRace masks. These gate activation
//      (e.g. a potion below your level, a class-restricted trinket) and
//      are use — not equip — requirements, so they belong here.
//   3. The on-use spell must be castable now: delegated to the engine's
//      own spell-castability helper `FUN_SPELL_IS_USABLE`, which folds in
//      form/stance, mechanic immunities, aura state, and the power check.
//      Insufficient power is the only condition that flips `noMana` (and
//      leaves `usable` false), exactly like `IsUsableSpell`.
//
// Cache-miss handling matches `C_Item.IsEquippableItem`: a sync peek with
// no load fired, returning `(false, false)` if the item isn't cached yet.

#include "Game.h"
#include "Offsets.h"
#include "item/Arg.h"
#include "item/Record.h"
#include "item/Spell.h"
#include "spell/Lookup.h"

#include <cstdint>

namespace Item::Usable {

namespace {

using SpellIsUsable_t = int(__fastcall *)(const uint8_t *spellRecord, int *outNoMana);

const uint8_t *PlayerDescriptor() {
    auto *player = static_cast<const uint8_t *>(Game::ResolveUnitToken("player"));
    if (player == nullptr)
        return nullptr;
    return *reinterpret_cast<const uint8_t *const *>(
        player + Offsets::OFF_UNIT_DESCRIPTOR);
}

// True iff `mask` restricts a 1-based `index` (class/race) the player
// isn't part of. Mask 0 and 0xFFFFFFFF both mean "no restriction".
bool RestrictedOut(uint32_t mask, uint32_t index1Based) {
    if (mask == 0 || mask == 0xFFFFFFFF || index1Based == 0)
        return false;
    return (mask & (1u << (index1Based - 1))) == 0;
}

struct Usability {
    bool usable;
    bool noMana;
};

Usability ComputeUsability(int itemID) {
    Usability r{false, false};
    if (itemID <= 0)
        return r;

    auto *record = Item::PeekRecord(static_cast<uint32_t>(itemID));
    if (record == nullptr)
        return r; // not cached → sync "false", no load fired

    const int onUseSpellID = Item::Spell::FindOnUseSpellIDInRecord(record);
    if (onUseSpellID == 0)
        return r; // no on-use ability → nothing to use

    auto *desc = PlayerDescriptor();
    if (desc == nullptr)
        return r; // pre-login / no player

    // Use requirements (NOT equip requirements): level + class/race.
    const int playerLevel = *reinterpret_cast<const int *>(
        desc + Offsets::OFF_UNIT_FIELD_LEVEL);
    const uint32_t reqLevel = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_REQUIRED_LEVEL);
    if (static_cast<int>(reqLevel) > playerLevel)
        return r;

    const uint32_t allowClass = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_ALLOWABLE_CLASS);
    const uint32_t playerClass = *(desc + Offsets::OFF_UNIT_DESCRIPTOR_CLASS_BYTE);
    if (RestrictedOut(allowClass, playerClass))
        return r;

    const uint32_t allowRace = *reinterpret_cast<const uint32_t *>(
        record + Offsets::OFF_ITEMSTATS_ALLOWABLE_RACE);
    const uint32_t playerRace = *(desc + Offsets::OFF_UNIT_DESCRIPTOR_RACE_BYTE);
    if (RestrictedOut(allowRace, playerRace))
        return r;

    // On-use spell castability + noMana, from the engine's own helper.
    // `::Spell::Lookup` (leading `::`) — inside `Item::Usable`, a bare
    // `Spell::` binds to the included `Item::Spell` namespace.
    auto *spellRec = ::Spell::Lookup::RecordForID(onUseSpellID);
    if (spellRec == nullptr)
        return r;

    auto isUsable = reinterpret_cast<SpellIsUsable_t>(Offsets::FUN_SPELL_IS_USABLE);
    int noMana = 0;
    const int usable = isUsable(spellRec, &noMana) & 0xFF;
    r.usable = usable != 0;
    r.noMana = noMana != 0;
    return r;
}

// `IsUsableItem(item)` — legacy global. Returns `(usable, noMana)` as
// 1/nil pairs, matching stock 1.12's `IsUsableAction` / our
// `IsUsableSpell` convention.
int __fastcall Script_IsUsableItem(void *L) {
    const int itemID = Item::Arg::ResolveItemID(L, 1);
    const Usability r = ComputeUsability(itemID);
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

// `C_Item.IsUsableItem(item)` — modern table-namespace form. Returns
// proper booleans (`usable`, `noMana`) per the `C_Item.*` convention.
int __fastcall Script_C_Item_IsUsableItem(void *L) {
    const int itemID = Item::Arg::ResolveItemID(L, 1);
    const Usability r = ComputeUsability(itemID);
    Game::Lua::PushBool(L, r.usable);
    Game::Lua::PushBool(L, r.noMana);
    return 2;
}

} // namespace

static void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("IsUsableItem", &Script_IsUsableItem);
    Game::Lua::RegisterTableFunction("C_Item", "IsUsableItem",
                                     &Script_C_Item_IsUsableItem);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Item::Usable
