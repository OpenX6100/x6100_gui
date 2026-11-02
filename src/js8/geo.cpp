/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 map: positions, Web Mercator, great circles
 */

#include "geo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace x6100::js8::geo {

namespace {

constexpr double EARTH_KM = 6371.0;
constexpr double MAX_LAT  = 85.0511287798; // Web Mercator's square world

double rad(double d) { return d * M_PI / 180.0; }
double deg(double r) { return r * 180.0 / M_PI; }

} // namespace

std::optional<Box> grid_box(std::string_view g) {
    auto n = g.size();
    if (n < 2 || n > 10 || n % 2) return std::nullopt;
    // Fields 20 x 10 degrees (A-R), squares /10 (0-9), subsquares /24
    // (A-X), then /10 and /24 again.
    double lon = -180, lat = -90, w = 360, h = 180;
    for (std::size_t i = 0; i < n; i += 2) {
        int  pair = (int)(i / 2);
        char a = (char)std::toupper((unsigned char)g[i]), b = (char)std::toupper((unsigned char)g[i + 1]);
        int  div = pair == 0 ? 18 : (pair % 2 ? 10 : 24);
        int  x, y;
        if (pair % 2) { // digits
            if (!std::isdigit((unsigned char)a) || !std::isdigit((unsigned char)b)) return std::nullopt;
            x = a - '0';
            y = b - '0';
        } else {
            char last = pair == 0 ? 'R' : 'X';
            if (a < 'A' || a > last || b < 'A' || b > last) return std::nullopt;
            x = a - 'A';
            y = b - 'A';
        }
        w /= div;
        h /= div;
        lon += x * w;
        lat += y * h;
    }
    return Box{{lat, lon}, {lat + h, lon + w}};
}

std::optional<LatLon> grid_center(std::string_view grid) {
    auto b = grid_box(grid);
    if (!b) return std::nullopt;
    return LatLon{(b->sw.lat + b->ne.lat) / 2, (b->sw.lon + b->ne.lon) / 2};
}

double distance_km(LatLon a, LatLon b) {
    double dlat = rad(b.lat - a.lat), dlon = rad(b.lon - a.lon);
    double s = std::sin(dlat / 2) * std::sin(dlat / 2) +
               std::cos(rad(a.lat)) * std::cos(rad(b.lat)) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2 * EARTH_KM * std::atan2(std::sqrt(s), std::sqrt(1 - s));
}

double bearing_deg(LatLon a, LatLon b) {
    double p1 = rad(a.lat), p2 = rad(b.lat), dl = rad(b.lon - a.lon);
    double t  = deg(std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) -
                                                                std::sin(p1) * std::cos(p2) * std::cos(dl)));
    return std::fmod(t + 360, 360);
}

double unwrap(double lon, double ref) {
    while (lon - ref > 180) lon -= 360;
    while (lon - ref < -180) lon += 360;
    return lon;
}

std::vector<LatLon> great_circle(LatLon a, LatLon b, int n) {
    if (n < 1) n = 1;
    auto vec = [](LatLon p, double v[3]) {
        double la = rad(p.lat), lo = rad(p.lon);
        v[0] = std::cos(la) * std::cos(lo);
        v[1] = std::cos(la) * std::sin(lo);
        v[2] = std::sin(la);
    };
    double va[3], vb[3];
    vec(a, va);
    vec(b, vb);
    double dot   = std::clamp(va[0] * vb[0] + va[1] * vb[1] + va[2] * vb[2], -1.0, 1.0);
    double omega = std::acos(dot);
    std::vector<LatLon> out;
    out.reserve(n + 1);
    double prev = a.lon;
    for (int i = 0; i <= n; i++) {
        double t = (double)i / n, v[3];
        if (omega < 1e-9 || std::fabs(M_PI - omega) < 1e-9) { // same or opposite points: no unique path
            v[0] = va[0]; v[1] = va[1]; v[2] = va[2];
            if (i == n) { v[0] = vb[0]; v[1] = vb[1]; v[2] = vb[2]; }
        } else {
            double s0 = std::sin((1 - t) * omega) / std::sin(omega), s1 = std::sin(t * omega) / std::sin(omega);
            for (int k = 0; k < 3; k++) v[k] = s0 * va[k] + s1 * vb[k];
        }
        double lon = unwrap(deg(std::atan2(v[1], v[0])), prev);
        double lat = deg(std::asin(std::clamp(v[2], -1.0, 1.0)));
        prev       = lon;
        out.push_back({lat, lon});
    }
    return out;
}

double mercator_y(double lat) {
    lat = std::clamp(lat, -MAX_LAT, MAX_LAT);
    return deg(std::log(std::tan(M_PI / 4 + rad(lat) / 2)));
}

double mercator_lat(double y) {
    return deg(2 * std::atan(std::exp(rad(y))) - M_PI / 2);
}

void View::project(LatLon p, double &x, double &y) const {
    x = (unwrap(p.lon, lon_c) - lon_c) * px_deg + width / 2.0;
    y = -(mercator_y(p.lat) - merc_c) * px_deg + height / 2.0;
}

LatLon View::unproject(double x, double y) const {
    double lon = (x - width / 2.0) / px_deg + lon_c;
    double my  = -(y - height / 2.0) / px_deg + merc_c;
    return {mercator_lat(my), unwrap(lon, 0)};
}

View fit(LatLon home, const std::vector<LatLon> &points, int width, int height, double margin, double min_lon,
         double min_lat) {
    double lo0 = home.lon, lo1 = home.lon, la0 = home.lat, la1 = home.lat;
    for (auto &p : points) {
        double lon = unwrap(p.lon, home.lon);
        lo0 = std::min(lo0, lon);
        lo1 = std::max(lo1, lon);
        la0 = std::min(la0, p.lat);
        la1 = std::max(la1, p.lat);
    }
    if (lo1 - lo0 < min_lon) {
        double c = (lo0 + lo1) / 2;
        lo0 = c - min_lon / 2;
        lo1 = c + min_lon / 2;
    }
    if (la1 - la0 < min_lat) {
        double c = (la0 + la1) / 2;
        la0 = c - min_lat / 2;
        la1 = c + min_lat / 2;
    }
    la0 = std::max(la0, -MAX_LAT);
    la1 = std::min(la1, MAX_LAT);
    double m0 = mercator_y(la0), m1 = mercator_y(la1);
    double use_w = width * (1 - 2 * margin), use_h = height * (1 - 2 * margin);
    View   v;
    v.width  = width;
    v.height = height;
    v.px_deg = std::min(use_w / std::max(lo1 - lo0, 1e-6), use_h / std::max(m1 - m0, 1e-6));
    v.px_deg = std::max(v.px_deg, width / 360.0); // never wider than the whole world
    v.lon_c  = unwrap((lo0 + lo1) / 2, 0);
    v.merc_c = (m0 + m1) / 2;
    return v;
}

View whole_world(double lon_c, int width, int height) {
    const double top = mercator_y(75), bottom = mercator_y(-56);
    View         v;
    v.width  = width;
    v.height = height;
    v.px_deg = std::min(width / 360.0, height / (top - bottom));
    v.lon_c  = unwrap(lon_c, 0);
    v.merc_c = (top + bottom) / 2;
    return v;
}

} // namespace x6100::js8::geo
