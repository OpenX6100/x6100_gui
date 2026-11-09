/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - tests for the shared ADIF record writer
 *  (src/adif_rec.c)
 */

extern "C" {
#include "adif_rec.h"
}

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {
/* 2026-11-08 18:23:45 UTC */
const time_t T = 1794162225;

std::string slurp(const char *path) {
    std::ifstream     f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
} // namespace

TEST_CASE("ADIF record fields", "[adif_rec]") {
    char       buf[512];
    adif_rec_t r;

    adif_rec_begin(&r, buf, sizeof(buf));
    adif_rec_str(&r, "call", "W1AW");
    adif_rec_str(&r, "name", "");           /* empty: left out */
    adif_rec_str(&r, "qth", nullptr);
    adif_rec_mode(&r, MODE_FT4, nullptr);
    adif_rec_times(&r, T - 125, T);
    adif_rec_freq(&r, 7047500);
    adif_rec_int(&r, "stx", 7);
    size_t n = adif_rec_end(&r);
    CHECK(n == strlen(buf));
    CHECK(std::string(buf) ==
          "<call:4>W1AW <mode:4>MFSK <submode:3>FT4 "
          "<qso_date:8>20261108 <time_on:6>182140 <qso_date_off:8>20261108 <time_off:6>182345 "
          "<band:3>40m <freq:8>7.047500 <stx:1>7 <eor>\n");
}

TEST_CASE("ADIF record that does not fit", "[adif_rec]") {
    char       buf[40];
    adif_rec_t r;

    adif_rec_begin(&r, buf, sizeof(buf));
    adif_rec_str(&r, "call", "W1AW");
    adif_rec_str(&r, "comment", "a comment far too long for the buffer it goes in");
    adif_rec_str(&r, "name", "Hiram");
    CHECK(adif_rec_end(&r) == 0);
    CHECK(buf[0] == '\0');                  /* never half a record */

    adif_rec_begin(&r, buf, 0);
    CHECK(adif_rec_end(&r) == 0);
}

TEST_CASE("ADIF modes, both ways", "[adif_rec]") {
    const qso_log_mode_t all[] = {MODE_SSB, MODE_AM,   MODE_FM,  MODE_CW,    MODE_FT8,   MODE_FT4,
                                  MODE_FT2, MODE_JS8,  MODE_RTTY, MODE_JTTY, MODE_PSK31, MODE_OLIVIA};
    const char *m, *s;

    for (auto mode : all) {
        adif_mode_of(mode, &m, &s);
        REQUIRE(m != nullptr);
        CHECK(adif_mode_parse(m, s) == mode);
    }
    adif_mode_of(MODE_PSK31, &m, &s);
    CHECK(std::string(m) == "PSK");
    CHECK(std::string(s) == "PSK31");
    adif_mode_of(MODE_OTHER, &m, &s);
    CHECK(m == nullptr);

    CHECK(adif_mode_parse("OLIVIA", "OLIVIA 32/1000") == MODE_OLIVIA);
    CHECK(adif_mode_parse("SSB", "USB") == MODE_SSB);
    CHECK(adif_mode_parse("PSK31", nullptr) == MODE_PSK31);   /* before ADIF 3.0 */
    CHECK(adif_mode_parse("FT4", nullptr) == MODE_FT4);
    CHECK(adif_mode_parse("mfsk", "ft4") == MODE_FT4);
    CHECK(adif_mode_parse("MFSK", "Q65") == MODE_OTHER);
    CHECK(adif_mode_parse("PSK", "PSK63") == MODE_OTHER);
    CHECK(adif_mode_parse(nullptr, nullptr) == MODE_OTHER);

    CHECK(adif_mode_by_name("FT4") == MODE_FT4);
    CHECK(adif_mode_by_name("FT8") == MODE_FT8);
    CHECK(adif_mode_by_name("WSPR") == MODE_OTHER);
}

TEST_CASE("ADIF mode by the apps' name", "[adif_rec]") {
    char       buf[128];
    adif_rec_t r;

    adif_rec_begin(&r, buf, sizeof(buf));
    adif_rec_mode_name(&r, "FT4");
    adif_rec_mode_name(&r, "Q65");          /* not in the table: as given */
    adif_rec_mode_name(&r, "");             /* nothing to write */
    adif_rec_end(&r);
    CHECK(std::string(buf) == "<mode:4>MFSK <submode:3>FT4 <mode:3>Q65 <eor>\n");
}

TEST_CASE("ADIF olivia submode", "[adif_rec]") {
    char       buf[128];
    adif_rec_t r;

    adif_rec_begin(&r, buf, sizeof(buf));
    adif_rec_mode(&r, MODE_OLIVIA, "OLIVIA 32/1000");
    adif_rec_end(&r);
    CHECK(std::string(buf) == "<mode:6>OLIVIA <submode:14>OLIVIA 32/1000 <eor>\n");
}

TEST_CASE("ADIF band names", "[adif_rec]") {
    CHECK(std::string(adif_band_name(1838000)) == "160m");
    CHECK(std::string(adif_band_name(7300000)) == "40m");
    CHECK(std::string(adif_band_name(14070000)) == "20m");
    CHECK(std::string(adif_band_name(50313000)) == "6m");
    CHECK(std::string(adif_band_name(9000000)) == "");
}

TEST_CASE("ADIF file: the header once", "[adif_rec]") {
    char path[] = "/tmp/test_adif_rec_XXXXXX";
    int  fd = mkstemp(path);

    REQUIRE(fd >= 0);
    close(fd);                              /* empty: gets the header */
    CHECK(adif_rec_append(path, "X6100 test log", "<call:4>W1AW <eor>\n"));
    CHECK(adif_rec_append(path, "X6100 test log", "<call:5>K1ABC <eor>\n"));
    CHECK(slurp(path) == "X6100 test log\n" ADIF_HEADER "<call:4>W1AW <eor>\n<call:5>K1ABC <eor>\n");
    unlink(path);

    CHECK_FALSE(adif_rec_append("/nonexistent/dir/log.adi", "x", "<eor>\n"));
}
