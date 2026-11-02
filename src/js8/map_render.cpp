/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 map: the base map, drawn for any view
 */

#include "map_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace x6100::js8::map {

namespace {

constexpr int SUB_ROWS = 4; // vertical anti-aliasing: sub-rows per pixel row

template <typename T> bool read_at(const std::vector<uint8_t> &b, size_t off, T &out) {
    if (off + sizeof(T) > b.size()) return false;
    std::memcpy(&out, b.data() + off, sizeof(T));
    return true;
}

inline uint32_t blend(uint32_t d, uint32_t s, float a) {
    int ia = (int)(a * 256.0f + 0.5f);
    if (ia <= 0) return d;
    if (ia > 256) ia = 256;
    int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    int sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
    dr += ((sr - dr) * ia) >> 8;
    dg += ((sg - dg) * ia) >> 8;
    db += ((sb - db) * ia) >> 8;
    return 0xFF000000u | (uint32_t)dr << 16 | (uint32_t)dg << 8 | (uint32_t)db;
}

/// Map units -> pixels for one wrap copy: x_px = x * s + ox, y_px = y * s + oy.
struct Xform {
    double s, ox, oy;
};

struct Edge {
    float y0, y1; // y0 < y1, pixels
    float x0;     // x at y0
    float dxdy;
};

class Target {
public:
    Target(uint32_t *px, int w, int h, int stride) : px_(px), w_(w), h_(h), stride_(stride) {}

    void clear(uint32_t c) {
        for (int y = 0; y < h_; y++) std::fill(px_ + (size_t)y * stride_, px_ + (size_t)y * stride_ + w_, c);
    }

    /// Even-odd fill of every edge's shape with anti-aliased edges.
    void fill(std::vector<Edge> &edges, uint32_t color) {
        if (edges.empty()) return;
        std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) { return a.y0 < b.y0; });
        std::vector<float>       acc(w_ + 2), diff(w_ + 2), xs;
        std::vector<const Edge *> active;
        size_t                    next = 0;
        const float               wgt  = 1.0f / SUB_ROWS;
        for (int row = 0; row < h_; row++) {
            bool any = false;
            for (int sub = 0; sub < SUB_ROWS; sub++) {
                float y = row + (sub + 0.5f) / SUB_ROWS;
                while (next < edges.size() && edges[next].y0 <= y) active.push_back(&edges[next++]);
                active.erase(std::remove_if(active.begin(), active.end(), [y](const Edge *e) { return e->y1 <= y; }),
                             active.end());
                xs.clear();
                for (const Edge *e : active)
                    if (e->y0 <= y) xs.push_back(e->x0 + (y - e->y0) * e->dxdy);
                if (xs.size() < 2) continue;
                std::sort(xs.begin(), xs.end());
                for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                    float a = std::max(xs[i], 0.0f), b = std::min(xs[i + 1], (float)w_);
                    if (b <= a) continue;
                    int ia = (int)a, ib = (int)b;
                    if (ia == ib) {
                        acc[ia] += (b - a) * wgt;
                    } else {
                        acc[ia] += (ia + 1 - a) * wgt;
                        diff[ia + 1] += wgt;
                        diff[ib] -= wgt;
                        acc[ib] += (b - ib) * wgt;
                    }
                    any = true;
                }
            }
            if (!any) continue;
            uint32_t *out = px_ + (size_t)row * stride_;
            float     run = 0;
            for (int x = 0; x < w_; x++) {
                run += diff[x];
                float c = run + acc[x];
                if (c > 0.002f) out[x] = blend(out[x], color, c > 1 ? 1 : c);
                acc[x] = diff[x] = 0;
            }
            acc[w_] = diff[w_] = acc[w_ + 1] = diff[w_ + 1] = 0;
        }
    }

    /// A 1 px anti-aliased line (Xiaolin Wu).
    void line(float x0, float y0, float x1, float y1, uint32_t color, float alpha = 1.0f) {
        bool steep = std::fabs(y1 - y0) > std::fabs(x1 - x0);
        if (steep) {
            std::swap(x0, y0);
            std::swap(x1, y1);
        }
        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        float dx = x1 - x0, grad = dx < 1e-6f ? 0 : (y1 - y0) / dx;
        int   major = steep ? h_ : w_;
        int   xs = std::max(0, (int)std::floor(x0 + 0.5f)), xe = std::min(major - 1, (int)std::floor(x1 + 0.5f));
        for (int x = xs; x <= xe; x++) {
            float y = y0 + grad * (x - x0);
            int   iy = (int)std::floor(y);
            float f  = y - iy;
            plot(steep, x, iy, color, (1 - f) * alpha);
            plot(steep, x, iy + 1, color, f * alpha);
        }
    }

    void hline(int y, uint32_t color, float alpha) {
        if (y < 0 || y >= h_) return;
        uint32_t *out = px_ + (size_t)y * stride_;
        for (int x = 0; x < w_; x++) out[x] = blend(out[x], color, alpha);
    }

    void vline(int x, uint32_t color, float alpha) {
        if (x < 0 || x >= w_) return;
        for (int y = 0; y < h_; y++) px_[(size_t)y * stride_ + x] = blend(px_[(size_t)y * stride_ + x], color, alpha);
    }

    int w() const { return w_; }
    int h() const { return h_; }

