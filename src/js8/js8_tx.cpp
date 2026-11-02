/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 transmit, C interface for the dialog
 */

#include "js8_tx.h"

#include "tx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>

using namespace x6100::js8;

struct js8_tx {
    js8_tx_cb_t                  cb{};
    double                       synth_hz = 0;
    std::unique_ptr<Transmitter> tx;
};

namespace {

void copy_str(char *dst, std::size_t cap, const std::string &src) {
    if (!dst || cap == 0) return;
    std::size_t n = std::min(cap - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

} // namespace

extern "C" void js8_tx_preview(const char *my_call, const char *my_grid, const char *text, js8_speed_t speed,
                               js8_tx_preview_t *out) {
    if (!out) return;
    *out      = js8_tx_preview_t{};
    auto plan = plan_message(my_call ? my_call : "", my_grid ? my_grid : "", text ? text : "", speed);
    out->ok      = plan.ok();
    out->frames  = (int)plan.frames.size();
    out->seconds = (float)plan.seconds();
    copy_str(out->preview, sizeof(out->preview), plan.preview);
    copy_str(out->error, sizeof(out->error), plan.error);
}

extern "C" bool js8_tx_sendable_char(char c) {
    return is_sendable_char(c);
}

extern "C" js8_tx_t *js8_tx_create(int rate, float synth_hz, const js8_tx_cb_t *cb) {
    auto *t = new (std::nothrow) js8_tx;
    if (!t) return nullptr;
    if (cb) t->cb = *cb;
    t->synth_hz = synth_hz;

    Transmitter::Callbacks callbacks;
    callbacks.play = [t](const std::vector<float> &audio, const TxFrame &, int index, int count) {
        if (!t->cb.play) return true;
        // Same level as the FT8 app's GFSK synthesiser.
        std::vector<int16_t> pcm(audio.size());
        for (std::size_t i = 0; i < audio.size(); i++)
            pcm[i] = (int16_t)std::lrintf(std::clamp(audio[i], -1.0f, 1.0f) * 32767.0f * 0.8f);
        return t->cb.play(pcm.data(), (unsigned)pcm.size(), index, count, t->cb.ctx);
    };
    callbacks.on_status = [t](const Transmitter::Status &s) {
        if (!t->cb.on_status) return;
        js8_tx_status_t st{};
        st.state     = s.state == Transmitter::State::Keying    ? JS8_TX_KEYING
                       : s.state == Transmitter::State::Waiting ? JS8_TX_WAITING
                                                                 : JS8_TX_IDLE;
        st.frame     = s.frame;
        st.frames    = s.frames;
        st.next_ms   = s.next_ms;
        st.offset_hz = (float)s.offset_hz;
        st.speed     = s.speed;
        copy_str(st.text, sizeof(st.text), s.text);
        t->cb.on_status(&st, t->cb.ctx);
    };
    callbacks.on_done = [t](const std::string &text, bool completed) {
        if (t->cb.on_done) t->cb.on_done(text.c_str(), completed, t->cb.ctx);
    };

    try {
        t->tx = std::make_unique<Transmitter>(rate, std::move(callbacks));
    } catch (...) {
        delete t;
        return nullptr;
    }
    return t;
}

extern "C" bool js8_tx_send(js8_tx_t *t, const char *my_call, const char *my_grid, const char *text, float offset_hz,
                            js8_speed_t speed, char *err, unsigned err_len) {
    if (!t) return false;
    auto        plan = plan_message(my_call ? my_call : "", my_grid ? my_grid : "", text ? text : "", speed);
    std::string why;
    if (!t->tx->send(plan, offset_hz, &why, t->synth_hz)) {
        copy_str(err, err_len, why);
        return false;
    }
    return true;
}

extern "C" void js8_tx_stop(js8_tx_t *t) {
    if (t) t->tx->stop();
}

extern "C" bool js8_tx_busy(js8_tx_t *t) {
    return t && t->tx->busy();
}

extern "C" bool js8_tx_stopping(js8_tx_t *t) {
    return t && t->tx->stopping();
}

extern "C" void js8_tx_destroy(js8_tx_t *t) {
    if (!t) return;
    // Stop and wait for the TX thread while t->tx is still set: a frame on
    // the air polls js8_tx_stopping(t) until it sees the stop. reset() alone
    // cleared the pointer before ~Transmitter() joined, and that poll then
    // crashed the app with the radio keyed.
    t->tx->stop();
    t->tx->join();
    t->tx.reset();
    delete t;
}
