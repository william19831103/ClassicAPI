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

// `C_Map.GetMapOverlays([areaID])` — every WorldMapOverlay.dbc entry for
// a zone, regardless of exploration, with the tile grid fully resolved.
//
// The engine's own surface (GetNumMapOverlays / GetMapOverlayInfo) only
// exposes DISCOVERED overlays — the exploration gate is a server-fed
// list of discovered overlay IDs — which is why map addons (pfUI's
// mapreveal, Cartographer …) ship hand-measured overlay tables to draw
// unexplored areas. This reads the DBC directly instead: live data for
// every zone, on any client, no per-server table drift.
//
// With no argument, returns overlays for the world map's CURRENT view
// (mirroring the resolution the engine's GetMapInfo does: continent /
// zone selection → WorldMapArea row). With a numeric `areaID`
// (AreaTable id — the stable zone identity), resolves the WorldMapArea
// row whose areaID column matches.
//
// ## Why the tile grid is resolved here (Octo data quirks)
//
// An overlay's art is `<textureName>1.blp, 2, …` numbered row-major on a
// 256px grid, canonically `ceil(w/256) x ceil(h/256)` tiles with only
// the last column/row allowed partial. On stock 1.12 data that formula
// is always right; on Octo it is not, in three ways (established
// empirically by the InkLab map generator, mirrored here):
//
//   1. FOREIGN TILES: unrelated art continues an overlay's number
//      sequence in the same folder (Icepoint Rock's folder carries
//      `kaneqnuun` tiles, Hyjal carries Tel Abim art). Anything derived
//      from "what files exist" draws duplicate landmass. The overlay
//      list must come ONLY from the DBC (which this module guarantees),
//      and tiles past the true grid must be dropped.
//   2. SLIVER COLUMNS: the DBC rect can round away a narrow edge column
//      (kaneqnuun's real art has a third, 8px-wide column; its DBC `w`
//      yields 2 columns). Trusting ceil(w/256) both loses the sliver
//      and — worse — shears the row-major layout into diagonal strips.
//      The art itself reveals the truth: only a row's LAST tile may be
//      narrower than 256, so the first partial-width tile (searched one
//      past the DBC's column count) marks the real column count, and
//      the first partial-height tile marks the last row.
//   3. UPSCALED RE-EXPORTS: the same rect re-exported at higher
//      resolution (Stonetalon's WindshearCrag: 4 tiles for a 1-tile
//      rect). All tiles are real and must be kept, drawn scaled to the
//      rect. Only distinguishable from quirk 1 when the art has NO
//      partial edge anywhere AND the DBC rect is a whole multiple of
//      256 in both dimensions; the grid shape is then recovered from
//      the aspect ratio (cols ≈ round(sqrt(n·w/h))).
//
// Tile dimensions come from the BLP headers via the engine's own VFS
// reader (`FUN_FILE_READ`, so patch-MPQ overrides are honored). Both
// BLP1 and BLP2 keep width/height at header offsets +0x0C/+0x10.
// Resolution runs once per overlay per session (cached).
//
// ## HD map patches (uniform tile upscale)
//
// A custom `patch-*.mpq` can replace the overlay art with higher-res tiles
// (every 256px tile re-exported at 256*scale). Because we read the *file*
// dimensions, that upscale must not be assumed to be 256: the resolver
// detects the full-cell size `S` (the largest tile dimension = 256*scale)
// and expresses both the partial-tile threshold and the texcoords relative
// to it. Draw sizes/positions remain in logical map pixels (the patch changes
// texture resolution, not the map canvas), so an interior tile reads texcoord
// 1.0 at any scale and stock data (S == 256) is byte-identical to before.
// The upscale factor need not be integral; only a uniform per-overlay scale
// is assumed.
//
// ## Return shape
//
// Array of overlay tables:
//   textureName   — bare DBC name ("GOLDSHIRE")
//   texturePath   — "Interface\WorldMap\<dir>\GOLDSHIRE" (engine format)
//   areaID        — primary AreaTable id revealed (first nonzero)
//   areaIDs       — all AreaTable ids revealed (WorldMapOverlay.dbc
//                   areaID[4]; usually one, some overlays span 2-3 subzones)
//   textureWidth / textureHeight — the DBC placement rect (reliable for
//                   placement; its IMPLIED tile count is what lies)
//   offsetX / offsetY — placement on the zone canvas
//   mapPointX / mapPointY
//   hitRectTop/Left/Bottom/Right — hit rectangle in world-map canvas px
//                   (~1002x668); the tight clickable bounds of the landmass,
//                   the subzone-center source map addons scrape
//   tileCols / tileRows / upscaled — the resolved grid
//   tiles         — array of ready-to-draw tiles:
//                   { file      — texture path incl. number (SetTexture-ready)
//                     width, height        — draw size in map pixels
//                     texCoordX, texCoordY — right/bottom texcoords
//                     offsetX, offsetY }   — absolute canvas position
//   fileDataIDs   — ordered tile texture paths (retail's field name; vanilla
//                   has no numeric fileDataIDs, so these are paths). Redundant
//                   with tiles[].file — for retail-ported exploration addons.
//
// Texture-less rows (a subzone hit region with no distinct art) still appear,
// with textureName == "", zero size, and an empty `tiles` table — so
// geometry consumers see every subzone while `ipairs(ov.tiles)` draw loops
// stay safe. A consumer draws a textured overlay with a dumb loop: per tile
// SetTexture, SetWidth/Height, SetTexCoord(0, texCoordX, 0, texCoordY), SetPoint.

