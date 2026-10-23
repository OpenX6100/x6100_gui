/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 data files on the SD card's DATA partition
 */

#include "datafile.hpp"

#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

namespace x6100::js8 {

namespace {

bool exists(const std::string &path) {
    return access(path.c_str(), F_OK) == 0;
}

std::string base_name(const std::string &path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string dir_name(const std::string &path) {
    auto slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

// The whole file, or false if it can't be opened or a read fails part way
// (a half-read inbox saved back would lose the rest).
bool read_all(const std::string &path, std::string &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char   buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

} // namespace

DataFile read_data_file(const std::string &path) {
    DataFile          d;
    const std::string tmp = path + ".tmp";
    // A save cut short: the .tmp was complete (it's renamed only after its
    // fsync) but FAT's rename over the old file didn't finish.
    if (!exists(path) && exists(tmp)) std::rename(tmp.c_str(), path.c_str());
    if (!exists(path)) return d; // no file yet: empty

    if (read_all(path, d.text)) {
        d.exists = true;
        return d;
    }
    d.text.clear();

    // There but unreadable: keep it for the user under another name, so
    // the next save can't write over what's in it.
    char        stamp[32];
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
    gmtime_r(&now, &tm);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%SZ", &tm);
    const std::string aside = path + ".unreadable-" + stamp;
    if (std::rename(path.c_str(), aside.c_str()) == 0) {
        d.notice = base_name(path) + " couldn't be read: kept as " + base_name(aside) + ", starting a new one";
    } else {
        d.writable = false;
        d.notice   = base_name(path) + " can't be read or moved: not saving to it (check the SD card)";
    }
    return d;
}

bool write_data_file(const std::string &path, const std::string &text) {
    const std::string tmp = path + ".tmp";
    FILE             *f   = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok      = ok && std::fflush(f) == 0;
    ok      = ok && fsync(fileno(f)) == 0;
    ok      = std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    // The rename is on the card only once its directory is.
    int dir = open(dir_name(path).c_str(), O_RDONLY | O_DIRECTORY);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
    return true;
}

} // namespace x6100::js8
