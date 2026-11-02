/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 receive
 */

#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace x6100::js8 {

namespace {

std::uint16_t le16(const unsigned char *p) { return (std::uint16_t)(p[0] | (p[1] << 8)); }
std::uint32_t le32(const unsigned char *p) {
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}
void put16(unsigned char *p, std::uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
void put32(unsigned char *p, std::uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xff; }

struct FileCloser {
    void operator()(FILE *f) const { if (f) std::fclose(f); }
};

} // namespace

bool read_wav(const std::string &path, WavAudio &out, std::string &error) {
    std::unique_ptr<FILE, FileCloser> f(std::fopen(path.c_str(), "rb"));
    if (!f) {
        error = "cannot open " + path;
        return false;
    }

    unsigned char hdr[12];
    if (std::fread(hdr, 1, 12, f.get()) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) {
        error = "not a RIFF/WAVE file";
        return false;
    }

    int  channels = 0, bits = 0;
    bool have_fmt = false;

    unsigned char chunk[8];
    while (std::fread(chunk, 1, 8, f.get()) == 8) {
        std::uint32_t size = le32(chunk + 4);

        if (!std::memcmp(chunk, "fmt ", 4)) {
            unsigned char fmt[16];
            if (size < 16 || std::fread(fmt, 1, 16, f.get()) != 16) {
                error = "bad fmt chunk";
                return false;
            }
            std::uint16_t format = le16(fmt);
            channels             = le16(fmt + 2);
            out.rate             = (int)le32(fmt + 4);
            bits                 = le16(fmt + 14);
            // WAVE_FORMAT_EXTENSIBLE carries PCM too; its subformat follows.
            if (format != 1 && format != 0xFFFE) {
                error = "only PCM WAV is supported";
                return false;
            }
            if (bits != 16 || channels < 1 || out.rate <= 0) {
                error = "need 16-bit PCM";
                return false;
            }
            have_fmt = true;
            std::fseek(f.get(), (long)(size - 16 + (size & 1)), SEEK_CUR);
        } else if (!std::memcmp(chunk, "data", 4)) {
            if (!have_fmt) {
                error = "data chunk before fmt chunk";
                return false;
            }
            std::size_t frames = size / (2u * channels);
            std::vector<std::int16_t> pcm(frames * channels);
            std::size_t got = std::fread(pcm.data(), 2 * channels, frames, f.get());
            out.samples.resize(got);
            for (std::size_t i = 0; i < got; i++) out.samples[i] = pcm[i * channels] / 32768.0f;
            return true;
        } else {
            std::fseek(f.get(), (long)(size + (size & 1)), SEEK_CUR);
        }
    }

    error = "no data chunk";
    return false;
}

bool write_wav(const std::string &path, int rate, const std::vector<float> &samples) {
    std::unique_ptr<FILE, FileCloser> f(std::fopen(path.c_str(), "wb"));
    if (!f) return false;

    const std::uint32_t data_bytes = (std::uint32_t)(samples.size() * 2);
    unsigned char       h[44];
    std::memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);            // PCM
    put16(h + 22, 1);            // mono
    put32(h + 24, (std::uint32_t)rate);
    put32(h + 28, (std::uint32_t)rate * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
    if (std::fwrite(h, 1, 44, f.get()) != 44) return false;

    std::vector<std::int16_t> pcm(samples.size());
    for (std::size_t i = 0; i < samples.size(); i++)
        // Same 32768 scale as read_wav(), so values round-trip exactly.
        pcm[i] = (std::int16_t)std::clamp(std::lrintf(samples[i] * 32768.0f), -32768L, 32767L);
    return std::fwrite(pcm.data(), 2, pcm.size(), f.get()) == pcm.size();
}

} // namespace x6100::js8