#include "Game.h"
#include "Offsets.h"
#include "dbc/Lookup.h"
#include "map/Area.h"
#include "map/Overlays.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace Map::Overlays {

namespace {

using FileRead_t = int(__stdcall *)(int unused, const char *path, void **outBuf,
                                    unsigned int *outSize, unsigned int extraBytes,
                                    int flag1, int flag2);
using SMemFree_t = void(__stdcall *)(void *ptr, const char *file, int line,
                                     int flags);

constexpr int kMaxTiles = 64;

// Smallest power of two >= v (min 16), matching the engine's own
// WorldMapFrame tile-file-size loop. A tile's content fills content/NextPot
// of its (power-of-two) file — a fraction that's identical whether the file
// is stock or HD-upscaled, so texcoords derived from it are scale-invariant.
int NextPot(int v) {
    int p = 16;
    while (p < v)
        p <<= 1;
    return p;
}

// Resolved tile grid for one overlay, cached for the session (DBC and
// MPQ contents are immutable per process).
struct ResolvedOverlay {
    uint8_t cols;
    uint8_t rows;
    uint8_t count; // tiles to draw (foreign tail already dropped)
    bool upscaled;
    uint16_t cell; // full-cell file size (256 on stock, 256*scale on an HD patch)
    uint16_t fileW[kMaxTiles]; // BLP file dimensions per tile
    uint16_t fileH[kMaxTiles];
};

ResolvedOverlay **g_cache = nullptr;
int g_cacheSize = 0;

// Reads one BLP's header dimensions through the engine VFS. Returns
// false when the file doesn't exist (end of the tile sequence) or isn't
// a BLP.
bool ReadBlpDims(const char *path, uint16_t *outW, uint16_t *outH) {
    auto FileRead = reinterpret_cast<FileRead_t>(Offsets::FUN_FILE_READ);
    auto SMemFree = reinterpret_cast<SMemFree_t>(Offsets::FUN_STORM_SMEM_FREE);
    void *buf = nullptr;
    unsigned int size = 0;
    if (FileRead(0, path, &buf, &size, 0, 1, 0) == 0 || buf == nullptr)
        return false;
    bool ok = false;
    const uint8_t *b = static_cast<const uint8_t *>(buf);
    if (size >= 0x14 && b[0] == 'B' && b[1] == 'L' && b[2] == 'P') {
        const uint32_t w = *reinterpret_cast<const uint32_t *>(b + 0x0C);
        const uint32_t h = *reinterpret_cast<const uint32_t *>(b + 0x10);
        if (w > 0 && w <= 4096 && h > 0 && h <= 4096) {
            *outW = static_cast<uint16_t>(w);
            *outH = static_cast<uint16_t>(h);
            ok = true;
        }
    }
    SMemFree(buf, __FILE__, __LINE__, 0);
    return ok;
}

// The quirk disambiguation (see module comment). `basePath` is the
// numberless texture path; dbcW/dbcH the overlay's DBC rect.
void ResolveTiles(ResolvedOverlay *out, const char *basePath, int dbcW,
                  int dbcH) {
    char path[0x120];
    int n = 0;
    while (n < kMaxTiles) {
        std::snprintf(path, sizeof(path), "%s%d.blp", basePath, n + 1);
        if (!ReadBlpDims(path, &out->fileW[n], &out->fileH[n]))
            break;
        ++n;
    }

    int dbcCols = (dbcW + 255) / 256;
    int dbcRows = (dbcH + 255) / 256;
    if (dbcCols < 1)
        dbcCols = 1;
    if (dbcRows < 1)
        dbcRows = 1;

    // Full-cell file size. On stock data a full (interior) tile is 256px; an
    // HD map patch (patch-*.mpq) upscales every tile uniformly, so a full cell
    // is 256*scale. Full tiles are the largest; edges/slivers are smaller — so
    // the max tile dimension is the full-cell size. Partial-tile detection
    // (and the texcoords in PushTiles) must compare against this S, not a
    // hardcoded 256, or an HD edge tile (e.g. a 200px edge upscaled to 400px)
    // reads as "full" and shears the row-major grid into diagonal strips.
    int S = 256;
    for (int i = 0; i < n; ++i) {
        if (out->fileW[i] > S)
            S = out->fileW[i];
        if (out->fileH[i] > S)
            S = out->fileH[i];
    }
    out->cell = static_cast<uint16_t>(S);

    // Real column count: the first partial-width tile is a row's right
    // edge. Search one past dbcCols so a sliver column the DBC width
    // rounds off is still found.
    int cols = dbcCols;
    bool widthEdge = false;
    for (int i = 0; i < n && i <= dbcCols; ++i) {
        if (out->fileW[i] < S) {
            cols = i + 1;
            widthEdge = true;
            break;
        }
    }

    // Real row count: the row holding the first partial-height tile is
    // the last row.
    int rows = dbcRows;
    bool heightEdge = false;
    for (int i = 0; i < n; ++i) {
        if (out->fileH[i] < S) {
            rows = i / cols + 1;
            heightEdge = true;
            break;
        }
    }

    int limit = cols * rows;
    bool upscaled = false;
    // Upscale exception: no partial edge anywhere and a whole-tile DBC
    // rect means the extras are higher-res art for the same rect, not
    // foreign — keep them all; recover the grid from the aspect ratio.
    if (!widthEdge && !heightEdge && dbcW % 256 == 0 && dbcH % 256 == 0 &&
        n > limit) {
        upscaled = true;
        cols = static_cast<int>(std::floor(
            std::sqrt(static_cast<double>(n) * dbcW / dbcH) + 0.5));
        if (cols < 1)
            cols = 1;
        if (cols > n)
            cols = n;
        limit = n;
        rows = (n + cols - 1) / cols;
    }
    if (limit > n)
        limit = n;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    out->cols = static_cast<uint8_t>(cols);
    out->rows = static_cast<uint8_t>(rows);
    out->count = static_cast<uint8_t>(limit);
    out->upscaled = upscaled;
}

const ResolvedOverlay *CachedResolve(int overlayId, const char *basePath,
                                     int dbcW, int dbcH) {
    const int count =
        Game::Read<int>(Offsets::VAR_WORLDMAP_OVERLAY_COUNT);
    if (g_cache == nullptr || g_cacheSize < count + 1) {
        auto **fresh = new ResolvedOverlay *[count + 1]();
        for (int i = 0; i < g_cacheSize; ++i)
            fresh[i] = g_cache[i];
        delete[] g_cache;
        g_cache = fresh;
        g_cacheSize = count + 1;
    }
    if (overlayId < 0 || overlayId >= g_cacheSize)
        return nullptr;
    if (g_cache[overlayId] == nullptr) {
        auto *r = new ResolvedOverlay();
        ResolveTiles(r, basePath, dbcW, dbcH);
        g_cache[overlayId] = r;
    }
    return g_cache[overlayId];
}

// WorldMapArea row resolution (areaID → row, current view → row) lives in
// the shared `Map::Area` helper — `GetMapWorldSize` keys off the same rows.

// Pushes the `tiles` array for one overlay (leaves it on the stack top).
void PushTiles(void *L, const ResolvedOverlay *r, const char *basePath,
               int dbcW, int dbcH, int offX, int offY) {
    Game::Lua::NewTable(L);
    if (r == nullptr || r->count == 0)
        return;

    const int cols = r->cols;
    char file[0x120];
    for (int i = 0; i < r->count; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        std::snprintf(file, sizeof(file), "%s%d", basePath, i + 1);

        double drawW, drawH, tcX, tcY, posX, posY;
        if (r->upscaled) {
            // Higher-res re-export of the same rect: tiles split the DBC
            // rect evenly, full texcoords.
            drawW = static_cast<double>(dbcW) / r->cols;
            drawH = static_cast<double>(dbcH) / r->rows;
            tcX = 1.0;
            tcY = 1.0;
            posX = offX + col * drawW;
            posY = offY + row * drawH;
        } else {
            // Canonical 256px logical grid. Draw sizes/positions stay in
            // logical map pixels; the texcoord is the content fraction of the
            // (possibly HD-upscaled) file. `scale` = file px per logical px
            // (1 on stock, 2/4 on an HD patch), so a full interior tile reads
            // texCoord 1.0 regardless of file size, and an edge's content
            // (remainder * scale) is measured against its actual file width.
            const int fileW = r->fileW[i];
            const int fileH = r->fileH[i];
            const double scale = (r->cell > 0) ? (r->cell / 256.0) : 1.0;

            double logicalW = 256.0, logicalH = 256.0;
            tcX = 1.0;
            tcY = 1.0;
            if (col == r->cols - 1) {
                const int rem = dbcW - 256 * (r->cols - 1);
                if (rem <= 0) {
                    // Sliver the DBC width rounded away: whole file is content
                    // (its logical width is the file width de-scaled).
                    logicalW = fileW / scale;
                } else {
                    // Content fills rem/NextPot(rem) of the POT tile file.
                    // Scale-invariant, so an HD-upscaled file (POT*scale)
                    // reads the same fraction — using the raw file width here
                    // would halve the texcoord on any tile the patch doubled.
                    logicalW = rem;
                    tcX = rem / static_cast<double>(NextPot(rem));
                }
            }
            if (row == r->rows - 1) {
                const int rem = dbcH - 256 * (r->rows - 1);
                if (rem <= 0) {
                    logicalH = fileH / scale;
                } else {
                    logicalH = rem;
                    tcY = rem / static_cast<double>(NextPot(rem));
                }
            }
            drawW = logicalW;
            drawH = logicalH;
            posX = offX + col * 256;
            posY = offY + row * 256;
        }

        Game::Lua::PushNumber(L, static_cast<double>(i + 1));
        Game::Lua::NewTable(L);
        Game::Lua::SetFieldString(L, "file", file);
        Game::Lua::SetFieldNumber(L, "width", drawW);
        Game::Lua::SetFieldNumber(L, "height", drawH);
        Game::Lua::SetFieldNumber(L, "texCoordX", tcX);
        Game::Lua::SetFieldNumber(L, "texCoordY", tcY);
        Game::Lua::SetFieldNumber(L, "offsetX", posX);
        Game::Lua::SetFieldNumber(L, "offsetY", posY);
        Game::Lua::SetTable(L, -3);
    }
}

// The local player's explored-areas bitfield (`[player+0xE68] + 0xE6C`), or
// null before the player is resident. Bit `AreaBit` set == that AreaTable
// area is explored. Same storage the engine's discovered-overlay rebuild
// (FUN_004a67a0) reads.
const uint8_t *ExplorationBits() {
    auto *player = static_cast<const uint8_t *>(Game::ResolveUnitToken("player"));
    if (player == nullptr)
        return nullptr;
    auto *sub = Game::Read<const uint8_t *>(player, Offsets::OFF_CGPLAYER_INFO);
    if (sub == nullptr)
        return nullptr;
    return sub + Offsets::OFF_PLAYER_EXPLORED_BITS;
}

// True if the local player has discovered `overlayRec` — mirrors the
// per-areaID test in FUN_004a67a0's discovered-overlay rebuild: an overlay
// is explored if ANY of its (up to 4) AreaTable ids is explored. An area
// with gate (+0x28) < 0 needs no exploration; otherwise its AreaBit (+0x0C)
// is looked up in `bits`. `bits == null` (no player) → not explored.
bool IsOverlayExplored(const uint8_t *overlayRec, const uint8_t *bits) {
    if (bits == nullptr)
        return false;
    for (int k = 0; k < 4; ++k) {
        const int areaID =
            Game::Read<int>(overlayRec, Offsets::OFF_WMO_AREA_ID + k * 4);
        if (areaID <= 0)
            continue;
        const uint8_t *areaRec =
            DBC::Record(Offsets::VAR_AREATABLE_RECORDS,
                        Offsets::VAR_AREATABLE_COUNT, static_cast<uint32_t>(areaID));
        if (areaRec == nullptr)
            continue;
        if (Game::Read<int>(areaRec, Offsets::OFF_AREATABLE_EXPLORE_GATE) < 0)
            return true; // area needs no exploration → always counts
        const int areaBit =
            Game::Read<int>(areaRec, Offsets::OFF_AREATABLE_AREA_BIT);
        if (areaBit < 0)
            continue;
        const int byteIdx = areaBit >> 3;
        if (byteIdx >= 0x100)
            return true; // out of bitfield range → engine treats as explored
        if ((bits[byteIdx] & (1u << (areaBit & 7))) != 0)
            return true;
    }
    return false;
}

// `C_Map.GetMapOverlays([areaID])` — see the module comment. Thin wrapper
// over the shared `PushZoneOverlays` with no exploration filter.
int __fastcall Script_GetMapOverlays(void *L) {
    const int wmaRow = Game::Lua::IsNumber(L, 1)
                           ? Map::Area::RowForAreaID(static_cast<uint32_t>(
                                 Game::Lua::ToNumber(L, 1)))
                           : Map::Area::CurrentViewRow();
    Game::Lua::SetTop(L, 0);
    Map::Overlays::PushZoneOverlays(L, wmaRow, Map::Overlays::Filter::All);
    return 1;
}

} // namespace