private:
    void plot(bool steep, int a, int b, uint32_t color, float alpha) {
        int x = steep ? b : a, y = steep ? a : b;
        if (x < 0 || y < 0 || x >= w_ || y >= h_ || alpha <= 0) return;
        uint32_t &p = px_[(size_t)y * stride_ + x];
        p           = blend(p, color, alpha);
    }

    uint32_t *px_;
    int       w_, h_, stride_;
};

/// The wrap copies of the world that can show in the view: three cover
/// any view up to 540 degrees across (the whole-world view is about 510).
std::vector<Xform> xforms(const geo::View &v) {
    std::vector<Xform> out;
    double             s = 360.0 / MapData::WORLD * v.px_deg;
    double             oy = (v.merc_c - 180.0) * v.px_deg + v.height / 2.0;
    for (int k = -1; k <= 1; k++) out.push_back({s, (-180.0 - v.lon_c + 360.0 * k) * v.px_deg + v.width / 2.0, oy});
    return out;
}

bool visible(const MapData::Ring &r, const Xform &t, int w, int h) {
    double x0 = r.x0 * t.s + t.ox, x1 = r.x1 * t.s + t.ox, y0 = r.y0 * t.s + t.oy, y1 = r.y1 * t.s + t.oy;
    return x1 >= -1 && x0 <= w + 1 && y1 >= -1 && y0 <= h + 1;
}

void fill_layer(const MapData::Layer &l, const std::vector<Xform> &ts, Target &tg, uint32_t color, RenderStats *st) {
    std::vector<Edge> edges;
    for (const auto &t : ts)
        for (const auto &r : l.rings) {
            if (!visible(r, t, tg.w(), tg.h())) continue;
            if (st) st->rings++;
            for (uint32_t i = 0; i < r.count; i++) {
                const auto &a = l.points[r.first + i];
                const auto &b = l.points[r.first + (i + 1) % r.count]; // closed
                float ax = (float)(a.x * t.s + t.ox), ay = (float)(a.y * t.s + t.oy);
                float bx = (float)(b.x * t.s + t.ox), by = (float)(b.y * t.s + t.oy);
                if (ay == by) continue;
                if (ay > by) {
                    std::swap(ax, bx);
                    std::swap(ay, by);
                }
                if (by <= 0 || ay >= tg.h()) continue; // never crosses a row centre in view
                edges.push_back({ay, by, ax, (bx - ax) / (by - ay)});
            }
        }
    if (st) st->edges += (unsigned)edges.size();
    tg.fill(edges, color);
}

void line_layer(const MapData::Layer &l, const std::vector<Xform> &ts, Target &tg, uint32_t color, RenderStats *st) {
    const float w = (float)tg.w(), h = (float)tg.h();
    for (const auto &t : ts)
        for (const auto &r : l.rings) {
            if (!visible(r, t, tg.w(), tg.h())) continue;
            if (st) st->rings++;
            for (uint32_t i = 0; i + 1 < r.count; i++) {
                const auto &a = l.points[r.first + i];
                const auto &b = l.points[r.first + i + 1];
                float ax = (float)(a.x * t.s + t.ox), ay = (float)(a.y * t.s + t.oy);
                float bx = (float)(b.x * t.s + t.ox), by = (float)(b.y * t.s + t.oy);
                if ((ax < -1 && bx < -1) || (ax > w + 1 && bx > w + 1) || (ay < -1 && by < -1) ||
                    (ay > h + 1 && by > h + 1))
                    continue;
                if (st) st->segments++;
                tg.line(ax, ay, bx, by, color);
            }
        }
}

} // namespace

