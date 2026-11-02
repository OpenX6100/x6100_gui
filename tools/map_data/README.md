# JS8 map data

The Show Map view's base map (docs/MAP_PLAN.md): Natural Earth land, lakes,
country borders and state/province lines at two scales (110m for wide
views, 50m close-in), projected to Web Mercator ahead of time.

- `make_map_data.py` builds `js8_map.bin` (about 1 MB) from Natural Earth
  v5.1.2 GeoJSON, downloaded into `ne/` on first run (pinned commit, so the
  output is the same every time). The file layout is in the script.
- `js8_map.bin` is checked in: the radio's build uses it as is.
- `map_bench.cpp` draws a few views with `src/js8/map_render.cpp`, times
  them and saves PPMs. It needs only `geo.cpp` and `map_render.cpp`, so the
  same source runs on the radio:

```sh
python3 make_map_data.py
g++ -O2 -std=c++17 -I../../src/js8 map_bench.cpp ../../src/js8/geo.cpp \
    ../../src/js8/map_render.cpp -o map_bench
./map_bench js8_map.bin /tmp 9

# for the radio (static, no libraries needed there)
~/Work/x6100/toolchain/armv7-eabihf--glibc--stable-2023.08-1/bin/arm-linux-g++ \
    -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c++17 -static \
    -Wno-psabi -I../../src/js8 map_bench.cpp ../../src/js8/geo.cpp \
    ../../src/js8/map_render.cpp -o map_bench_arm
```

Natural Earth is public domain (https://www.naturalearthdata.com/).
