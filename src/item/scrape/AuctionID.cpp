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
#include "item/Link.h"

#include <cstdint>

namespace Item::AuctionID {

namespace {

// Three independent storage arrays. The engine resolves a list-type
// string ("list" / "owner" / "bidder") to one of these triples at
// runtime — we do the same.
struct ListSlot {
    const char *literalLowercase;
    uintptr_t entries;
    uintptr_t count;
};

constexpr ListSlot kLists[] = {
    {"list",   Offsets::VAR_AUCTION_LIST_ENTRIES,   Offsets::VAR_AUCTION_LIST_COUNT},
    {"owner",  Offsets::VAR_AUCTION_OWNER_ENTRIES,  Offsets::VAR_AUCTION_OWNER_COUNT},
    {"bidder", Offsets::VAR_AUCTION_BIDDER_ENTRIES, Offsets::VAR_AUCTION_BIDDER_COUNT},
};

bool EqualsIgnoreCase(const char *s, const char *literalLowercase) {
    for (;; ++s, ++literalLowercase) {
        const unsigned char a = static_cast<unsigned char>(*s);
        const unsigned char b = static_cast<unsigned char>(*literalLowercase);
        const unsigned char aLower = (a >= 'A' && a <= 'Z') ? a + 32 : a;
        if (aLower != b)
            return false;
        if (b == 0)
            return true;
    }
}

const ListSlot *FindList(const char *type) {
    if (type == nullptr || *type == '\0')
        return nullptr;
    for (const auto &slot : kLists) {
        if (EqualsIgnoreCase(type, slot.literalLowercase))
            return &slot;
    }
    return nullptr;
}

} // namespace

// `GetAuctionItemID(type, index)` — itemID companion to
// `GetAuctionItemLink`. `type` is one of `"list"` (the browse view),
// `"owner"` (your active sells), or `"bidder"` (auctions you're
// bidding on). nil for unknown type, OOR index, or empty / not-yet-
// loaded slot.
static int __fastcall Script_GetAuctionItemID(void *L) {
    if (!Game::Lua::IsString(L, 1) || !Game::Lua::IsNumber(L, 2)) {
        Game::Lua::Error(L, "Usage: GetAuctionItemID(\"list\"|\"owner\"|\"bidder\", index)");
        return 0;
    }
    const ListSlot *list = FindList(Game::Lua::ToString(L, 1));
    if (list == nullptr)
        return 0;
    const int index = static_cast<int>(Game::Lua::ToNumber(L, 2)) - 1;
    if (index < 0)
        return 0;

    const int count = static_cast<int>(*reinterpret_cast<const uint32_t *>(list->count));
    if (index >= count)
        return 0;

    auto *entries = reinterpret_cast<const uint8_t *const *>(list->entries);
    auto *entry = entries[index];
    if (entry == nullptr)
        return 0;

    const int itemID = static_cast<int>(*reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_AUCTION_ENTRY_ITEM_ID));
    if (itemID == 0)
        return 0;

    Game::Lua::PushNumber(L, static_cast<double>(itemID));
    return 1;
}

// `GetAuctionItemLink(type, index)` with the realm's GUIDLow carried in the
// auction result's suffix-factor field. The native client link builder drops
// that field, so rebuild the link with it as the fourth item-link field.
static int __fastcall Script_GetAuctionItemLink(void *L) {
    if (!Game::Lua::IsString(L, 1) || !Game::Lua::IsNumber(L, 2)) {
        Game::Lua::Error(L, "Usage: GetAuctionItemLink(\"list\"|\"owner\"|\"bidder\", index)");
        return 0;
    }
    const ListSlot *list = FindList(Game::Lua::ToString(L, 1));
    if (list == nullptr)
        return 0;
    const int index = static_cast<int>(Game::Lua::ToNumber(L, 2)) - 1;
    if (index < 0)
        return 0;

    const int count = static_cast<int>(*reinterpret_cast<const uint32_t *>(list->count));
    if (index >= count)
        return 0;
    auto *entries = reinterpret_cast<const uint8_t *const *>(list->entries);
    const uint8_t *entry = entries[index];
    if (entry == nullptr)
        return 0;

    const uint32_t itemID = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_AUCTION_ENTRY_ITEM_ID);
    if (itemID == 0)
        return 0;
    const uint32_t enchantID = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_AUCTION_ENTRY_ENCHANT_ID);
    const uint32_t property = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_AUCTION_ENTRY_RANDOM_PROPERTY);
    const uint32_t uniqueID = *reinterpret_cast<const uint32_t *>(
        entry + Offsets::OFF_AUCTION_ENTRY_UNIQUE_ID);

    char link[256];
    if (!Item::Link::BasicFromIDEnchantPropertyUnique(
            itemID, enchantID, property, uniqueID, link, sizeof(link)))
        return 0;
    Game::Lua::PushString(L, link);
    return 1;
}

static void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("GetAuctionItemID", &Script_GetAuctionItemID);
    Game::Lua::RegisterGlobalFunction("GetAuctionItemLink", &Script_GetAuctionItemLink);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Item::AuctionID
