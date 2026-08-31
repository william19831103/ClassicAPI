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

namespace Quest::Log {

namespace {

// Walks the unit's `+0xE68` sub-struct quest list (20 slots, stride
// 0xC) and returns true on first match. Mirrors the engine's loop in
// `Script_IsUnitOnQuest` (`0x004DFE10`).
bool UnitHasQuest(const uint8_t *unit, int target) {
    // The quest list lives in the CGPlayer sub-struct at +0xE68, which
    // only exists on real player objects. Gate on the object being a
    // player, not merely player-controlled — a pet/totem/MC'd creature
    // sets UNIT_FLAG_PLAYER_CONTROLLED but has no sub-struct there, so
    // IsPlayerControlled would deref garbage and crash (same root cause
    // as pfUI issue #34).
    if (target <= 0 || !Unit::Flags::IsPlayerObject(unit))
        return false;

    auto *info = *reinterpret_cast<const uint8_t *const *>(
        unit + Offsets::OFF_CGPLAYER_INFO);
    if (info == nullptr)
        return false;

    auto *base = info + Offsets::OFF_CGPLAYER_INFO_QUEST_LIST;
    for (int i = 0; i < Offsets::CGPLAYER_INFO_QUEST_LIST_MAX; ++i) {
        const int questID = *reinterpret_cast<const int *>(
            base + i * Offsets::CGPLAYER_INFO_QUEST_LIST_STRIDE);
        if (questID == target)
            return true;
    }
    return false;
}

} // namespace

static int __fastcall Script_GetQuestIDForLogIndex(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L, "Usage: GetQuestIDForLogIndex(index)");
        return 0;
    }

    const int idx = static_cast<int>(Game::Lua::ToNumber(L, 1)) - 1;
    const int total = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRY_COUNT));
    if (idx < 0 || idx >= total)
        return 0; // nil for out-of-range

    auto *entry = reinterpret_cast<uint8_t *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRIES)) + idx * 16;

    // Field +8 is the header indicator: non-NULL = header, NULL = real quest.
    // Verified by Script_GetQuestLogTitle's isHeader push at 0x004DF9A9 (it pushes
    // 1.0 when [+8] != 0) and by the helper at 0x004DF150 used by IsUnitOnQuest
    // (returns the +0 questID only when [+8] == 0, NULL otherwise).
    if (*reinterpret_cast<void *const *>(entry + 8) != nullptr) {
        Game::Lua::PushNumber(L, 0.0);
        return 1;
    }

    const int questID = *reinterpret_cast<const int *>(entry + 0);
    Game::Lua::PushNumber(L, static_cast<double>(questID));
    return 1;
}

// `C_QuestLog.GetLogIndexForQuestID(questID)` — the inverse of
// `GetQuestIDForLogIndex`: the 1-based quest-log index of `questID`, or nil
// if the quest isn't in the log. The index spans the full entry array
// (headers included), matching `GetQuestIDForLogIndex` /
// `GetQuestLogTitle`. Walks `VAR_QUEST_LOG_ENTRIES`, skipping header rows via
// the `+8` header-pointer gate, and returns the first matching entry's index.
static int __fastcall Script_GetLogIndexForQuestID(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L, "Usage: GetLogIndexForQuestID(questID)");
        return 0;
    }
    const int target = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (target <= 0)
        return 0; // nil — no such quest

    const int total = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRY_COUNT));
    auto *base = reinterpret_cast<const uint8_t *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRIES));
    for (int i = 0; i < total; ++i) {
        auto *entry = base + i * Offsets::OFF_QUEST_LOG_ENTRY_STRIDE;
        if (*reinterpret_cast<const void *const *>(
                entry + Offsets::OFF_QUEST_LOG_ENTRY_HEADER_PTR) != nullptr)
            continue; // header row
        const int questID = *reinterpret_cast<const int *>(
            entry + Offsets::OFF_QUEST_LOG_ENTRY_QUEST_ID);
        if (questID == target) {
            Game::Lua::PushNumber(L, static_cast<double>(i + 1)); // 1-based
            return 1;
        }
    }
    return 0; // nil — quest not in the log
}

