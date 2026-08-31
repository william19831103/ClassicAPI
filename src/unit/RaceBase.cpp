// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.

#include "Game.h"
#include "Offsets.h"
#include "dbc/Names.h"
#include "unit/Identity.h"

#include <cstdint>

namespace Unit::RaceBase {

namespace {

// `UnitRaceBase(unit) → (raceFile, raceID)` — returns the locale-
// independent race token (`"Human"`, `"Orc"`, `"Dwarf"`, `"NightElf"`,
// `"Scourge"`, `"Tauren"`, `"Gnome"`, `"Troll"`) plus the numeric
// raceID (1..8 in vanilla), or `(nil, nil)` if the unit can't be
// resolved.
//
// Sibling to `UnitClassBase` — same descriptor-byte + DBC.Filename
// chain, just reading `UNIT_FIELD_BYTES_0` byte 0 (race) instead of
// byte 1 (class) and looking up `ChrRaces.dbc::Filename` at `+0x3C`
// (locale-independent string, distinct from the localized `Name[9]`
// at `+0x44` that `UnitRace` already returns).
//
// Race byte is broadcast in UpdateField data, so this works for any
// synced unit token (player, target, party / raid, mouseover,
// inspect targets — anything FUN_RESOLVE_UNIT_TOKEN handles).
//
// Returns `(nil, nil)` for unresolvable units (empty target /
// party slot, unloaded descriptor), unit-byte 0 (uninitialized /
// NPC), or out-of-range race bytes (no matching ChrRaces.dbc row).
int __fastcall Script_UnitRaceBase(void *L) {
    if (!Game::Lua::IsString(L, 1)) {
        Game::Lua::Error(L, "Usage: UnitRaceBase(\"unit\")");
        return 0;
    }
    const char *token = Game::Lua::ToString(L, 1);
    if (token == nullptr) {
        Game::Lua::PushNil(L);
        Game::Lua::PushNil(L);
        return 2;
    }

    // Mirror Script_UnitRace: the literal "player" token reads the
    // login-time race byte global (populated at character-enter, before
    // the in-world descriptor exists), so this resolves at addon-load
    // instead of returning nil until the player object spawns. All other
    // tokens go through the unit descriptor.
    uint8_t raceByte = 0;
    if (Unit::Identity::IsPlayerToken(token)) {
        raceByte = Game::Read<uint8_t>(Offsets::VAR_PLAYER_RACE_BYTE);
    } else {
        auto *unit = static_cast<const uint8_t *>(Game::ResolveUnitToken(token));
        if (unit != nullptr) {
            auto *desc =
                Game::Read<const uint8_t *>(unit, Offsets::OFF_UNIT_DESCRIPTOR);
            if (desc != nullptr)
                raceByte = *(desc + Offsets::OFF_UNIT_DESCRIPTOR_RACE_BYTE);
        }
        // No live descriptor (out of range / different map) → fall back to the
        // engine's player-info cache, matching what UnitRace itself does out of
        // range (race at OFF_PLAYER_INFO_RACE). `token` is already valid here —
        // resolve() raises on a bad token before this — so GuidForToken won't
        // raise.
        if (raceByte == 0) {
            const uint8_t *rec = Unit::Identity::PlayerInfoRecord(
                Unit::Identity::GuidForToken(token));
            if (rec != nullptr)
                raceByte = static_cast<uint8_t>(
                    Game::Read<uint32_t>(rec, Offsets::OFF_PLAYER_INFO_RACE));
        }
    }
    if (raceByte == 0) {
        Game::Lua::PushNil(L);
        Game::Lua::PushNil(L);
        return 2;
    }

    const char *raceFile = DBC::RaceToken(raceByte);
    if (raceFile == nullptr) {
        Game::Lua::PushNil(L);
        Game::Lua::PushNil(L);
        return 2;
    }
    Game::Lua::PushString(L, raceFile);
    Game::Lua::PushNumber(L, static_cast<double>(raceByte));
    return 2;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("UnitRaceBase", &Script_UnitRaceBase);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

} // namespace Unit::RaceBase
