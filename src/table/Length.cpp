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

// Stale table-length healing — Lua 5.1 border semantics for 5.0's
// out-of-band lengths.
//
// Lua 5.0 stores a table's `table.insert`/`getn` length OUT of band (a
// `t.n` field, else the registry's weak LUA_SIZES table). Clearing a
// table's keys (`for k in pairs(t) do t[k] = nil end`) does NOT reset it —
// 5.0 code must call `table.setn(t, 0)`. Lua 5.1 removed all of that:
// lengths are computed borders, and a cleared table is length 0
// automatically.
//
// That difference breaks the standard Ace2-era dual-compatibility idiom
// the moment our 5.1 syntax backport makes their probe pass:
//
//     local lua51 = loadstring("return function(...) return ... end")
//                       and true or false
//     local table_setn = lua51 and function() end or table.setn
//
// With `lua51` true (the `...`-expression now compiles via
// LuaSyntax::Transpile), `table_setn` is a NO-OP — correct on real 5.1,
// wrong on this VM, whose table functions still read the out-of-band
// length. Cleared-and-reused tables keep stale lengths: `table.insert`
// appends past the nil'd slots and `table.getn` counts entries that no
// longer exist. Verified live: Dewdrop-2.0's menus (the SuperAPI /
// NampowerSettings minimap buttons) break exactly this way, and work
// again when the probe is forced back to 5.0.
//
// Since we made the client PASS 5.1 probes, 5.1 length semantics must
// hold. The engine funnels every length read through ONE function —
// `luaL_getn` (LUAL_GETN, consulted by table.insert / getn / remove /
// concat / sort / foreachi, plus our `unpack`) — so a single co-hook
// closes the gap for every consumer at once:
//
//   1. Reported slot populated (`t[n] ~= nil`) → return n unchanged. The
//      healthy path costs one rawgeti.
//   2. `t[n]` nil but `t.n` is a number → return n unchanged. An explicit
//      `n` field is the 5.0 vararg-table contract (`arg` = {n=3, holes}),
//      where trailing nils are intentional — healing it would corrupt
//      vararg counts.
//   3. `t[n]` nil, no `t.n`: scan down for the true border `b`. Heal to `b`
//      ONLY when the stored length overshoots it by more than one slot
//      (`n - b > 1`) — a cleared-and-reused table (Dewdrop's menus) is stale
//      by many slots. When it overshoots by exactly one (`n - b == 1`), the
//      nil at slot `n` is a legitimate 5.0 append slot — `table.insert(t, nil)`
//      writes `t[n]=nil` and `setn(n)` — so keep the stored length; healing it
//      would make the NEXT append overwrite that slot and shift every element
//      after it down by one (issue #36: Ace2/Waterfall's `{key,val,…}` builders
//      that append a nil value corrupt silently). Read-only — no write-back,
//      so a length read never mutates state; the engine's own `table.insert`
//      calls `luaL_setn(n+1)` on the next append, which re-syncs the stored
//      length by itself.
//
// Why the gap test and not caller-gating: both Dewdrop (clear + rebuild via
// `table.insert`, Length.cpp's raison d'être) and Waterfall reach here THROUGH
// `table.insert`, so the caller can't tell them apart — only the staleness gap
// can. A cleared table is empty below `n` (large gap); a deliberate nil append
// is dense below `n` (gap 1). The gap is a heuristic (two consecutive nil
// appends would read as a cleared table), but consecutive nil appends are
// pathological, while single nil appends between real values are the common
// Ace2 idiom.
//
// `table.setn` itself still works for code that calls it — the heal only
// changes the answer when the stored length points more than one slot past
// the border, which is the same answer real 5.1 would give.

#include "Game.h"
#include "Offsets.h"

namespace Table::Length {

namespace {

using LuaLGetN_t = int(__fastcall *)(void *L, int idx);
using RawGetI_t = void(__fastcall *)(void *L, int idx, int n);

LuaLGetN_t g_getnOriginal = nullptr;

// True if t[k] (rawgeti) is nil. Balances the stack.
bool SlotIsNil(void *L, int absIdx, int k) {
    reinterpret_cast<RawGetI_t>(Offsets::LUA_RAWGETI)(L, absIdx, k);
    const bool isNil = Game::Lua::Type(L, -1) == Game::Lua::TYPE_NIL;
    Game::Lua::SetTop(L, -2);
    return isNil;
}

int __fastcall LuaLGetN_h(void *L, int idx) {
    const int n = g_getnOriginal(L, idx);
    if (n <= 0)
        return n;

    // Normalize a relative index once — the checks below push/pop around
    // the access, and the `t.n` probe has a key on the stack at its
    // access point, which would shift a negative index. Pseudo-indices
    // (registry and below, <= -10000) pass through unchanged.
    int absIdx = idx;
    if (idx < 0 && idx > -10000)
        absIdx = Game::Lua::GetTop(L) + idx + 1;

    if (!SlotIsNil(L, absIdx, n))
        return n; // healthy — the reported last slot is populated

    // Explicit `t.n` count (vararg-table contract): trailing nils are
    // intentional, keep the stored length.
    Game::Lua::PushString(L, "n");
    Game::Lua::RawGet(L, absIdx);
    const bool hasExplicitN = Game::Lua::Type(L, -1) == Game::Lua::TYPE_NUMBER;
    Game::Lua::SetTop(L, -2);
    if (hasExplicitN)
        return n;

    // Scan down for the true border.
    int b = n - 1;
    while (b > 0 && SlotIsNil(L, absIdx, b))
        --b;

    // Overshoots the border by exactly one slot: a legitimate 5.0 nil append
    // (`table.insert(t, nil)`) sits in a dense array, not a cleared table.
    // Keep the stored length so the next append lands after the nil, not on it.
    if (n - b <= 1)
        return n;

    // Stale by more than one slot — a cleared-and-reused table. Heal to the
    // border, the 5.1 answer.
    return b;
}

const Game::HookAutoRegister _hook{Offsets::LUAL_GETN,
                                   reinterpret_cast<void *>(&LuaLGetN_h),
                                   reinterpret_cast<void **>(&g_getnOriginal)};

} // namespace

} // namespace Table::Length