bool MapData::load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse(bytes);
}

bool MapData::parse(const std::vector<uint8_t> &b) {
    layers_.clear();
    if (b.size() < 12 || std::memcmp(b.data(), "JS8MAP1\0", 8) != 0) return false;
    uint32_t n = 0;
    read_at(b, 8, n);
    if (n > 64) return false;
    std::vector<Layer> layers;
    for (uint32_t i = 0; i < n; i++) {
        size_t   at = 12 + (size_t)i * 20;
        uint8_t  kind = 0, detail = 0;
        uint32_t rc = 0, pc = 0, ro = 0, po = 0;
        if (!read_at(b, at, kind) || !read_at(b, at + 1, detail) || !read_at(b, at + 4, rc) ||
            !read_at(b, at + 8, pc) || !read_at(b, at + 12, ro) || !read_at(b, at + 16, po))
            return false;
        if ((uint64_t)ro + (uint64_t)rc * 24 > b.size() || (uint64_t)po + (uint64_t)pc * 8 > b.size()) return false;
        Layer l;
        l.kind   = kind;
        l.detail = detail;
        l.rings.resize(rc);
        l.points.resize(pc);
        for (uint32_t k = 0; k < rc; k++) {
            auto &r = l.rings[k];
            size_t o = ro + (size_t)k * 24;
            read_at(b, o, r.first);
            read_at(b, o + 4, r.count);
            read_at(b, o + 8, r.x0);
            read_at(b, o + 12, r.y0);
            read_at(b, o + 16, r.x1);
            read_at(b, o + 20, r.y1);
            if ((uint64_t)r.first + r.count > pc) return false;
        }
        if (pc) std::memcpy(l.points.data(), b.data() + po, (size_t)pc * 8);
        layers.push_back(std::move(l));
    }
    layers_ = std::move(layers);
    return !layers_.empty();
}

void render(const MapData &data, const geo::View &view, uint32_t *argb, int stride, const Style &style,
            RenderStats *stats) {
    if (!argb || view.width <= 0 || view.height <= 0) return;
    Target tg(argb, view.width, view.height, stride);
    tg.clear(style.ocean);
    int detail = view.px_deg >= DETAIL_PX_DEG ? 1 : 0;
    if (stats) {
        *stats        = {};
        stats->detail = detail;
    }
    auto ts = xforms(view);
    auto layer = [&](uint8_t kind) -> const MapData::Layer * {
        for (const auto &l : data.layers())
            if (l.kind == kind && l.detail == detail) return &l;
        return nullptr;
    };
    if (auto l = layer(MapData::LAND)) fill_layer(*l, ts, tg, style.land, stats);
    if (auto l = layer(MapData::LAKE)) fill_layer(*l, ts, tg, style.ocean, stats);
    if (auto l = layer(MapData::STATE)) line_layer(*l, ts, tg, style.state, stats);
    if (auto l = layer(MapData::BORDER)) line_layer(*l, ts, tg, style.border, stats);

    if (style.grid_lines) {
        // Maidenhead fields: every 20 degrees of longitude, 10 of latitude.
        float a = ((style.grid >> 24) & 0xFF) / 255.0f;
        for (int k = -1; k <= 1; k++)
            for (int lon = -180; lon < 180; lon += 20) {
                double x = (lon + 360.0 * k - view.lon_c) * view.px_deg + view.width / 2.0;
                tg.vline((int)std::floor(x + 0.5), style.grid, a);
            }
        for (int lat = -80; lat <= 80; lat += 10) {
            double y = (view.merc_c - geo::mercator_y(lat)) * view.px_deg + view.height / 2.0;
            tg.hline((int)std::floor(y + 0.5), style.grid, a);
        }
    }
}

} // namespace x6100::js8::map
