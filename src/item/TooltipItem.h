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

#pragma once

#include <cstdint>

namespace Item::TooltipItem {

// The itemID a tooltip is currently displaying, or 0 when it isn't
// showing an item. Two storage paths:
//   - link paths (SetItemByID / SetHyperlink) set the itemID at
//     OFF_TOOLTIP_ITEM_ID (+0x398), which the per-tooltip clear zeroes;
//   - CGItem paths (SetInventoryItem / SetBagItem / SetItemByGUID) set the
//     instance GUID at OFF_TOOLTIP_ITEM_GUID_LO (+0x380), which the clear
//     does NOT zero — so it goes stale once the tooltip moves on to a unit
//     / gameobject / spell.
// We trust the itemID directly, and otherwise resolve the GUID — but only
// when no unit / gameobject / spell id is currently set (each of those IS
// cleared-and-set on show, so their presence means the lingering item GUID
// isn't the live content). `outGuid` (optional) receives the item GUID (0
// when none), which lets callers build the dressed (suffixed) link.
// Returns 0 for a null tooltip or any non-item tooltip.
//
// Note: auction tooltips (SetAuctionItem) stash the itemID in a separate
// descriptor (+0x3D0) with no CGItem/GUID, so no dressed link is
// recoverable from them — we deliberately don't surface those here rather
// than return a suffix-stripped link.
int CurrentID(const void *tooltipObj, uint64_t *outGuid);

// Returns the raw 32-bit Random Property captured from the most recent
// remote-player SetInventoryItem call for this tooltip. `outItemID` receives
// the matching visible item ID. The item ID check prevents a cached value
// from being applied to a different tooltip item.
bool CurrentVisibleItem(const void *tooltipObj, uint32_t *outItemID,
                        uint32_t *outProperty);

// Clears the per-tooltip visible-item side state. Called by the common
// tooltip clear hook and by SetInventoryItem before publishing a new value.
void ClearVisibleItem(const void *tooltipObj);

} // namespace Item::TooltipItem
