/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 station list (desktop JS8Call's Call Activity)
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace x6100::js8 {

struct Station {
    std::string  call;
    std::string  grid;           ///< latest heard (heartbeat, CQ or GRID)
    std::int64_t heard_ms = 0;   ///< last decoded, wall clock ms
    int          snr      = 0;   ///< how we hear them (last decode)
    float        freq_hz  = 0;   ///< their last audio offset
    int          mode     = 0;   ///< speed last heard at (varicode submode 0/1/2/4)
    bool         heard_me = false;       ///< they have sent us something
    bool         aprs_gate = false;      ///< passed an APRS message back over JS8 ("@APRSIS MSG TO:"): a two-way gateway
    std::int64_t heard_me_ms = 0;
    std::optional<int> reported_snr;     ///< how they hear us, if they said
    std::string  via;            ///< only heard through this relay station (desktop's "through")
};

/// One decoded message, as the station list needs it.
struct StationEvent {
    std::string  from, to, text;
    bool         to_me = false;
    int          snr   = 0;
    float        freq_hz = 0;
    int          mode    = 0;    ///< speed (varicode submode 0/1/2/4)
    std::int64_t when_ms = 0;
};

/// Stations heard, with those that heard us first, like desktop JS8Call's
/// Call Activity with its ★ ("Hearing Your Station").
class StationList {
public:
    static constexpr std::int64_t EXPIRE_MS = 60 * 60 * 1000;

    void add(const StationEvent &ev, const std::string &my_call);

    /// `call` named in a relay that reached us through `via` (desktop puts
    /// a relay path's stations in its Call Activity). A station heard
    /// directly within `expire_ms` keeps what we heard of it.
    void add_via(const std::string &call, const std::string &via, float freq_hz, int mode, std::int64_t when_ms,
                 std::int64_t expire_ms = EXPIRE_MS);

    /// Stations heard within `expire_ms` (0: all): those that heard us
    /// first (most recent first), then the rest by most recently heard.
    std::vector<Station> sorted(std::int64_t now_ms, std::int64_t expire_ms = EXPIRE_MS) const;

    /// The `max` most recently heard within `expire_ms` (0: all), most
    /// recent first; copies only those.
    std::vector<Station> recent(std::int64_t now_ms, std::size_t max, std::int64_t expire_ms = EXPIRE_MS) const;

    /// One station, if heard within `expire_ms` (0: any time).
    const Station *find(const std::string &call, std::int64_t now_ms, std::int64_t expire_ms = EXPIRE_MS) const;

    /// Forget stations not heard within `expire_ms` (0: keep all). They
    /// were only hidden before, and every read copied them all.
    void expire(std::int64_t now_ms, std::int64_t expire_ms);

    /// Heard directly since the list was made or reset, even if it has
    /// expired since or the list was cleared: a "new station" is one not
    /// heard before.
    bool heard_before(const std::string &call) const { return heard_.count(call) > 0; }

    /// The Clear button: empty the list, keep what was heard before.
    void clear() { stations_.clear(); }
    /// Start afresh (the list is reused for another frequency).
    void reset() {
        stations_.clear();
        heard_.clear();
    }

    std::size_t size() const { return stations_.size(); }

private:
    std::map<std::string, Station> stations_;
    std::set<std::string>          heard_;
};

} // namespace x6100::js8
