/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 map: positions, Web Mercator, great circles
 *
 *  Maidenhead locators to positions, distances, the curved paths drawn to
 *  stations, and the view that fits the stations on the map (docs/MAP_PLAN.md).
 */

#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace x6100::js8::geo {

/// Degrees; east and north positive.
struct LatLon {
    double lat = 0;
    double lon = 0;
};

/// A locator's area: south-west and north-east corners.
struct Box {
    LatLon sw, ne;
};

/// A Maidenhead locator of 2, 4, 6, 8 or 10 characters (any case) as its
/// area. nullopt if it isn't one.
std::optional<Box> grid_box(std::string_view grid);

/// The centre of a locator's area, as the radio's qth_str_to_pos() gives it
/// (so map positions and the Stations view's distances agree).
std::optional<LatLon> grid_center(std::string_view grid);

/// Great-circle distance, km (mean Earth radius 6371 km, as qth.c).
double distance_km(LatLon a, LatLon b);

/// Initial great-circle bearing from a to b, degrees 0-359.
double bearing_deg(LatLon a, LatLon b);

/// `lon` moved by whole turns to within 180 degrees of `ref`.
double unwrap(double lon, double ref);

/// Points along the great circle from a to b (`n` segments, n + 1 points),
/// longitudes continuous from a's (they may pass +-180): drawn in Mercator
/// they give GridTracker's curved paths.
std::vector<LatLon> great_circle(LatLon a, LatLon b, int n = 64);

/// Web Mercator "y" of a latitude, in degrees of longitude at the equator
/// (so x and y share a scale). Latitudes are clamped to +-85.05 (the square
/// world of web maps).
double mercator_y(double lat);
double mercator_lat(double y);

/// A map view: which point is in the middle and how many pixels a degree
/// of longitude is. Pixels: x right, y down, (0, 0) the top-left corner.
struct View {
    double lon_c   = 0;   ///< longitude at the centre
    double merc_c  = 0;   ///< mercator_y() at the centre
    double px_deg  = 1;   ///< pixels per degree of longitude
    int    width   = 0;
    int    height  = 0;

    /// Screen position of a point; its longitude is taken within 180
    /// degrees of the centre's.
    void project(LatLon p, double &x, double &y) const;
    /// The point at a screen position.
    LatLon unproject(double x, double y) const;
};

/// The view (width x height px) that shows `home` and every point with a
/// margin (a fraction of each side) around them, never closer in than
/// min_lon x min_lat degrees (one station nearby mustn't zoom the map to a
/// town) and never wider than the whole world. Longitudes are taken within
/// 180 degrees of home's, so a path across the Pacific stays in one piece.
View fit(LatLon home, const std::vector<LatLon> &points, int width, int height, double margin = 0.12,
         double min_lon = 40, double min_lat = 18);

/// The whole inhabited world (latitudes -56 to +75) fitted to the height,
/// centred on `lon_c`. On a wide, low screen that's more than 360 degrees
/// across: the world repeats at the sides, as a web map does (the base map
/// fills them; stations are drawn once, within 180 degrees of the centre).
View whole_world(double lon_c, int width, int height);

} // namespace x6100::js8::geo
