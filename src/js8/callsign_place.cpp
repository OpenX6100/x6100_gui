/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 map: where a callsign is, from its prefix
 */

#include "callsign_place.hpp"
#include "strutil.hpp"

#include "classify.hpp" // base_callsign()

#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace x6100::js8 {

namespace {

/// A region a call area stands for.
struct Region {
    const char *country;  ///< cty.dat's name
    const char *prefixes; ///< the prefixes it applies to, "" = any of the country's
    int         area;
    const char *name;
    double      lat, lon;
};

// Centres are each region's middle, roughly: good enough for "somewhere in
// British Columbia", which is all a prefix can say. Canada's VO and VY
// prefixes have their own areas; every other Canadian prefix (VE, VA, VB,
// VC, VG, VX, CF, CG, CJ, CK, CY, CZ, XJ-XO) follows VE's numbers.
const Region REGIONS[] = {
    {"Canada", "VO", 1, "Newfoundland", 48.6, -56.0},
    {"Canada", "VO", 2, "Labrador", 53.6, -62.0},
    {"Canada", "VY", 0, "Nunavut", 66.0, -95.0},
    {"Canada", "VY", 1, "Yukon", 63.5, -135.5},
    {"Canada", "VY", 2, "Prince Edward Island", 46.3, -63.2},
    {"Canada", "", 1, "Nova Scotia", 45.0, -63.0},
    {"Canada", "", 2, "Quebec", 52.5, -72.5},
    {"Canada", "", 3, "Ontario", 50.0, -85.5},
    {"Canada", "", 4, "Manitoba", 55.0, -97.5},
    {"Canada", "", 5, "Saskatchewan", 54.5, -106.0},
    {"Canada", "", 6, "Alberta", 55.0, -114.5},
    {"Canada", "", 7, "British Columbia", 54.0, -125.0},
    {"Canada", "", 8, "Northwest Territories", 64.5, -119.0},
    {"Canada", "", 9, "New Brunswick", 46.5, -66.4},
    // US call areas: a US call no longer says where its holder lives, but
    // most still do (the middle of the area's states).
    {"United States", "", 0, "US call area 0", 42.5, -98.0},
    {"United States", "", 1, "US call area 1", 43.8, -71.3},
    {"United States", "", 2, "US call area 2", 42.0, -75.0},
    {"United States", "", 3, "US call area 3", 40.3, -77.0},
    {"United States", "", 4, "US call area 4", 34.0, -83.0},
    {"United States", "", 5, "US call area 5", 32.5, -97.0},
    {"United States", "", 6, "US call area 6", 37.2, -119.5},
    {"United States", "", 7, "US call area 7", 42.5, -114.5},
    {"United States", "", 8, "US call area 8", 41.5, -83.5},
    {"United States", "", 9, "US call area 9", 41.8, -88.5},
    {"Australia", "", 1, "Australian Capital Territory", -35.4, 149.0},
    {"Australia", "", 2, "New South Wales", -32.2, 147.0},
    {"Australia", "", 3, "Victoria", -37.0, 144.3},
    {"Australia", "", 4, "Queensland", -22.5, 144.4},
    {"Australia", "", 5, "South Australia", -30.1, 135.8},
    {"Australia", "", 6, "Western Australia", -25.3, 121.6},
    {"Australia", "", 7, "Tasmania", -42.0, 146.6},
    {"Australia", "", 8, "Northern Territory", -19.4, 133.4},
    {"Japan", "", 0, "Shinetsu", 37.3, 138.4},
    {"Japan", "", 1, "Kanto", 36.0, 139.6},
    {"Japan", "", 2, "Tokai", 35.2, 137.3},
    {"Japan", "", 3, "Kansai", 34.8, 135.6},
    {"Japan", "", 4, "Chugoku", 34.9, 132.8},
    {"Japan", "", 5, "Shikoku", 33.7, 133.4},
    {"Japan", "", 6, "Kyushu", 32.7, 130.9},
    {"Japan", "", 7, "Tohoku", 39.6, 140.6},
    {"Japan", "", 8, "Hokkaido", 43.4, 142.8},
    {"Japan", "", 9, "Hokuriku", 36.7, 136.8},
};

bool starts_with_any(const std::string &s, const char *list) {
    // list: prefixes of two letters, concatenated ("VOVY")
    for (const char *p = list; p[0] && p[1]; p += 2)
        if (s.size() >= 2 && s[0] == p[0] && s[1] == p[1]) return true;
    return false;
}

} // namespace

