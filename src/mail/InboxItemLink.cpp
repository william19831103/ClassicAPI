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

// `GetInboxItemLink(messageIndex[, attachmentIndex])` — returns
// `(link, itemID)` for the item attached to inbox message
// `messageIndex` (1-based). Vanilla supports one attachment per message; the
// optional 1-based `attachmentIndex` exists for modern-signature
// parity and is no-op for any value other than 1.
//
// `itemID` as a second return is a ClassicAPI extension on top of
// modern's link-only signature — saves callers a `string.match` to
// parse it back out of the link.
//
// Inbox records retain enchant, random-property, and suffix-factor fields.
// This realm uses the suffix factor as the attachment item's GUIDLow, so the
// link preserves it in the fourth `item:` field for addon consumption.
//
// Returns nothing on out-of-range messageIndex, empty message slot,
// no attachment on the message, or attachmentIndex ≠ 1.

#include "Game.h"
#include "Offsets.h"
#include "item/Link.h"

#include <cstdint>

namespace Mail::InboxItemLink {

namespace {

int __fastcall Script_GetInboxItemLink(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L,
            "Usage: GetInboxItemLink(messageIndex[, attachmentIndex])");
        return 0;
    }
    const int oneBasedMsg = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (oneBasedMsg < 1)
        return 0;
    if (Game::Lua::IsNumber(L, 2)) {
        const int attach = static_cast<int>(Game::Lua::ToNumber(L, 2));
        if (attach != 1)
            return 0;
    }

    const int msgIdx = oneBasedMsg - 1;
    const int count = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_INBOX_COUNT));
    if (msgIdx >= count)
        return 0;

    // `VAR_INBOX_ENTRIES` is a pointer slot, not the array itself —
    // one extra deref to reach the entry-pointer array.
    auto **entries = *reinterpret_cast<const uint8_t ***>(
        Offsets::VAR_INBOX_ENTRIES);
    if (entries == nullptr)
        return 0;
    const uint8_t *entry = entries[msgIdx];
    if (entry == nullptr)
        return 0;

    const uint32_t itemID = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_INBOX_ENTRY_ITEM_ID);
    if (itemID == 0)
        return 0;

    const uint32_t enchantID = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_INBOX_ENTRY_ENCHANT_ID);
    const uint32_t property = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_INBOX_ENTRY_RANDOM_PROPERTY);
    const uint32_t uniqueID = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_INBOX_ENTRY_UNIQUE_ID);

    char buf[256];
    if (!Item::Link::BasicFromIDEnchantPropertyUnique(
            itemID, enchantID, property, uniqueID, buf, sizeof buf))
        return 0;
    Game::Lua::PushString(L, buf);
    Game::Lua::PushNumber(L, static_cast<double>(itemID));
    return 2;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("GetInboxItemLink",
                                       &Script_GetInboxItemLink);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace Mail::InboxItemLink
