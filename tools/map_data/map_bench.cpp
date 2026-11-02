/*
 * Draws the JS8 map's base map for a few views, times it and saves each as a
 * PPM (docs/MAP_PLAN.md, phase 2). Built from src/js8/geo.cpp and
 * src/js8/map_render.cpp only, so the same source runs on the PC and, built
 * with the radio's toolchain, on the radio:
 *
 *   g++ -O2 -std=c++17 -I../../src/js8 map_bench.cpp ../../src/js8/geo.cpp \
 *       ../../src/js8/map_render.cpp -o map_bench
 *   ./map_bench js8_map.bin [out_dir] [runs]
 */

#include "geo.hpp"
#include "map_render.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace x6100::js8;

static void save_ppm(const std::string &path, const std::vector<uint32_t> &px, int w, int h) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (uint32_t p : px) {
        unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s js8_map.bin [out_dir] [runs]\n", argv[0]);
        return 2;
    }
    std::string out = argc > 2 ? argv[2] : ".";
    int         runs = argc > 3 ? std::atoi(argv[3]) : 5;

    auto t0 = std::chrono::steady_clock::now();
    map::MapData data;
    if (!data.load(argv[1])) {
        std::fprintf(stderr, "can't load %s\n", argv[1]);
        return 1;
    }
    double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("load: %.1f ms\n", load_ms);

    const int W = 771, H = 268; // the map area above the legend and TX bar
    geo::LatLon home = *geo::grid_center("CN89");
    struct Case {
        const char       *name;
        geo::View         view;
    };
    auto pts = [](std::initializer_list<const char *> grids) {
        std::vector<geo::LatLon> v;
        for (auto g : grids) v.push_back(*geo::grid_center(g));
        return v;
    };
    std::vector<Case> cases = {
        {"na_close", geo::fit(home, pts({"CN85", "DN17", "EN81", "EN91", "EM95", "EN34", "CM87", "DM43", "FN03", "FN42", "EM12"}), W, H)},
        {"world_fit", geo::fit(home, pts({"PM95", "FN42", "EM95", "CN85"}), W, H)},
        {"world_whole", geo::whole_world(home.lon, W, H)},
        {"eu_close", geo::fit(*geo::grid_center("IO91"), pts({"JO62", "JN18", "IN80", "JO89", "JN45", "KP20"}), W, H)},
        {"oc_close", geo::fit(*geo::grid_center("QF56"), pts({"RF73", "OF78", "OI33", "RG37"}), W, H)},
        {"local", geo::fit(home, pts({"CN88"}), W, H)},
    };
    std::vector<uint32_t> px((size_t)W * H);
    for (auto &c : cases) {
        std::vector<double> ms;
        map::RenderStats    st;
        for (int i = 0; i < runs; i++) {
            auto a = std::chrono::steady_clock::now();
            map::render(data, c.view, px.data(), W, {}, &st);
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
        }
        std::sort(ms.begin(), ms.end());
        std::printf("%-12s %6.1f px/deg  detail %s  median %7.1f ms  (min %.1f, max %.1f)  rings %u edges %u segments %u\n",
                    c.name, c.view.px_deg, st.detail ? "50m " : "110m", ms[ms.size() / 2], ms.front(), ms.back(),
                    st.rings, st.edges, st.segments);
        save_ppm(out + "/" + c.name + ".ppm", px, W, H);
    }
    return 0;
}
