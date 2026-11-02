/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 data files on the SD card's DATA partition
 *
 *  The inbox, the held messages and js8_texts.txt are small text files on
 *  FAT32. Writing goes through a temporary file, so a power cut leaves the
 *  old version or the new one; reading puts back a save cut short, and
 *  moves a file that can't be read out of the way instead of letting the
 *  next save write over it.
 */

#pragma once

#include <string>

namespace x6100::js8 {

/// A data file, as read.
struct DataFile {
    bool        exists   = false; ///< read (or put back from a save cut short)
    bool        writable = true;  ///< false: unreadable and couldn't be moved aside: never write over it
    std::string text;             ///< its contents; empty when missing or unreadable
    std::string notice;           ///< what went wrong, for the screen; empty when all is well
};

/// Read `path`. Missing, but a `.tmp` is there (a save cut short between
/// writing it and renaming it): the `.tmp` is put back and read. There but
/// unreadable: renamed to `path.unreadable-YYYYMMDD-HHMMSSZ` (kept for the
/// user, and the next save starts a new file); if even that fails, not
/// writable.
DataFile read_data_file(const std::string &path);

/// Write `text` to `path` safely: `path.tmp`, fsync, rename over `path`,
/// fsync the directory. False (and `path` untouched) on any error.
bool write_data_file(const std::string &path, const std::string &text);

} // namespace x6100::js8