std::string effective_prefix(std::string_view call_in) {
    // Desktop JS8Call's Radio::effective_prefix(), with its non_prefix_suffix.
    static const std::regex non_prefix_suffix(R"(^([0-9AMPQR]|QRP|F[DF]|[AM]M|L[HT]|LGT)$)");
    std::string call   = upper(call_in);
    std::string prefix = call;
    auto        slash  = call.find('/');
    if (slash != std::string::npos) {
        size_t right = call.size() - slash - 1;
        if (right >= slash) {
            prefix = call.substr(0, slash);
        } else {
            prefix = call.substr(slash + 1);
            if (std::regex_search(prefix, non_prefix_suffix)) prefix = call.substr(0, slash);
        }
    }
    return prefix;
}

int call_area(std::string_view call_in) {
    std::string call = upper(call_in);
    // A single-digit part: portable in that call area ("KG4UHM/6").
    size_t start = 0;
    while (start <= call.size()) {
        size_t end = call.find('/', start);
        if (end == std::string::npos) end = call.size();
        if (start > 0 && end - start == 1 && std::isdigit((unsigned char)call[start])) return call[start] - '0';
        start = end + 1;
    }
    // Else the digit that ends the prefix: the last digit with only letters
    // after it ("VE7NHW", "7K1ABC", "W6").
    std::string p = effective_prefix(call);
    size_t      i = p.size();
    while (i > 0 && std::isalpha((unsigned char)p[i - 1])) i--;
    if (i > 0 && std::isdigit((unsigned char)p[i - 1])) return p[i - 1] - '0';
    return -1;
}

bool call_area_region(const std::string &country, std::string_view prefix_in, int area, std::string &name,
                      geo::LatLon &pos) {
    if (area < 0) return false;
    std::string prefix = upper(prefix_in);
    // Prefixes with their own areas first (Canada's VO, VY).
    for (const auto &r : REGIONS) {
        if (country != r.country || r.area != area || !r.prefixes[0]) continue;
        if (starts_with_any(prefix, r.prefixes)) {
            name = r.name;
            pos  = {r.lat, r.lon};
            return true;
        }
    }
    for (const auto &r : REGIONS) {
        if (country != r.country || r.area != area || r.prefixes[0]) continue;
        // A prefix with its own area table never falls through to the general one.
        bool own = false;
        for (const auto &o : REGIONS)
            if (country == o.country && o.prefixes[0] && starts_with_any(prefix, o.prefixes)) own = true;
        if (own) return false;
        name = r.name;
        pos  = {r.lat, r.lon};
        return true;
    }
    return false;
}

bool CountryFile::load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

