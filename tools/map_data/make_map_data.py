#!/usr/bin/env python3
"""Build the JS8 map's base-map data from Natural Earth (docs/MAP_PLAN.md).

Natural Earth (public domain, https://www.naturalearthdata.com/) land,
lakes, country borders and state/province lines, at two scales: 110m for
wide views and 50m for close-in ones. Every point is projected to Web
Mercator ahead of time, so the radio only scales and shifts integers.

    ./make_map_data.py [--ne DIR] [--out FILE]

--ne     a folder holding the GeoJSON files (downloaded into it when missing)
--out    the data file to write (default: js8_map.bin next to this script)

File layout (little-endian), read by src/js8/map_render.cpp:

    char     magic[8]    "JS8MAP1\\0"
    uint32   layer_count
    layer_count x {
        uint8    kind     0 land (fill), 1 lake (fill), 2 border (line), 3 state line
        uint8    detail   0 = 110m (wide views), 1 = 50m (close-in)
        uint16   reserved
        uint32   ring_count, point_count
        uint32   rings_offset, points_offset     (from the start of the file)
    }
    rings:  ring_count x { uint32 first_point, point_count; int32 x0, y0, x1, y1 }
    points: point_count x { int32 x, y }

Coordinates are "map units": the whole Web Mercator world is 2^24 square;
x = (lon + 180) / 360 * 2^24, y = (180 - mercator_y(lat)) / 360 * 2^24 with
mercator_y in degrees (y grows southwards; latitudes beyond +-85.05 are
clamped). Fills use the even-odd rule over all of a layer's rings, which
makes holes (islands in lakes, lakes in land) come out right whatever the
ring orientation.
"""

import argparse, json, math, os, struct, sys, urllib.request

NE_COMMIT = 'ca96624a56bd078437bca8184e78163e5039ad19'  # natural-earth-vector, v5.1.2 + fixes (2022-06-02)
NE_URL = 'https://raw.githubusercontent.com/nvkelso/natural-earth-vector/%s/geojson/%s.geojson'
WORLD = 1 << 24
MAX_LAT = 85.0511287798

LAYERS = [  # (kind, detail, file)
    (0, 0, 'ne_110m_land'), (1, 0, 'ne_110m_lakes'),
    (2, 0, 'ne_110m_admin_0_boundary_lines_land'), (3, 0, 'ne_110m_admin_1_states_provinces_lines'),
    (0, 1, 'ne_50m_land'), (1, 1, 'ne_50m_lakes'),
    (2, 1, 'ne_50m_admin_0_boundary_lines_land'), (3, 1, 'ne_50m_admin_1_states_provinces_lines'),
]


def mercator_y(lat):
    lat = max(-MAX_LAT, min(MAX_LAT, lat))
    return math.degrees(math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)))


def to_units(lon, lat):
    x = (lon + 180.0) / 360.0 * WORLD
    y = (180.0 - mercator_y(lat)) / 360.0 * WORLD
    return int(round(x)), int(round(y))


def rings_of(geom, fill):
    t, c = geom['type'], geom['coordinates']
    if fill:
        if t == 'Polygon': return list(c)
        if t == 'MultiPolygon': return [r for poly in c for r in poly]
    else:
        if t == 'LineString': return [c]
        if t == 'MultiLineString': return list(c)
    return []


def load(ne_dir, name):
    path = os.path.join(ne_dir, name + '.geojson')
    if not os.path.exists(path):
        print('downloading', name, file=sys.stderr)
        urllib.request.urlretrieve(NE_URL % (NE_COMMIT, name), path)
    with open(path) as f:
        return json.load(f)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--ne', default=os.path.join(here, 'ne'))
    ap.add_argument('--out', default=os.path.join(here, 'js8_map.bin'))
    a = ap.parse_args()
    os.makedirs(a.ne, exist_ok=True)

    layers = []
    for kind, detail, name in LAYERS:
        fill = kind in (0, 1)
        rings, points = [], []
        for feat in load(a.ne, name)['features']:
            for ring in rings_of(feat['geometry'], fill):
                pts = []
                for lon, lat, *_ in ring:
                    p = to_units(lon, lat)
                    if not pts or p != pts[-1]: pts.append(p)  # drop repeats
                if fill and len(pts) > 1 and pts[0] == pts[-1]: pts.pop()  # closing point is implied
                if len(pts) < (3 if fill else 2): continue
                xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
                rings.append((len(points), len(pts), min(xs), min(ys), max(xs), max(ys)))
                points.extend(pts)
        layers.append((kind, detail, rings, points))
        print('%-45s %6d rings %8d points' % (name, len(rings), len(points)), file=sys.stderr)

    header = 8 + 4 + len(layers) * 20
    offset = header
    table, blobs = [], []
    for kind, detail, rings, points in layers:
        rb = b''.join(struct.pack('<IIiiii', *r) for r in rings)
        pb = b''.join(struct.pack('<ii', x, y) for x, y in points)
        table.append(struct.pack('<BBHIIII', kind, detail, 0, len(rings), len(points), offset, offset + len(rb)))
        blobs += [rb, pb]
        offset += len(rb) + len(pb)
    with open(a.out, 'wb') as f:
        f.write(b'JS8MAP1\0' + struct.pack('<I', len(layers)) + b''.join(table) + b''.join(blobs))
    print('wrote %s (%d bytes)' % (a.out, offset), file=sys.stderr)


if __name__ == '__main__':
    main()
