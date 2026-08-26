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

#include "item/TooltipItem.h"

#include "Game.h"
#include "Offsets.h"
#include "item/ID.h"
#include "item/Link.h"
#include "item/Location.h"
#include "unit/Flags.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Item::TooltipItem {

namespace {

struct VisibleItemState {
    const void *tooltip = nullptr;
    uint32_t itemID = 0;
    uint32_t property = 0;
    bool valid = false;
};

VisibleItemState g_visibleItem;
// The engine fires OnTooltipSetItem from inside the native item builder,
// before SetInventoryItem/SetHyperlink returns. Keep the captured value in a
// separate slot during that window; the common tooltip Clear hook must not
// erase it before the callback reads GameTooltip:GetItem().
VisibleItemState g_pendingVisibleItem;

void PrepareForReload() {
    g_visibleItem = {};
    g_pendingVisibleItem = {};
}

const Game::ReloadAutoRegister _reloadReg{&PrepareForReload};

using ResolveUnitToken_t = void *(__fastcall *)(const char *token);
using GetVisibleItem_t = void *(__thiscall *)(void *unit, int slot0);
using SetInventoryItem_t = int(__fastcall *)(void *L);
using SetHyperlink_t = int(__fastcall *)(void *L);
using GetInventoryItemLink_t = int(__fastcall *)(void *L);

SetInventoryItem_t g_setInventoryItemOriginal = nullptr;
SetHyperlink_t g_setHyperlinkOriginal = nullptr;
GetInventoryItemLink_t g_getInventoryItemLinkOriginal = nullptr;

int CallNativeGetInventoryItemLink(void *L) {
    // Prefer the MinHook trampoline when the native entry was hooked. If
    // that hook is unavailable, call the original VA directly; this also
    // keeps the Lua-global replacement below usable on clients where the
    // native entry is not hookable.
    if (g_getInventoryItemLinkOriginal != nullptr)
        return g_getInventoryItemLinkOriginal(L);
    auto fn = reinterpret_cast<GetInventoryItemLink_t>(
        Offsets::FUN_SCRIPT_GET_INVENTORY_ITEM_LINK);
    return fn(L);
}

void *ResolveUnit(const char *token) {
    if (token == nullptr || *token == '\0')
        return nullptr;
    auto fn = reinterpret_cast<ResolveUnitToken_t>(
        Offsets::FUN_RESOLVE_UNIT_TOKEN);
    return fn(token);
}

bool ReadRemoteVisibleItem(const char *unitToken, int slot,
                           uint32_t *outItemID, uint32_t *outProperty) {
    if (outItemID != nullptr)
        *outItemID = 0;
    if (outProperty != nullptr)
        *outProperty = 0;
    if (unitToken == nullptr || slot < 1 || slot > 19)
        return false;

    void *unit = ResolveUnit(unitToken);
    if (unit == nullptr || !Unit::Flags::IsPlayerObject(
                                  static_cast<const uint8_t *>(unit)))
        return false;

    // Local inventory already has a live CGItem and uses the engine's native
    // link builder. This side channel is for another player's broadcast
    // visible-items array.
    if (unit == ResolveUnit("player"))
        return false;

    auto getVisible = reinterpret_cast<GetVisibleItem_t>(
        Offsets::FUN_UNIT_GET_VISIBLE_ITEM);
    auto *entry = static_cast<const uint8_t *>(getVisible(unit, slot - 1));
    if (entry == nullptr)
        return false;

    const uint32_t itemID = Game::Read<uint32_t>(
        entry, Offsets::OFF_VISIBLE_ITEM_ITEM_ID);
    if (itemID == 0)
        return false;

    if (outItemID != nullptr)
        *outItemID = itemID;
    if (outProperty != nullptr)
        *outProperty = Game::Read<uint32_t>(
            entry, Offsets::OFF_VISIBLE_ITEM_PROPERTIES);
    return true;
}

void CaptureVisibleItem(const void *tooltipObj, void *L) {
    if (tooltipObj == nullptr || L == nullptr)
        return;
    if (!Game::Lua::IsString(L, 2) || !Game::Lua::IsNumber(L, 3))
        return;

    const char *unitToken = Game::Lua::ToString(L, 2);
    const int slot = static_cast<int>(Game::Lua::ToNumber(L, 3));
    uint32_t itemID = 0;
    uint32_t property = 0;
    if (!ReadRemoteVisibleItem(unitToken, slot, &itemID, &property))
        return;

    g_visibleItem.tooltip = tooltipObj;
    g_visibleItem.itemID = itemID;
    g_visibleItem.property = property;
    g_visibleItem.valid = true;
}

bool ParseItemLinkProperty(const char *link, uint32_t *outItemID,
                           uint32_t *outProperty) {
    if (outItemID != nullptr)
        *outItemID = 0;
    if (outProperty != nullptr)
        *outProperty = 0;
    if (link == nullptr)
        return false;

    const char *item = std::strstr(link, "item:");
    if (item == nullptr)
        return false;
    item += 5;

    char *end = nullptr;
    const unsigned long itemID = std::strtoul(item, &end, 10);
    if (end == item || *end != ':')
        return false;

    const char *property = std::strchr(end + 1, ':');
    if (property == nullptr)
        return false;
    ++property;

    const unsigned long value = std::strtoul(property, &end, 10);
    if (end == property || (*end != ':' && *end != '|' && *end != '\0'))
        return false;

    if (outItemID != nullptr)
        *outItemID = static_cast<uint32_t>(itemID);
    if (outProperty != nullptr)
        *outProperty = static_cast<uint32_t>(value);
    return itemID != 0;
}

int __fastcall SetHyperlink_h(void *L) {
    const void *tooltipObj = nullptr;
    VisibleItemState captured;
    if (L != nullptr && Game::Lua::Type(L, 1) == Game::Lua::TYPE_TABLE) {
        tooltipObj = Game::Lua::ResolveObject(L, 1);
        if (tooltipObj != nullptr && Game::Lua::IsString(L, 2)) {
            const char *link = Game::Lua::ToString(L, 2);
            uint32_t itemID = 0;
            uint32_t property = 0;
            // Ordinary Blizzard suffix IDs are small and are already handled
            // correctly by the native path. The inspect workaround is
            // specifically the extended value above 16 bits.
            if (ParseItemLinkProperty(link, &itemID, &property) &&
                property > 0xFFFFu) {
                captured.tooltip = tooltipObj;
                captured.itemID = itemID;
                captured.property = property;
                captured.valid = true;
            }
        }
    }

    g_visibleItem = {};
    g_pendingVisibleItem = captured;
    const int result = g_setHyperlinkOriginal(L);

    // SetHyperlink clears the tooltip while parsing the link. Publish the
    // captured value after the original returns, while the pending copy was
    // already available to OnTooltipSetItem during the native build.
    g_pendingVisibleItem = {};
    g_visibleItem = {};
    if (captured.valid)
        g_visibleItem = captured;
    return result;
}

const Game::HookAutoRegister _setHyperlinkHook{
    Offsets::FUN_SCRIPT_GAMETOOLTIP_SET_HYPERLINK,
    reinterpret_cast<void *>(&SetHyperlink_h),
    reinterpret_cast<void **>(&g_setHyperlinkOriginal)};

int __fastcall GetInventoryItemLink_h(void *L) {
    if (L != nullptr && Game::Lua::IsString(L, 1) &&
        Game::Lua::IsNumber(L, 2)) {
        const char *unitToken = Game::Lua::ToString(L, 1);
        const int slot = static_cast<int>(Game::Lua::ToNumber(L, 2));
        uint32_t itemID = 0;
        uint32_t property = 0;
        if (ReadRemoteVisibleItem(unitToken, slot, &itemID, &property)) {
            char link[256];
            if (Item::Link::BasicFromIDProperty(itemID, property,
                                                link, sizeof(link))) {
                Game::Lua::PushString(L, link);
                return 1;
            }
        }
    }
    return CallNativeGetInventoryItemLink(L);
}

const Game::HookAutoRegister _getInventoryItemLinkHook{
    Offsets::FUN_SCRIPT_GET_INVENTORY_ITEM_LINK,
    reinterpret_cast<void *>(&GetInventoryItemLink_h),
    reinterpret_cast<void **>(&g_getInventoryItemLinkOriginal)};

// Some UI paths retain the C closure that was installed in the Lua global
// table, rather than reaching the native VA through a normal call site. Put
// the same wrapper into the global table after the engine has rebuilt its
// script functions. The native hook above remains useful for callers that
// already cached the original closure.
void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("GetInventoryItemLink",
                                     &GetInventoryItemLink_h);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

int __fastcall SetInventoryItem_h(void *L) {
    const void *tooltipObj = nullptr;
    if (L != nullptr && Game::Lua::Type(L, 1) == Game::Lua::TYPE_TABLE)
        tooltipObj = Game::Lua::ResolveObject(L, 1);

    // The original enters the common tooltip-clear path. Capture the
    // visible entry first and expose it through the pending slot while the
    // native builder is running; ClearItemGuid leaves that slot intact.
    VisibleItemState captured;
    if (tooltipObj != nullptr) {
        g_visibleItem = {};
        CaptureVisibleItem(tooltipObj, L);
        captured = g_visibleItem;
        g_visibleItem = {};
    }

    g_pendingVisibleItem = captured;
    const int result = g_setInventoryItemOriginal(L);

    g_pendingVisibleItem = {};
    g_visibleItem = {};
    if (captured.valid)
        g_visibleItem = captured;
    return result;
}

const Game::HookAutoRegister _setInventoryItemHook{
    Offsets::FUN_SCRIPT_GAMETOOLTIP_SET_INVENTORY_ITEM,
    reinterpret_cast<void *>(&SetInventoryItem_h),
    reinterpret_cast<void **>(&g_setInventoryItemOriginal)};

} // namespace