bool CountryFile::parse(std::string_view text) {
    entities_.clear();
    entries_.clear();
    // Records end with ';': "Name: CQ: ITU: Cont: Lat: Lon(+W): UTC: Prefix:"
    // then the prefixes and exact calls ("=CALL"), comma-separated, each
    // with optional overrides (CQ) [ITU] <lat/lon> {continent} ~utc~.
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(';', pos);
        if (end == std::string_view::npos) break;
        std::string_view rec = text.substr(pos, end - pos);
        pos                  = end + 1;

        std::vector<std::string> fields;
        size_t                   p = 0;
        for (int i = 0; i < 8; i++) {
            size_t c = rec.find(':', p);
            if (c == std::string_view::npos) break;
            fields.push_back(trim(rec.substr(p, c - p)));
            p = c + 1;
        }
        if (fields.size() < 8) continue;
        Entity e;
        e.name      = fields[0];
        e.cq        = std::atoi(fields[1].c_str());
        e.itu       = std::atoi(fields[2].c_str());
        e.continent = fields[3];
        e.pos       = {std::atof(fields[4].c_str()), -std::atof(fields[5].c_str())}; // cty.dat: west positive
        e.prefix    = fields[7][0] == '*' ? fields[7].substr(1) : fields[7];
        int index   = (int)entities_.size();
        entities_.push_back(e);

        std::string_view list = rec.substr(p);
        size_t           q    = 0;
        while (q < list.size()) {
            size_t comma = list.find(',', q);
            if (comma == std::string_view::npos) comma = list.size();
            std::string tok = trim(list.substr(q, comma - q));
            q               = comma + 1;
            if (tok.empty()) continue;
            Entry  en;
            en.entity = index;
            bool   exact = tok[0] == '=';
            size_t k     = exact ? 1 : 0;
            size_t name_end = tok.find_first_of("([<{~", k);
            std::string key = tok.substr(k, name_end == std::string::npos ? std::string::npos : name_end - k);
            for (size_t o = name_end; o != std::string::npos && o < tok.size();) {
                char   open = tok[o], close = open == '(' ? ')' : open == '[' ? ']' : open == '<' ? '>' : open == '{' ? '}' : '~';
                size_t c = tok.find(close, o + 1);
                if (c == std::string::npos) break;
                std::string v = tok.substr(o + 1, c - o - 1);
                if (open == '(') en.cq = std::atoi(v.c_str());
                else if (open == '[') en.itu = std::atoi(v.c_str());
                else if (open == '{') en.continent = v;
                else if (open == '<') {
                    size_t slash = v.find('/');
                    if (slash != std::string::npos)
                        en.pos = geo::LatLon{std::atof(v.substr(0, slash).c_str()), -std::atof(v.substr(slash + 1).c_str())};
                }
                o = tok.find_first_of("([<{~", c + 1);
            }
            if (key.empty()) continue;
            entries_[(exact ? "=" : "") + upper(key)] = en;
        }
    }
    return !entities_.empty();
}

std::optional<CallsignPlace> CountryFile::find(std::string_view call_in) const {
    std::string call = upper(trim(call_in));
    if (call.empty() || entities_.empty()) return std::nullopt;

    const Entry *en = nullptr;
    auto         ex = entries_.find("=" + call);
    if (ex != entries_.end()) en = &ex->second;
    std::string prefix = effective_prefix(call);
    if (!en) {
        for (std::string cand = prefix; !cand.empty(); cand.pop_back()) {
            auto it = entries_.find(cand);
            if (it != entries_.end()) {
                en = &it->second;
                break;
            }
        }
    }
    if (!en) return std::nullopt;

    const Entity *e = &entities_[en->entity];
    // Desktop's fixup: KG4 calls other than 2x1 and 2x3 are mainland US,
    // not Guantanamo Bay (measured on the base call, so "KG4AB/P" stays).
    std::string base = base_callsign(call); // classify.hpp: the longest part with a digit
    if (e->name == "Guantanamo Bay" && base.rfind("KG4", 0) == 0 && base.size() != 5 && base.size() != 3) {
        for (const auto &o : entities_)
            if (o.name == "United States") {
                e  = &o;
                en = nullptr;
                break;
            }
    }

    CallsignPlace pl;
    pl.country   = e->name;
    pl.continent = en && !en->continent.empty() ? en->continent : e->continent;
    pl.cq_zone   = en && en->cq ? en->cq : e->cq;
    pl.itu_zone  = en && en->itu ? en->itu : e->itu;
    if (en && en->pos) {
        pl.pos = *en->pos; // cty.dat's own position for this prefix or call
        return pl;
    }
    pl.pos = e->pos;
    std::string name;
    geo::LatLon rpos;
    if (call_area_region(e->name, prefix, call_area(call), name, rpos)) {
        pl.region = name;
        pl.pos    = rpos;
    }
    return pl;
}

std::optional<std::string> CountryFile::continent_near(geo::LatLon p) const {
    const Entity *best = nullptr;
    double        best_km = 0;
    for (const auto &e : entities_) {
        double km = geo::distance_km(p, e.pos);
        if (!best || km < best_km) {
            best    = &e;
            best_km = km;
        }
    }
    if (!best) return std::nullopt;
    return best->continent;
}

} // namespace x6100::js8
