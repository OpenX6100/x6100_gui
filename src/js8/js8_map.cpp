/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 map (C API)
 */

#include "js8_map.h"

#include "callsign_place.hpp"
#include "geo.hpp"
#include "map_render.hpp"

#include <sqlite3.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_set>

using namespace x6100::js8;

namespace {

std::mutex  countries_mutex;
CountryFile countries;

void copy_str(char *dst, std::size_t cap, const std::string &src) {
    if (!dst || !cap) return;
    std::snprintf(dst, cap, "%s", src.c_str());
}

geo::View to_view(const js8_map_view_t *v) {
    geo::View g;
    g.lon_c  = v->lon_c;
    g.merc_c = v->merc_c;
    g.px_deg = v->px_deg;
    g.width  = v->width;
    g.height = v->height;
    return g;
}

void from_view(const geo::View &g, js8_map_view_t *v) {
    v->lon_c  = g.lon_c;
    v->merc_c = g.merc_c;
    v->px_deg = g.px_deg;
    v->width  = g.width;
    v->height = g.height;
}

} // namespace

extern "C" bool js8_map_load_countries(const char *path) {
    CountryFile f;
    if (!path || !f.load(path)) return false;
    std::lock_guard<std::mutex> lock(countries_mutex);
    countries = std::move(f);
    return true;
}

extern "C" bool js8_map_countries_loaded(void) {
    std::lock_guard<std::mutex> lock(countries_mutex);
    return countries.loaded();
}

extern "C" bool js8_map_place(const char *call, const char *grid, js8_map_place_t *out) {
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    std::optional<CallsignPlace> pl;
    if (call && *call) {
        std::lock_guard<std::mutex> lock(countries_mutex);
        pl = countries.find(call);
    }
    if (pl) copy_str(out->continent, sizeof(out->continent), pl->continent);
    if (grid && *grid) {
        if (auto c = geo::grid_center(grid)) {
            out->lat = c->lat;
            out->lon = c->lon;
            copy_str(out->where, sizeof(out->where), grid);
            return true;
        }
    }
    if (!pl) return false;
    out->lat    = pl->pos.lat;
    out->lon    = pl->pos.lon;
    out->approx = true;
    copy_str(out->where, sizeof(out->where), pl->region.empty() ? pl->country : pl->region);
    return true;
}

extern "C" bool js8_map_my_continent(const char *call, const char *grid, char out[3]) {
    if (!out) return false;
    out[0] = '\0';
    std::lock_guard<std::mutex> lock(countries_mutex);
    if (call && *call) {
        if (auto pl = countries.find(call)) {
            copy_str(out, 3, pl->continent);
            return true;
        }
    }
    if (grid && *grid) {
        if (auto c = geo::grid_center(grid)) {
            if (auto cont = countries.continent_near(*c)) {
                copy_str(out, 3, *cont);
                return true;
            }
        }
    }
    return false;
}

extern "C" bool js8_map_choose_view(js8_map_mode_t mode, double my_lat, double my_lon, const char *my_continent,
                                    const js8_map_point_t *pts, unsigned n, int width, int height,
                                    js8_map_view_t *out) {
    if (!out) return false;
    geo::LatLon home{my_lat, my_lon};
    if (mode == JS8_MAP_WORLD) {
        from_view(geo::whole_world(my_lon, width, height), out);
        return true;
    }
    bool                     know_mine = my_continent && my_continent[0];
    std::vector<geo::LatLon> all, near;
    bool                     outside = false;
    for (unsigned i = 0; pts && i < n; i++) {
        geo::LatLon p{pts[i].lat, pts[i].lon};
        all.push_back(p);
        // A station whose continent is unknown counts as near: it can't
        // prove there's DX, and it mustn't flip the view.
        bool on_mine = !know_mine || !pts[i].continent[0] || std::strcmp(pts[i].continent, my_continent) == 0;
        if (on_mine) near.push_back(p);
        else outside = true;
    }
    bool world = mode == JS8_MAP_AUTO && outside;
    from_view(geo::fit(home, world ? all : near, width, height), out);
    return world;
}

extern "C" void js8_map_project(const js8_map_view_t *v, double lat, double lon, float *x, float *y) {
    double dx = 0, dy = 0;
    if (v) to_view(v).project({lat, lon}, dx, dy);
    if (x) *x = (float)dx;
    if (y) *y = (float)dy;
}

extern "C" bool js8_map_grid_rect(const js8_map_view_t *v, const char *grid, float *x0, float *y0, float *x1,
                                  float *y1) {
    if (!v || !grid) return false;
    auto b = geo::grid_box(grid);
    if (!b) return false;
    geo::View g = to_view(v);
    // Project the south-west corner, then place the east edge by the box's
    // width so a square never splits across the date line.
    double ax, ay, bx, by;
    g.project(b->sw, ax, ay);
    g.project({b->ne.lat, b->sw.lon}, bx, by);
    double w = (b->ne.lon - b->sw.lon) * g.px_deg;
    if (x0) *x0 = (float)ax;
    if (x1) *x1 = (float)(ax + w);
    if (y0) *y0 = (float)by; // north edge: smaller y
    if (y1) *y1 = (float)ay;
    return true;
}