int CurrentID(const void *tooltipObj, uint64_t *outGuid) {
    if (outGuid != nullptr)
        *outGuid = 0;
    if (tooltipObj == nullptr)
        return 0;
    auto *c = static_cast<const uint8_t *>(tooltipObj);

    const uint64_t itemGuid =
        Game::Read<uint64_t>(c, Offsets::OFF_TOOLTIP_ITEM_GUID_LO);

    // Link item: the itemID field is authoritative (and reliably cleared).
    const int linkItemID = Game::Read<int>(c, Offsets::OFF_TOOLTIP_ITEM_ID);
    if (linkItemID > 0) {
        if (outGuid != nullptr)
            *outGuid = itemGuid;
        return linkItemID;
    }

    // CGItem item: only the GUID is stored, and +0x380 isn't zeroed by the
    // clear — so it lingers after the tooltip switches to a unit / GO /
    // spell. Each of those sets its own (cleared-then-written) id, so trust
    // the item GUID only when none of them is currently shown, then resolve
    // it to the itemID (this also yields the dressed link for callers).
    if (itemGuid == 0)
        return 0;
    if (Game::Read<uint64_t>(c, Offsets::OFF_TOOLTIP_UNIT_GUID_LO) != 0)
        return 0;
    if (Game::Read<uint64_t>(c, Offsets::OFF_TOOLTIP_GAMEOBJECT_GUID_LO) != 0)
        return 0;
    if (Game::Read<int>(c, Offsets::OFF_TOOLTIP_SPELL_ID) > 0)
        return 0;

    const uint8_t *cg = Item::Location::ResolveByGUID(itemGuid);
    if (cg == nullptr)
        return 0;
    const int id = Item::ID::FromCGItem(cg);
    if (id <= 0)
        return 0;
    if (outGuid != nullptr)
        *outGuid = itemGuid;
    return id;
}

bool CurrentVisibleItem(const void *tooltipObj, uint32_t *outItemID,
                        uint32_t *outProperty) {
    if (outItemID != nullptr)
        *outItemID = 0;
    if (outProperty != nullptr)
        *outProperty = 0;
    const VisibleItemState *state = &g_visibleItem;
    if (tooltipObj == nullptr ||
        (!state->valid || state->tooltip != tooltipObj)) {
        state = &g_pendingVisibleItem;
    }
    if (!state->valid || state->tooltip != tooltipObj)
        return false;
    if (outItemID != nullptr)
        *outItemID = state->itemID;
    if (outProperty != nullptr)
        *outProperty = state->property;
    return true;
}

void ClearVisibleItem(const void *tooltipObj) {
    if (tooltipObj == nullptr || g_visibleItem.tooltip == tooltipObj)
        g_visibleItem = {};
}

} // namespace Item::TooltipItem