// Shared overlay walk — see Overlays.h. Filters by exploration and builds
// the rich per-overlay table (texture + resolved tiles + hit rect + areaIDs
// + fileDataIDs). Uses the anonymous-namespace helpers above (same TU).
void PushZoneOverlays(void *L, int wmaRow, Filter filter) {
    Game::Lua::NewTable(L);
    if (wmaRow <= 0)
        return;

    const uint8_t *wmaRec = DBC::Record(Offsets::VAR_WORLDMAP_AREA_RECORDS,
                                        Offsets::VAR_WORLDMAP_AREA_COUNT,
                                        static_cast<uint32_t>(wmaRow));
    const char *mapDir =
        (wmaRec != nullptr)
            ? Game::Read<const char *>(wmaRec, Offsets::OFF_WMA_NAME)
            : nullptr;

    // Exploration bitfield only needed for the filtered variants.
    const uint8_t *bits =
        (filter == Filter::All) ? nullptr : ExplorationBits();

    const int count =
        Game::Read<int>(Offsets::VAR_WORLDMAP_OVERLAY_COUNT);
    int outIdx = 0;
    for (int id = 1; id <= count; ++id) {
        const uint8_t *rec = DBC::Record(Offsets::VAR_WORLDMAP_OVERLAY_RECORDS,
                                         Offsets::VAR_WORLDMAP_OVERLAY_COUNT,
                                         static_cast<uint32_t>(id));
        if (rec == nullptr ||
            Game::Read<int>(rec, Offsets::OFF_WMO_WORLDMAP_AREA) != wmaRow)
            continue;

        if (filter != Filter::All) {
            const bool explored = IsOverlayExplored(rec, bits);
            if (filter == Filter::Explored && !explored)
                continue;
            if (filter == Filter::Unexplored && explored)
                continue;
        }

        const char *texName =
            Game::Read<const char *>(rec, Offsets::OFF_WMO_TEXTURE_NAME);
        const bool hasTexture = (texName != nullptr && texName[0] != '\0');

        const auto fieldInt = [rec](uintptr_t off) {
            return *reinterpret_cast<const int32_t *>(rec + off);
        };
        const int offX = fieldInt(Offsets::OFF_WMO_OFFSET_X);
        const int offY = fieldInt(Offsets::OFF_WMO_OFFSET_Y);

        // Texture placement + tile grid only exist for rows that carry art.
        // A hit-rect-only row (areaID + hit rect, no texture — a clickable
        // subzone region with no distinct BLP) still gets a result row: empty
        // textureName/path, zero size, and an empty `tiles` table (r == null →
        // PushTiles emits `{}`), so geometry consumers see it while art-draw
        // loops (`ipairs(ov.tiles)`) iterate zero times. Stock 1.12 and Octo
        // both happen to give every overlay a texture, but the DBC schema
        // allows texture-less rows, so we don't assume.
        char basePath[0x104];
        int dbcW = 0, dbcH = 0;
        const ResolvedOverlay *r = nullptr;
        if (hasTexture) {
            // Full path, mirroring the engine's own GetMapOverlayInfo format
            // (unresolvable map dir falls back to the "World" folder).
            if (mapDir != nullptr && mapDir[0] != '\0')
                std::snprintf(basePath, sizeof(basePath),
                              "Interface\\WorldMap\\%s\\%s", mapDir, texName);
            else
                std::snprintf(basePath, sizeof(basePath),
                              "Interface\\WorldMap\\World\\%s", texName);
            dbcW = fieldInt(Offsets::OFF_WMO_TEXTURE_WIDTH);
            dbcH = fieldInt(Offsets::OFF_WMO_TEXTURE_HEIGHT);
            r = CachedResolve(id, basePath, dbcW, dbcH);
        } else {
            basePath[0] = '\0';
        }

        outIdx += 1;
        Game::Lua::PushNumber(L, static_cast<double>(outIdx));
        Game::Lua::NewTable(L);
        Game::Lua::SetFieldString(L, "textureName", hasTexture ? texName : "");
        Game::Lua::SetFieldString(L, "texturePath", basePath);
        Game::Lua::SetFieldNumber(L, "textureWidth", dbcW);
        Game::Lua::SetFieldNumber(L, "textureHeight", dbcH);
        Game::Lua::SetFieldNumber(L, "offsetX", offX);
        Game::Lua::SetFieldNumber(L, "offsetY", offY);
        Game::Lua::SetFieldNumber(
            L, "mapPointX", fieldInt(Offsets::OFF_WMO_MAP_POINT_X));
        Game::Lua::SetFieldNumber(
            L, "mapPointY", fieldInt(Offsets::OFF_WMO_MAP_POINT_Y));

        // Hit rectangle (world-map canvas px, ~1002x668) — the tight clickable
        // bounds of the landmass, present for every row (texture or not). The
        // subzone-center source: (left+right)/2 / 1002, (top+bottom)/2 / 668.
        Game::Lua::SetFieldNumber(
            L, "hitRectTop", fieldInt(Offsets::OFF_WMO_HITRECT_TOP));
        Game::Lua::SetFieldNumber(
            L, "hitRectLeft", fieldInt(Offsets::OFF_WMO_HITRECT_LEFT));
        Game::Lua::SetFieldNumber(
            L, "hitRectBottom", fieldInt(Offsets::OFF_WMO_HITRECT_BOTTOM));
        Game::Lua::SetFieldNumber(
            L, "hitRectRight", fieldInt(Offsets::OFF_WMO_HITRECT_RIGHT));

        // AreaTable ids this overlay reveals (WorldMapOverlay.dbc areaID[4]).
        // `areaID` is the primary (first nonzero — every overlay has one);
        // `areaIDs` is the full nonzero list (52 of 707 overlays span 2-3
        // subzones, e.g. a lake straddling two areas). Same table shape as
        // `tiles` — build the array, then assign the scalar.
        int firstArea = 0, aCount = 0;
        Game::Lua::PushString(L, "areaIDs");
        Game::Lua::NewTable(L);
        for (int k = 0; k < 4; ++k) {
            const int aid = fieldInt(Offsets::OFF_WMO_AREA_ID + k * 4);
            if (aid == 0)
                continue;
            if (firstArea == 0)
                firstArea = aid;
            aCount += 1;
            Game::Lua::PushNumber(L, static_cast<double>(aCount));
            Game::Lua::PushNumber(L, static_cast<double>(aid));
            Game::Lua::SetTable(L, -3);
        }
        Game::Lua::SetTable(L, -3); // overlay.areaIDs = { ... }
        Game::Lua::SetFieldNumber(L, "areaID", firstArea);

        if (r != nullptr) {
            Game::Lua::SetFieldNumber(L, "tileCols", r->cols);
            Game::Lua::SetFieldNumber(L, "tileRows", r->rows);
            Game::Lua::SetFieldBool(L, "upscaled", r->upscaled);
        }
        Game::Lua::PushString(L, "tiles");
        PushTiles(L, r, basePath, dbcW, dbcH, offX, offY);
        Game::Lua::SetTable(L, -3);

        // fileDataIDs — ordered tile texture paths (retail's field name for
        // the tile list). Vanilla has no numeric fileDataIDs, so these are the
        // SetTexture-ready paths; redundant with tiles[].file, provided so
        // retail-ported exploration addons reading info.fileDataIDs work,
        // while tiles[] carries the per-tile draw geometry Octo needs.
        Game::Lua::PushString(L, "fileDataIDs");
        Game::Lua::NewTable(L);
        if (r != nullptr) {
            char file[0x120];
            for (int i = 0; i < r->count; ++i) {
                std::snprintf(file, sizeof(file), "%s%d", basePath, i + 1);
                Game::Lua::PushNumber(L, static_cast<double>(i + 1));
                Game::Lua::PushString(L, file);
                Game::Lua::SetTable(L, -3);
            }
        }
        Game::Lua::SetTable(L, -3); // overlay.fileDataIDs = { ... }

        Game::Lua::SetTable(L, -3);
    }
}

namespace {
void RegisterLuaFunctions() {
    Game::Lua::RegisterTableFunction("C_Map", "GetMapOverlays",
                                     &Script_GetMapOverlays);
}
const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};
} // namespace

} // namespace Map::Overlays
