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

// Completes the per-tooltip clear so the item GUID doesn't go stale.
//
// The engine's tooltip clear FUN_GAMETOOLTIP_CLEAR (FUN_00530050) zeroes the
// unit (+0x368), gameobject (+0x370), itemID (+0x398) and spell (+0x39c)
// identity fields — but NOT the item GUID (+0x380/+0x384) that the item builder
// writes for CGItem tooltips (SetBagItem / SetInventoryItem / …). The engine
// never reads that GUID as "current item" (the builder always overwrites it
// before use), so leaving it stale is harmless to the engine — but
// GameTooltip:GetItem / HasItem / IsEquippedItem read it (via
// Item::TooltipItem::CurrentID's GUID fallback, which exists because CGItem
// tooltips leave the itemID field 0). So after the tooltip switches to content
// that sets no item — SetUnitAura (→ FUN_0052f880), SetEquipmentSet (→ SetText),
// etc. — the stale GUID made GetItem resolve the *previous* item.
//
// All 14 Set*/builder paths route through this clear (verified via its xrefs),
// and the item builder calls it *before* writing the GUID, so co-hooking the
// clear to also zero +0x380/+0x384 fixes every case uniformly: a fresh tooltip
// always resets the item GUID, and only a real item build re-sets it. The hook
// runs no Lua and touches nothing but those 8 bytes after the original clear —
// none of the fragility of the OnTooltipSetItem fire.

#include "Game.h"
#include "Offsets.h"
#include "item/TooltipItem.h"

#include <cstdint>

namespace Tooltip::ClearItemGuid {

namespace {

using Clear_t = void(__fastcall *)(void *self);
Clear_t g_clearOriginal = nullptr;

void __fastcall Clear_h(void *self) {
    g_clearOriginal(self);
    if (self != nullptr)
        *reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(self) +
                                      Offsets::OFF_TOOLTIP_ITEM_GUID_LO) = 0;
    Item::TooltipItem::ClearVisibleItem(self);
}

static const Game::HookAutoRegister _hook{
    Offsets::FUN_GAMETOOLTIP_CLEAR,
    reinterpret_cast<void *>(&Clear_h),
    reinterpret_cast<void **>(&g_clearOriginal)};

} // namespace

} // namespace Tooltip::ClearItemGuid
