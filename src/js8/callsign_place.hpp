/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 map: where a callsign is, from its prefix
 *
 *  For a station heard without a grid: its country and continent from AD1C's
 *  country file (cty.dat, the one desktop JS8Call and WSJT-X ship), found as
 *  desktop's logbook finds it, and where the call area says more (Canada's
 *  provinces, US call areas, Australian states, Japanese areas) the centre
 *  of that region (docs/MAP_PLAN.md).
 */

#pragma once

#include "geo.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace x6100::js8 {

/// Where a callsign places a station.
struct CallsignPlace {
    std::string  country;       ///< cty.dat entity, e.g. "Canada"
    std::string  continent;     ///< "NA", "SA", "EU", "AF", "AS", "OC", "AN"
    std::string  region;        ///< e.g. "British Columbia", "US call area 6"; empty: the country
    geo::LatLon  pos;           ///< the region's centre, else the country's (cty.dat)
    int          cq_zone  = 0;
    int          itu_zone = 0;
};

/// AD1C's country file, parsed.
class CountryFile {
public:
    /// Parse cty.dat text. False if it holds no entity.
    bool parse(std::string_view text);
    /// Read and parse a cty.dat file.
    bool load(const std::string &path);

    bool   loaded() const { return !entities_.empty(); }
    size_t entity_count() const { return entities_.size(); }

    /// The place of a callsign (any case, with /P, /6, W6/ ... as heard), or
    /// nullopt if no prefix matches.
    std::optional<CallsignPlace> find(std::string_view call) const;

    /// The continent of the country whose centre is nearest `p`: yours from
    /// your grid when your callsign's prefix is unknown.
    std::optional<std::string> continent_near(geo::LatLon p) const;

private:
    struct Entity {
        std::string name, continent, prefix;
        int         cq = 0, itu = 0;
        geo::LatLon pos;
    };
    /// A prefix or exact call ("=CALL") with the overrides cty.dat allows.
    struct Entry {
        int                        entity = -1;
        int                        cq = 0, itu = 0; ///< 0: the entity's
        std::string                continent;       ///< empty: the entity's
        std::optional<geo::LatLon> pos;
    };
    std::vector<Entity>                    entities_;
    std::unordered_map<std::string, Entry> entries_; ///< key: prefix, or "=" + exact call
};

/// Desktop JS8Call's Radio::effective_prefix(): the part of a call that says
/// where it's from ("W6/VE7NHW" -> "W6", "VE7NHW/P" -> "VE7NHW",
/// "VE7NHW/W6" -> "W6"); suffixes /P, /M, /MM, /QRP, a digit ... are ignored.
std::string effective_prefix(std::string_view call);

/// The call area digit, or -1: a single-digit suffix ("KG4UHM/6" -> 6),
/// else the last digit of the effective prefix's own prefix ("VE7NHW" -> 7,
/// "7K1ABC" -> 1, "W6" -> 6).
int call_area(std::string_view call);

/// The region a country's call area stands for, if we know one: `country`
/// as cty.dat names it. False if the country or digit has no region.
bool call_area_region(const std::string &country, std::string_view prefix, int area, std::string &name,
                      geo::LatLon &pos);

} // namespace x6100::js8
