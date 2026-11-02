/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 map: the base map, drawn for any view
 *
 *  Natural Earth land, lakes, borders and state lines (tools/map_data), in
 *  GridTracker's Dark Gray colours, drawn by the radio itself for whatever
 *  view the map fits (docs/MAP_PLAN.md). Fills are anti-aliased (exact
 *  coverage across, 4 sub-rows down), lines are 1 px anti-aliased. No LVGL:
 *  the output is 0xAARRGGBB pixels, which is lv_color_t at LVGL's 32-bit
 *  colour depth.
 */

#pragma once

#include "geo.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace x6100::js8::map {

struct Style {
    uint32_t ocean  = 0xFF232227; // GridTracker Dark Gray's water
    uint32_t land   = 0xFF3F3F41; //   ... and land
    uint32_t border = 0xFF5C5C60; // country borders
    uint32_t state  = 0xFF4C4C50; // state and province lines
    uint32_t grid   = 0x16FFFFFF; // Maidenhead field lines (alpha 22)
    bool     grid_lines = true;
};

/// The base-map data file (tools/map_data/make_map_data.py).
class MapData {
public:
    bool load(const std::string &path);
    bool parse(const std::vector<uint8_t> &bytes);
    bool loaded() const { return !layers_.empty(); }

    enum Kind : uint8_t { LAND = 0, LAKE = 1, BORDER = 2, STATE = 3 };
    struct Ring {
        uint32_t first, count;
        int32_t  x0, y0, x1, y1; ///< bounding box, map units
    };
    struct Point {
        int32_t x, y;
    };
    struct Layer {
        uint8_t            kind = 0, detail = 0;
        std::vector<Ring>  rings;
        std::vector<Point> points;
    };
    const std::vector<Layer> &layers() const { return layers_; }

    /// The whole Web Mercator world is WORLD map units square.
    static constexpr int64_t WORLD = 1 << 24;

private:
    std::vector<Layer> layers_;
};

/// What a render did (for timing and tests).
struct RenderStats {
    int      detail = 0;        ///< 0 = 110m, 1 = 50m
    unsigned rings  = 0;        ///< rings drawn (after culling, per wrap copy)
    unsigned edges  = 0;        ///< fill edges
    unsigned segments = 0;      ///< line segments drawn
};

/// Views wider than this many pixels per degree use the 50m data.
constexpr double DETAIL_PX_DEG = 3.5;

/// Draw the base map for `view` into `argb` (view.width x view.height
/// pixels, `stride` pixels per row).
void render(const MapData &data, const geo::View &view, uint32_t *argb, int stride, const Style &style = {},
            RenderStats *stats = nullptr);

} // namespace x6100::js8::map