extern "C" void js8_map_path(const js8_map_view_t *v, double lat1, double lon1, double lat2, double lon2, int n,
                             float *xs, float *ys) {
    if (!v || !xs || !ys || n < 1) return;
    geo::View g   = to_view(v);
    auto      pts = geo::great_circle({lat1, lon1}, {lat2, lon2}, n);
    // Keep the path in one piece: project the first point normally, then
    // every next one from the previous point's unwrapped longitude.
    double x0 = 0, y0 = 0;
    g.project(pts[0], x0, y0);
    double base_lon = geo::unwrap(pts[0].lon, g.lon_c);
    for (int i = 0; i <= n; i++) {
        double lon = base_lon + (pts[i].lon - pts[0].lon); // pts are continuous
        xs[i]      = (float)((lon - g.lon_c) * g.px_deg + g.width / 2.0);
        ys[i]      = (float)(-(geo::mercator_y(pts[i].lat) - g.merc_c) * g.px_deg + g.height / 2.0);
    }
}

extern "C" double js8_map_distance_km(double lat1, double lon1, double lat2, double lon2) {
    return geo::distance_km({lat1, lon1}, {lat2, lon2});
}

extern "C" double js8_map_bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    return geo::bearing_deg({lat1, lon1}, {lat2, lon2});
}

// ---- Worked before ---------------------------------------------------------

namespace {

std::mutex                      worked_mutex;
bool                            worked_known = false;
std::unordered_set<std::string> worked_grids, worked_countries;

std::string grid4(const char *grid) {
    std::string g;
    for (int i = 0; grid && grid[i] && i < 4; i++) g += (char)std::toupper((unsigned char)grid[i]);
    return g.size() == 4 && geo::grid_box(g) ? g : std::string();
}

std::string country_of(const char *call) {
    if (!call || !*call) return {};
    std::lock_guard<std::mutex> lock(countries_mutex);
    auto                        pl = countries.find(call);
    return pl ? pl->country : std::string();
}

} // namespace

extern "C" bool js8_map_load_worked(const char *db_path) {
    sqlite3 *db = nullptr;
    if (!db_path || sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    std::vector<std::pair<std::string, std::string>> rows; // call, grid
    sqlite3_stmt *st = nullptr;
    bool          ok = sqlite3_prepare_v2(db, "SELECT remote_callsign, remote_grid FROM qso_log", -1, &st, nullptr) ==
              SQLITE_OK;
    while (ok && sqlite3_step(st) == SQLITE_ROW) {
        auto c = (const char *)sqlite3_column_text(st, 0), g = (const char *)sqlite3_column_text(st, 1);
        rows.emplace_back(c ? c : "", g ? g : "");
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) return false;
    std::unordered_set<std::string> grids, cs;
    for (auto &[c, g] : rows) {
        auto g4 = grid4(g.c_str());
        if (!g4.empty()) grids.insert(g4);
        auto country = country_of(c.c_str());
        if (!country.empty()) cs.insert(country);
    }
    std::lock_guard<std::mutex> lock(worked_mutex);
    worked_grids     = std::move(grids);
    worked_countries = std::move(cs);
    worked_known     = true;
    return true;
}

extern "C" void js8_map_worked_add(const char *call, const char *grid) {
    auto g4 = grid4(grid);
    auto c  = country_of(call);
    std::lock_guard<std::mutex> lock(worked_mutex);
    if (!g4.empty()) worked_grids.insert(g4);
    if (!c.empty()) worked_countries.insert(c);
}

extern "C" unsigned js8_map_worked_grids(void) {
    std::lock_guard<std::mutex> lock(worked_mutex);
    return (unsigned)worked_grids.size();
}

extern "C" unsigned js8_map_worked_countries(void) {
    std::lock_guard<std::mutex> lock(worked_mutex);
    return (unsigned)worked_countries.size();
}

extern "C" js8_map_new_t js8_map_new_kind(const char *call, const char *grid, char *what, unsigned size) {
    if (what && size) what[0] = '\0';
    auto c  = country_of(call);
    auto g4 = grid4(grid);
    std::lock_guard<std::mutex> lock(worked_mutex);
    if (!worked_known) return JS8_MAP_NEW_NONE;
    if (!c.empty() && !worked_countries.count(c)) {
        copy_str(what, size, c);
        return JS8_MAP_NEW_DXCC;
    }
    if (!g4.empty() && !worked_grids.count(g4)) {
        copy_str(what, size, g4);
        return JS8_MAP_NEW_GRID;
    }
    return JS8_MAP_NEW_NONE;
}

struct js8_map_data {
    map::MapData data;
};

extern "C" js8_map_data_t *js8_map_data_load(const char *path) {
    if (!path) return nullptr;
    auto *d = new (std::nothrow) js8_map_data;
    if (!d) return nullptr;
    if (!d->data.load(path)) {
        delete d;
        return nullptr;
    }
    return d;
}

extern "C" void js8_map_data_free(js8_map_data_t *d) {
    delete d;
}

extern "C" bool js8_map_render_base(const js8_map_data_t *d, const js8_map_view_t *v, uint32_t *argb, int stride) {
    if (!d || !v || !argb) return false;
    map::render(d->data, to_view(v), argb, stride);
    return true;
}