// `C_QuestLog.GetHeaderIndexForQuest(questID)` — the 1-based log index of the
// collapsible category header (zone / "Dungeon" / class sort, …) the quest
// sits under, or nil if the quest isn't in the log or has no header above it.
// The log is laid out header-then-its-quests, so we locate the quest's entry
// and walk backwards to the nearest preceding header row (`+8` non-NULL).
static int __fastcall Script_GetHeaderIndexForQuest(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::Error(L, "Usage: GetHeaderIndexForQuest(questID)");
        return 0;
    }
    const int target = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (target <= 0)
        return 0; // nil — no such quest

    const int total = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRY_COUNT));
    auto *base = reinterpret_cast<const uint8_t *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRIES));

    // Locate the quest's entry.
    int questIndex = -1;
    for (int i = 0; i < total; ++i) {
        auto *entry = base + i * Offsets::OFF_QUEST_LOG_ENTRY_STRIDE;
        if (*reinterpret_cast<const void *const *>(
                entry + Offsets::OFF_QUEST_LOG_ENTRY_HEADER_PTR) != nullptr)
            continue; // header row
        if (*reinterpret_cast<const int *>(
                entry + Offsets::OFF_QUEST_LOG_ENTRY_QUEST_ID) == target) {
            questIndex = i;
            break;
        }
    }
    if (questIndex < 0)
        return 0; // quest not in the log

    // Walk back to the nearest preceding header.
    for (int i = questIndex - 1; i >= 0; --i) {
        auto *entry = base + i * Offsets::OFF_QUEST_LOG_ENTRY_STRIDE;
        if (*reinterpret_cast<const void *const *>(
                entry + Offsets::OFF_QUEST_LOG_ENTRY_HEADER_PTR) != nullptr) {
            Game::Lua::PushNumber(L, static_cast<double>(i + 1)); // 1-based
            return 1;
        }
    }
    return 0; // nil — quest sits above any header (shouldn't normally happen)
}

// `C_QuestLog.IsOnQuest(questID)` — true iff `questID` is currently
// in the player's quest log (incomplete OR ready-to-turn-in; the log
// holds both). Walks `VAR_QUEST_LOG_ENTRIES` and matches against each
// real entry's questID field. Headers are skipped via the `+8`
// header-pointer gate documented above.
//
// Returns `false` for non-positive or non-number input (no
// `lua_error` — modern semantics).
static int __fastcall Script_IsOnQuest(void *L) {
    if (!Game::Lua::IsNumber(L, 1)) {
        Game::Lua::PushBool(L, false);
        return 1;
    }
    const int target = static_cast<int>(Game::Lua::ToNumber(L, 1));
    if (target <= 0) {
        Game::Lua::PushBool(L, false);
        return 1;
    }

    const int total = *reinterpret_cast<const int *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRY_COUNT));
    auto *base = reinterpret_cast<const uint8_t *>(
        static_cast<uintptr_t>(Offsets::VAR_QUEST_LOG_ENTRIES));
    for (int i = 0; i < total; ++i) {
        auto *entry = base + i * Offsets::OFF_QUEST_LOG_ENTRY_STRIDE;
        if (*reinterpret_cast<const void *const *>(
                entry + Offsets::OFF_QUEST_LOG_ENTRY_HEADER_PTR) != nullptr) {
            continue; // header row
        }
        const int questID = *reinterpret_cast<const int *>(
            entry + Offsets::OFF_QUEST_LOG_ENTRY_QUEST_ID);
        if (questID == target) {
            Game::Lua::PushBool(L, true);
            return 1;
        }
    }
    Game::Lua::PushBool(L, false);
    return 1;
}

// `C_QuestLog.IsUnitOnQuest(unit, questID)` — true iff `unit` has
// `questID` in their quest list. Walks the unit's `+0xE68` quest
// sub-struct (`Script_IsUnitOnQuest`'s data source). For `"player"`
// equivalent to `IsOnQuest`; for other tokens (target / party / raid
// members) requires the unit to be in the engine's sync range so
// their quest data has been broadcast.
//
// Returns `false` for invalid input (non-string unit, non-number /
// non-positive questID, unresolvable token, or units missing the
// `+0xE68` sub-struct).
static int __fastcall Script_IsUnitOnQuest(void *L) {
    if (!Game::Lua::IsString(L, 1) || !Game::Lua::IsNumber(L, 2)) {
        Game::Lua::PushBool(L, false);
        return 1;
    }
    const char *token = Game::Lua::ToString(L, 1);
    const int target = static_cast<int>(Game::Lua::ToNumber(L, 2));
    if (token == nullptr || target <= 0) {
        Game::Lua::PushBool(L, false);
        return 1;
    }

    auto *unit = static_cast<const uint8_t *>(Game::ResolveUnitToken(token));
    Game::Lua::PushBool(L, UnitHasQuest(unit, target));
    return 1;
}

static void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_QuestLog", "GetQuestIDForLogIndex",
                                     &Script_GetQuestIDForLogIndex);
    Game::Lua::RegisterTableFunction("C_QuestLog", "GetLogIndexForQuestID",
                                     &Script_GetLogIndexForQuestID);
    Game::Lua::RegisterTableFunction("C_QuestLog", "GetHeaderIndexForQuest",
                                     &Script_GetHeaderIndexForQuest);
    Game::Lua::RegisterTableFunction("C_QuestLog", "IsOnQuest",
                                     &Script_IsOnQuest);
    Game::Lua::RegisterTableFunction("C_QuestLog", "IsUnitOnQuest",
                                     &Script_IsUnitOnQuest);
}

static const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace Quest::Log
