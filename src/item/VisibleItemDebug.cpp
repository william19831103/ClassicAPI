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
#include "unit/Flags.h"

#include <cstdint>

namespace Item::VisibleItemDebug {

namespace {

using ResolveUnitToken_t = void *(__fastcall *)(const char *token);
using GetVisibleItem_t = void *(__thiscall *)(void *unit, int slot0);

// Diagnostic: dump the raw 0x30-byte "visible item" entry the engine keeps
// per equipment slot on a remote/inspected unit (`[unit + 0xE68] + 0x118 +
// slot*0x30`, walked by FUN_UNIT_GET_VISIBLE_ITEM). This is what the item
// link for another player's equipment is built from, so dumping it tells us
// whether the 32-bit random property was truncated BEFORE it got stored
// (entry already holds the low-16-bits value) or only when the link builder
// reads it back (entry holds the full 32 bits).
//
// Returns a table:
//   {
//     unit    = <token>,
//     slot    = <1-based>,
//     itemID  = <uint32 @ +0x08>,
//     dwords  = { d0, d1, ..., d11 },   // the 0x30 bytes as 12 little-endian u32
//   }
// or nothing for a non-player unit / unresolved token / empty slot.
//
// Note: this reads the *broadcast* visible-items array, so it is only
// meaningful for player objects (gate on IsPlayerObject, never an NPC —
// the +0xE68 sub-struct is uninitialized there and the helper AVs).
int __fastcall Script_GetVisibleItemRaw(void *L) {
    if (!Game::Lua::IsString(L, 1) || !Game::Lua::IsNumber(L, 2)) {
        Game::Lua::Error(L, "Usage: C_Item.GetVisibleItemRaw(unit, slot)");
        return 0;
    }
    const char *token = Game::Lua::ToString(L, 1);
    const int slot = static_cast<int>(Game::Lua::ToNumber(L, 2));
    if (token == nullptr || slot < 1)
        return 0;

    auto resolve = reinterpret_cast<ResolveUnitToken_t>(Offsets::FUN_RESOLVE_UNIT_TOKEN);
    auto *unit = static_cast<uint8_t *>(resolve(token));
    if (unit == nullptr)
        return 0;
    if (!Unit::Flags::IsPlayerObject(unit))
        return 0;

    auto getVisible = reinterpret_cast<GetVisibleItem_t>(
        Offsets::FUN_UNIT_GET_VISIBLE_ITEM);
    auto *entry = static_cast<uint8_t *>(getVisible(unit, slot - 1));
    if (entry == nullptr)
        return 0;

    const uint32_t itemID = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_VISIBLE_ITEM_ITEM_ID);

    // 0x30 = 48 bytes = 12 u32. Give the raw dwords so the caller can eyeball
    // where (and at what width) the random property landed.
    Game::Lua::NewTable(L);                                    // result table
    Game::Lua::SetFieldString(L, "unit", token);
    Game::Lua::SetFieldNumber(L, "slot", static_cast<double>(slot));
    Game::Lua::SetFieldNumber(L, "itemID", static_cast<double>(itemID));

    Game::Lua::NewTable(L);                                    // dwords sub-table
    for (int i = 0; i < 12; ++i) {
        const uint32_t d = *reinterpret_cast<const uint32_t *>(
            entry + static_cast<size_t>(i) * sizeof(uint32_t));
        Game::Lua::PushNumber(L, static_cast<double>(i));      // numeric key
        Game::Lua::PushNumber(L, static_cast<double>(d));
        Game::Lua::RawSet(L, -3);                              // dwords[i] = d
    }
    // result["dwords"] = dwords. lua_settable/rawset expect [table, key, value]
    // with value on top; popping value then key. We have [result, dwords], so
    // insert the key *below* the dwords value before the rawset.
    Game::Lua::PushString(L, "dwords");   // [result, dwords, "dwords"]
    Game::Lua::Insert(L, -2);            // [result, "dwords", dwords] (key below value)
    Game::Lua::RawSet(L, -3);            // result["dwords"] = dwords

    return 1;
}

} // namespace

static void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Item", "GetVisibleItemRaw", &Script_GetVisibleItemRaw);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Item::VisibleItemDebug
