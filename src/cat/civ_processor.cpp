/*
 * X6100 CI-V command handlers — new dispatch using CivPacketView/CivTxPacker.
 *
 * Zero-copy replacement for Frame::process(). All 18 handlers from
 * cat_frame.cpp are adapted here. When migration is complete, cat_frame.cpp
 * and cat_internal.h can be removed.
 */

#include "civ_processor.h"
#include "civ_internal.h"
#include "../cfg/cfg_api.h"
#include "scope_streamer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "lvgl/lvgl.h"

// Application ports injected via civ_set_ports() (cat_init calls it).
static const app_ports_t *g_ports = nullptr;

namespace {

// ============================================================================
// Constants
// ============================================================================

constexpr uint8_t C_SND_FREQ = 0x00;      /* Send frequency data */
constexpr uint8_t C_SND_MODE = 0x01;      /* Send mode data, Sc  for transceive mode does not ack */
constexpr uint8_t C_RD_BAND = 0x02;       /* Read band edge frequencies */
constexpr uint8_t C_RD_FREQ = 0x03;       /* Read display frequency */
constexpr uint8_t C_RD_MODE = 0x04;       /* Read display mode */
constexpr uint8_t C_SET_FREQ = 0x05;      /* Set frequency data(1) */
constexpr uint8_t C_SET_MODE = 0x06;      /* Set mode data, Sc */
constexpr uint8_t C_SET_VFO = 0x07;       /* Set VFO */
constexpr uint8_t C_SET_MEM = 0x08;       /* Set channel, Sc(2) */
constexpr uint8_t C_WR_MEM = 0x09;        /* Write memory */
constexpr uint8_t C_MEM2VFO = 0x0a;       /* Memory to VFO */
constexpr uint8_t C_CLR_MEM = 0x0b;       /* Memory clear */
constexpr uint8_t C_RD_OFFS = 0x0c;       /* Read duplex offset frequency; default changes with HF/6M/2M */
constexpr uint8_t C_SET_OFFS = 0x0d;      /* Set duplex offset frequency */
constexpr uint8_t C_CTL_SCAN = 0x0e;      /* Control scan, Sc */
constexpr uint8_t C_CTL_SPLT = 0x0f;      /* Control split, and duplex mode Sc */
constexpr uint8_t C_SET_TS = 0x10;        /* Set tuning step, Sc */
constexpr uint8_t C_CTL_ATT = 0x11;       /* Set/get attenuator, Sc */
constexpr uint8_t C_CTL_ANT = 0x12;       /* Set/get antenna, Sc */
constexpr uint8_t C_CTL_ANN = 0x13;       /* Control announce (speech synth.), Sc */
constexpr uint8_t C_CTL_LVL = 0x14;       /* Set AF/RF/squelch, Sc */
constexpr uint8_t C_RD_SQSM = 0x15;       /* Read squelch condition/S-meter level, Sc */
constexpr uint8_t C_CTL_FUNC = 0x16;      /* Function settings (AGC,NB,etc.), Sc */
constexpr uint8_t C_SND_CW = 0x17;        /* Send CW message */
constexpr uint8_t C_SET_PWR = 0x18;       /* Set Power ON/OFF, Sc */
constexpr uint8_t C_RD_TRXID = 0x19;      /* Read transceiver ID code */
constexpr uint8_t C_CTL_MEM = 0x1a;       /* Misc memory/bank/rig control functions, Sc */
constexpr uint8_t C_SET_TONE = 0x1b;      /* Set tone frequency */
constexpr uint8_t C_CTL_PTT = 0x1c;       /* Control Transmit On/Off, Sc */
constexpr uint8_t C_CTL_EDGE = 0x1e;      /* Band edges */
constexpr uint8_t C_CTL_DVT = 0x1f;       /* Digital modes calsigns & messages */
constexpr uint8_t C_CTL_DIG = 0x20;       /* Digital modes settings & status */
constexpr uint8_t C_CTL_RIT = 0x21;       /* RIT/XIT control */
constexpr uint8_t C_CTL_DSD = 0x22;       /* D-STAR Data */
constexpr uint8_t C_SEND_SEL_FREQ = 0x25; /* Send/Recv sel/unsel VFO frequency */
constexpr uint8_t C_SEND_SEL_MODE = 0x26;
constexpr uint8_t C_CTL_SCP = 0x27;   /* Scope control & data */
constexpr uint8_t C_SND_VOICE = 0x28; /* Transmit Voice Memory Contents */
constexpr uint8_t C_CTL_MTEXT = 0x70; /* Microtelecom Extension */
constexpr uint8_t C_CTL_MISC = 0x7f;  /* Miscellaneous control, Sc */

constexpr uint8_t CODE_OK = 0xFB;
constexpr uint8_t CODE_NG = 0xFA;

constexpr uint8_t S_VFOA = 0x00;
constexpr uint8_t S_VFOB = 0x01;
constexpr uint8_t S_BTOA = 0xa0;
constexpr uint8_t S_XCHNG = 0xb0;

constexpr uint8_t M_LSB = 0x00;
constexpr uint8_t M_USB = 0x01;
constexpr uint8_t M_AM  = 0x02;
constexpr uint8_t M_CW  = 0x03;
constexpr uint8_t M_NFM = 0x05;
constexpr uint8_t M_CWR = 0x07;

constexpr uint8_t MEM_BS_REG = 0x01;
constexpr uint8_t MEM_IF_FW  = 0x03;
constexpr uint8_t MEM_LOCK   = 0x05;
constexpr uint8_t MEM_DM_FG  = 0x06;

} // anonymous namespace

namespace civ::detail {

// ============================================================================
// BCD helpers
// ============================================================================

void to_bcd(uint8_t bcd_data[], uint64_t data, uint8_t len) {
    int16_t i;

    for (i = 0; i < len / 2; i++) {
        uint8_t a = data % 10;

        data /= 10;
        a |= (data % 10) << 4;
        data /= 10;
        bcd_data[i] = a;
    }

    if (len & 1) {
        bcd_data[i] &= 0x0f;
        bcd_data[i] |= data % 10;
    }
}

void to_bcd_be(uint8_t bcd_data[], uint64_t data, uint8_t len) {
    for (int16_t i = (len - 1) / 2; i >= 0; i--) {
        uint8_t a = data % 10;

        data /= 10;
        a |= (data % 10) << 4;
        data /= 10;
        bcd_data[i] = a;
    }
}

uint64_t from_bcd(std::string_view bcd_data, uint8_t len) {
    int16_t     i;
    uint64_t    data = 0;

    if (len & 1) {
        data = static_cast<uint8_t>(bcd_data[len / 2]) & 0x0F;
    }

    for (i = (len / 2) - 1; i >= 0; i--) {
        data *= 10;
        data += static_cast<uint8_t>(bcd_data[i]) >> 4;
        data *= 10;
        data += static_cast<uint8_t>(bcd_data[i]) & 0x0F;
    }

    return data;
}

uint64_t from_bcd_be(std::string_view bcd_data, uint8_t len) {
    int16_t     i = 0;
    uint64_t    data = 0;

    if (len & 1) {
        data = static_cast<uint8_t>(bcd_data[0]) & 0x0F;
        i++;
    }

    for (; i <= (len / 2); i++) {
        data *= 10;
        data += static_cast<uint8_t>(bcd_data[i]) >> 4;
        data *= 10;
        data += static_cast<uint8_t>(bcd_data[i]) & 0x0F;
    }

    return data;
}

} // namespace civ::detail

namespace {
using namespace civ::detail;

// ============================================================================
// Mode helpers
// ============================================================================

// Set new mode preserving data flag
x6100_mode_t ci_mode_2_x_mode(x6100_mode_t x_mode, uint8_t ci_mode) {
    bool data_mode = (x_mode == x6100_mode_lsb_dig) || (x_mode == x6100_mode_usb_dig);
    switch (ci_mode) {
        case M_LSB:
            return data_mode ? x6100_mode_lsb_dig : x6100_mode_lsb;
        case M_USB:
            return data_mode ? x6100_mode_usb_dig : x6100_mode_usb;
        case M_AM:
            return x6100_mode_am;
        case M_CW:
            return x6100_mode_cw;
        case M_NFM:
            return x6100_mode_nfm;
        case M_CWR:
            return x6100_mode_cwr;
        default:
            return x_mode;
    }
}

x6100_mode_t ci_data_mode_2_x_mode(x6100_mode_t x_mode, uint8_t data_mode) {
    if (data_mode) {
        if (x_mode == x6100_mode_lsb) return x6100_mode_lsb_dig;
        if (x_mode == x6100_mode_usb) return x6100_mode_usb_dig;
    } else {
        if (x_mode == x6100_mode_lsb_dig) return x6100_mode_lsb;
        if (x_mode == x6100_mode_usb_dig) return x6100_mode_usb;
    }
    return x_mode;
}

uint8_t x_mode_2_ci_mode(x6100_mode_t mode) {
    switch (mode) {
        case x6100_mode_lsb_dig:
        case x6100_mode_lsb:
            return M_LSB;
            break;
        case x6100_mode_usb_dig:
        case x6100_mode_usb:
            return M_USB;
            break;
        case x6100_mode_cw:
            return M_CW;
            break;
        case x6100_mode_cwr:
            return M_CWR;
            break;
        case x6100_mode_am:
            return M_AM;
            break;
        case x6100_mode_nfm:
            return M_NFM;
            break;
        default:
            return 0;
            break;
    }
}

uint8_t x_mode_is_data_mode(x6100_mode_t x_mode) {
    return (x_mode == x6100_mode_lsb_dig) || (x_mode == x6100_mode_usb_dig);
}

uint8_t get_if_bandwidth() {
    uint32_t bw = cfg.filter.bw()->get();
    uint8_t val;
    switch (cfg.cur.mode()->get()) {
        case x6100_mode_cw:
        case x6100_mode_cwr:
        case x6100_mode_lsb:
        case x6100_mode_lsb_dig:
        case x6100_mode_usb:
        case x6100_mode_usb_dig:
            if (bw <= 500) {
                val =(bw - 25) / 50;
            } else {
                val = (bw - 50) / 100 + 5;
            }
            break;
        case x6100_mode_am:
        case x6100_mode_nfm:
            val = (bw - 100) / 200;
            break;
        default:
            val = 31;
            break;
    }
    return (val / 10) << 4 | (val % 10);
}

static uint32_t if_bandwidth_from_ci(uint8_t data) {
    switch (cfg.cur.mode()->get()) {
        case x6100_mode_cw:
        case x6100_mode_cwr:
        case x6100_mode_lsb:
        case x6100_mode_lsb_dig:
        case x6100_mode_usb:
        case x6100_mode_usb_dig:
            if (data <= 9) {
                return (uint32_t)data * 50u + 50u;       // 50 ~ 500 Hz
            }
            return (uint32_t)data * 100u - 400u;         // 600 ~ 3600 Hz
        case x6100_mode_am:
        case x6100_mode_nfm:
            return (uint32_t)data * 200u + 200u;         // 200 Hz ~ 10.0 kHz
        default:
            return 2700u;                                 // fallback
    }
}

int32_t freq_step_from_ci(uint8_t val) {
    switch (val) {
        case 0x00:
            return 10;
        case 0x01:
            return 100;
        case 0x02:
            return 500;
        case 0x03:
            return 1000;
        case 0x04:
            return 5000;
    }
    return 500;

}

uint8_t freq_step_to_ci(int32_t val) {
    switch (val) {
        case 1 ... 10:
            return 0x00;
        case 100:
            return 0x01;
        case 500:
            return 0x02;
        case 1000:
            return 0x03;
        case 5000:
            return 0x04;
    }
    return 0x02;
}

uint8_t x_cw_pitch_2_ci(int32_t key_tone) {
    //  0000=300 Hz ~ 0128=600 Hz ~ 0255=900 Hz
    return static_cast<float>(key_tone - 300) * 0.425f;
}

int32_t ci_cw_pitch_2_x(uint8_t val) {
    float tone = 2.3529411764705883f * static_cast<float>(val) + 300;
    // Round to 5
    return std::round(tone / 5.0f) * 5;
}

uint8_t get_ci_band_id() {
    int32_t freq_khz = cfg.cur.fg_freq()->get() / 1000;
    switch (freq_khz) {
        case 1'800 ... 2'000:
            return 0x01;
        case 3'400 ... 4'100:
            return 0x02;
        case 6'900 ... 7'500:
            return 0x03;
        case 9'900 ... 10'500:
            return 0x04;
        case 13'900 ... 14'500:
            return 0x05;
        case 17'900 ... 18'500:
            return 0x06;
        case 20'900 ... 21'500:
            return 0x07;
        case 24'400 ... 25'100:
            return 0x08;
        case 28'000 ... 30'000:
            return 0x09;
        case 50'000 ... 54'000:
            return 0x10;
    }
    // Not in a band
    return 0x15;
}

void set_ci_band(uint8_t band) {
    // Band center frequencies (Hz) from the CI-V band table.
    int32_t freq;
    switch (band) {
        case 0x01:
            freq = 1'900'000;
            break;
        case 0x02:
            freq = 3'500'000;
            break;
        case 0x03:
            freq = 7'100'000;
            break;
        case 0x04:
            freq = 10'100'000;
            break;
        case 0x05:
            freq = 14'200'000;
            break;
        case 0x06:
            freq = 18'100'000;
            break;
        case 0x07:
            freq = 21'200'000;
            break;
        case 0x08:
            freq = 24'900'000;
            break;
        case 0x09:
            freq = 28'400'000;
            break;
        case 0x10:
            freq = 50'000'000;
            break;
        default:
            return;
    }
    cfg.cur.fg_freq()->set(freq);
}

// ============================================================================
// VFO helpers
// ============================================================================

Parameter<int32_t>& vfo_freq(bool fg, int cur_vfo) {
    if (fg)
    {
        return cur_vfo == X6100_VFO_A ? *cfg.band.vfoa_freq() : *cfg.band.vfob_freq();
    }
    else
    {
        return cur_vfo == X6100_VFO_A ? *cfg.band.vfob_freq() : *cfg.band.vfoa_freq();
    }
}

Parameter<int32_t>& vfo_mode(bool fg, int cur_vfo) {
    if (fg)
    {
        return cur_vfo == X6100_VFO_A ? *cfg.band.vfoa_mode() : *cfg.band.vfob_mode();
    }
    else
    {
        return cur_vfo == X6100_VFO_A ? *cfg.band.vfob_mode() : *cfg.band.vfoa_mode();
    }
}

// ============================================================================
// Handler dispatch
// ============================================================================

using cmd_handler_t = std::string_view(*)(const CivPacketView&, CivTxPacker&);

// Forward declarations
std::string_view handle_snd_freq_x00(const CivPacketView&, CivTxPacker&);
std::string_view handle_rd_freq_x03(const CivPacketView&, CivTxPacker&);
std::string_view handle_rd_mode_x04(const CivPacketView&, CivTxPacker&);
std::string_view handle_set_freq_x05(const CivPacketView&, CivTxPacker&);
std::string_view handle_set_mode_x06(const CivPacketView&, CivTxPacker&);
std::string_view handle_set_vfo_x07(const CivPacketView&, CivTxPacker&);
std::string_view handle_ctl_splt_x0f(const CivPacketView&, CivTxPacker&);
std::string_view handle_set_ts_x10(const CivPacketView&, CivTxPacker&);
std::string_view handle_ctl_att_x11(const CivPacketView&, CivTxPacker&);
std::string_view handle_ctl_lvl_x14(const CivPacketView&, CivTxPacker&);
std::string_view handle_rd_sqsm_x15(const CivPacketView&, CivTxPacker&);
std::string_view handle_ctl_func_x16(const CivPacketView&, CivTxPacker&);
std::string_view handle_rd_trxid_x19(const CivPacketView&, CivTxPacker&);
std::string_view handle_ctl_mem_x1a(const CivPacketView&, CivTxPacker&);
std::string_view handle_ctl_ptt_x1c(const CivPacketView&, CivTxPacker&);
std::string_view handle_send_sel_freq_x25(const CivPacketView&, CivTxPacker&);
std::string_view handle_send_sel_mode_x26(const CivPacketView&, CivTxPacker&);
std::string_view handle_ctl_scp_x27(const CivPacketView&, CivTxPacker&);

cmd_handler_t cmd_handlers[256] = {};
bool cmd_handlers_ready = false;

void init_cmd_handlers() {
    if (cmd_handlers_ready) return;
    cmd_handlers_ready = true;
    cmd_handlers[0x00] = handle_snd_freq_x00;
    cmd_handlers[0x03] = handle_rd_freq_x03;
    cmd_handlers[0x04] = handle_rd_mode_x04;
    cmd_handlers[0x05] = handle_set_freq_x05;
    cmd_handlers[0x06] = handle_set_mode_x06;
    cmd_handlers[0x07] = handle_set_vfo_x07;
    cmd_handlers[0x0f] = handle_ctl_splt_x0f;
    cmd_handlers[0x10] = handle_set_ts_x10;
    cmd_handlers[0x11] = handle_ctl_att_x11;
    cmd_handlers[0x14] = handle_ctl_lvl_x14;
    cmd_handlers[0x15] = handle_rd_sqsm_x15;
    cmd_handlers[0x16] = handle_ctl_func_x16;
    cmd_handlers[0x19] = handle_rd_trxid_x19;
    cmd_handlers[0x1a] = handle_ctl_mem_x1a;
    cmd_handlers[0x1c] = handle_ctl_ptt_x1c;
    cmd_handlers[0x25] = handle_send_sel_freq_x25;
    cmd_handlers[0x26] = handle_send_sel_mode_x26;
    cmd_handlers[0x27] = handle_ctl_scp_x27;
    // cmd_handlers[0x27] = scope_streamer_handle_27;
}

// ============================================================================
// 0x00 — Send frequency (set foreground freq from BCD)
// ============================================================================

std::string_view handle_snd_freq_x00(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    if (data_size == 5) {
        cfg.cur.fg_freq()->set(from_bcd(request.get_command_data(), 10));
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x03 — Read display frequency
// ============================================================================

std::string_view handle_rd_freq_x03(const CivPacketView &request, CivTxPacker &resp) {
    uint8_t bcd[5];
    to_bcd(bcd, cfg.cur.fg_freq()->get(), 10);
    return resp.set_command(request.get_command()).append_data(bcd, 5).get_packet();
}

// ============================================================================
// 0x04 — Read display mode
// ============================================================================

std::string_view handle_rd_mode_x04(const CivPacketView &request, CivTxPacker &resp) {
    uint8_t v = x_mode_2_ci_mode((x6100_mode_t)cfg.cur.mode()->get());
    return resp.set_command(request.get_command()).append_byte(v).append_byte(v).get_packet();
}

// ============================================================================
// 0x05 — Set frequency
// ============================================================================

std::string_view handle_set_freq_x05(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    if (data_size == 5) {
        cfg.cur.fg_freq()->set(from_bcd(request.get_command_data(), 10));
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x06 — Set mode
// ============================================================================

std::string_view handle_set_mode_x06(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    if ((data_size >= 1) && (data_size <= 2)) {
        // Payload: mode, filter id
        x6100_mode_t new_mode = static_cast<x6100_mode_t>(cfg.cur.mode()->get());
        new_mode = ci_mode_2_x_mode(new_mode, request.get_subcommand());
        cfg.cur.mode()->set(new_mode);
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x07 — Set VFO
// ============================================================================

std::string_view handle_set_vfo_x07(const CivPacketView &request, CivTxPacker &resp) {
    x6100_vfo_t cur_vfo = (x6100_vfo_t)cfg.band.current_vfo()->get();


    switch (request.get_vfo()) {
        case S_VFOA:
            if (cur_vfo != X6100_VFO_A) {
                cfg.band.current_vfo()->set(X6100_VFO_A);
            }
            return resp.set_ok().get_packet();

        case S_VFOB:
            if (cur_vfo != X6100_VFO_B) {
                cfg.band.current_vfo()->set(X6100_VFO_B);
            }
            return resp.set_ok().get_packet();

        case S_XCHNG:
            cfg.band.current_vfo()->set(
                cur_vfo == X6100_VFO_A ? X6100_VFO_B : X6100_VFO_A);
            return resp.set_ok().get_packet();

        case S_BTOA:
            cfg_band_vfo_copy();
            return resp.set_ok().get_packet();

        case FRAME_END:
            return resp.set_command(request.get_command())
                       .set_vfo(static_cast<uint8_t>(cur_vfo == X6100_VFO_A ? S_VFOA : S_VFOB)).get_packet();

        default:
            return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x0F — Control split
// ============================================================================

std::string_view handle_ctl_splt_x0f(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    if (data_size == 0) {
        return resp.set_command(request.get_command())
            .append_byte(static_cast<uint8_t>(cfg.band.split()->get()))
            .get_packet();
    } else if (data_size == 1) {
        cfg.band.split()->set(request.get_subcommand());
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x10 — Set tuning step
// ============================================================================

std::string_view handle_set_ts_x10(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    if (data_size == 0) {
        return resp.set_command(request.get_command())
            .append_byte(freq_step_to_ci(cfg.mode.freq_step()->get()))
            .get_packet();
    } else if (data_size == 1) {
        cfg.mode.freq_step()->set(freq_step_from_ci(request.get_subcommand()));
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x11 — Get/set attenuator
// ============================================================================

std::string_view handle_ctl_att_x11(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    if (data_size == 0) {
        return resp.set_command(request.get_command())
            .append_byte(static_cast<uint8_t>(cfg.cur.att()->get() * 0x20))
            .get_packet();
    } else if (data_size == 1) {
        cfg.cur.att()->set(request.get_subcommand());
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x14 — Set AF/RF/squelch level
// ============================================================================

std::string_view handle_ctl_lvl_x14(const CivPacketView &request, CivTxPacker &resp) {
    size_t  data_size = request.get_command_data().size();
    uint8_t bcd[2]    = {0, 0};
    uint8_t subcmd    = request.get_subcommand();

    if (data_size == 1) {
        auto after_cmd = resp.set_command(request.get_command()).set_subcommand(subcmd);
        switch (subcmd) {
            case 0x01: // Get AF level
                to_bcd_be(bcd, cfg.volume()->get() * 255 / 55, 3);
                break;
            case 0x02: // Get RF gain
                to_bcd_be(bcd, cfg.rfgain()->get() * 255 / 100, 3);
                break;
            case 0x03: // Get SQL level
                to_bcd_be(bcd, cfg.squelch()->get() * 255 / 100, 3);
                break;
            case 0x07: //  [TWIN PBT] (PBT1) position
            case 0x08: //  [TWIN PBT] (PBT2) position
                return after_cmd.append_byte(0x01).append_byte(0x28).get_packet();
            case 0x09: // cw keytone
                to_bcd_be(bcd, x_cw_pitch_2_ci(cfg.cw.key_tone()->get()), 3);
                break;
            case 0x0a: // Get Tx power
                to_bcd_be(bcd, std::round(cfg.pwr()->get() * 255 / 10), 3);
                break;
            case 0x15: // Get MONI level
                to_bcd_be(bcd, cfg.moni()->get() * 255 / 100, 3);
                break;
            default:
                return civ::detail::set_unsupported(request, resp);
        }
        return after_cmd.append_data(bcd, 2).get_packet();
    } else if (data_size == 3) {
        auto bcd_val = from_bcd_be(request.get_subcommand_data(), 3);
        switch (subcmd) {
            case 0x01: // Set AF level
                cfg.volume()->set(bcd_val * 55 / 255);
                break;
            case 0x02: // Set RF gain
                cfg.rfgain()->set(bcd_val * 100 / 255);
                break;
            case 0x03: // Set SQL level
                cfg.squelch()->set(bcd_val * 100 / 255);
                break;
            case 0x09: // cw keytone
                cfg.cw.key_tone()->set(ci_cw_pitch_2_x(bcd_val));
                break;
            case 0x0a: // Set Tx power
                {
                    float pwr = static_cast<float>(bcd_val) * 10.0f / 255.0f;
                    cfg.pwr()->set(pwr);
                    break;
                }
            case 0x15: // Set MONI level
                cfg.moni()->set(bcd_val * 100 / 255);
                break;
            default:
                return civ::detail::set_unsupported(request, resp);
        }
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x15 — Read squelch condition / S-meter
// ============================================================================

std::string_view handle_rd_sqsm_x15(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    if (data_size == 1) {
        static float   alc, pwr, swr;
        static uint8_t msg_id;
        g_ports->telemetry->tx_info_refresh(&msg_id, &alc, &pwr, &swr);
        uint8_t val;
        uint8_t bcd[2] = {0, 0};
        switch (request.get_subcommand()) {
            case 0x02: // Get S-Meter
                {
                    float db = g_ports->telemetry->s_meter_get_raw_db();
                    val        = db * 0.75f + 96;
                    to_bcd_be(bcd, val, 3);
                    return resp.set_command(request.get_command())
                        .set_subcommand(request.get_subcommand())
                        .append_data(bcd, 2)
                        .get_packet();
                }
            case 0x07: // Read the OVF status (not supported)
                return resp.set_command(request.get_command())
                    .set_subcommand(request.get_subcommand())
                    .append_byte(0x00)
                    .get_packet();
            case 0x11: // Get Power-Meter
                val = -pwr * pwr + 35 * pwr;
                to_bcd_be(bcd, val, 3);
                return resp.set_command(request.get_command()).set_subcommand(request.get_subcommand()).get_packet();
            case 0x12: // Get SWR-Meter
                val = -21 * swr * swr + 134 * swr - 122;
                to_bcd_be(bcd, val, 3);
                return resp.set_command(request.get_command())
                    .set_subcommand(request.get_subcommand())
                    .append_data(bcd, 2)
                    .get_packet();
            case 0x13: // Get ALC-Meter,
                val = alc * 120 / 10;
                to_bcd_be(bcd, val, 3);
                return resp.set_command(request.get_command())
                    .set_subcommand(request.get_subcommand())
                    .append_data(bcd, 2)
                    .get_packet();
            default:
                return resp.set_ng().get_packet();
        }
    } else {
        return civ::detail::set_unsupported(request, resp);
        // return resp.set_ng().get_packet();
    }
}

// ============================================================================
// 0x16 — Function settings (AGC, NB, NR, etc.)
// ============================================================================

std::string_view handle_ctl_func_x16(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    uint8_t subcmd = request.get_subcommand();


    if (data_size == 1) {
        // READ — return current value
        auto after_cmd = resp.set_command(request.get_command()).set_subcommand(subcmd);
        switch (subcmd) {
            case 0x02:
                return after_cmd.append_byte(static_cast<uint8_t>(cfg.cur.pre()->get())).get_packet();
            case 0x22:
                return after_cmd.append_byte(static_cast<uint8_t>(cfg.dsp.nb()->get())).get_packet();
            case 0x40:
                return after_cmd.append_byte(static_cast<uint8_t>(cfg.dsp.nr()->get())).get_packet();
            case 0x44:
                return after_cmd.append_byte(static_cast<uint8_t>(cfg.dsp.comp()->get() > 1)).get_packet();
            case 0x46:
                return after_cmd.append_byte(static_cast<uint8_t>(cfg.vox.on()->get())).get_packet();
            case 0x45: // Monitor on/off
                return after_cmd.append_byte(static_cast<uint8_t>(cfg.moni()->get() > 0)).get_packet();
            case 0x5D: // Tone squelch — unsupported
                return after_cmd.append_byte(0x00).get_packet();
            default:
                return civ::detail::set_unsupported(request, resp);
        }
    } else if (data_size == 2) {
        // WRITE — set value from request
        auto new_val = static_cast<uint8_t>(request.get_subcommand_data()[0]);
        switch (subcmd) {
            case 0x02:
                cfg.cur.pre()->set(new_val > 0);
                break;
            case 0x22:
                cfg.dsp.nb()->set(new_val);
                break;
            case 0x40:
                cfg.dsp.nr()->set(new_val);
                break;
            case 0x44:
                cfg.dsp.comp()->set(new_val ? 4 : 1);
                break;
            case 0x46:
                cfg.vox.on()->set(new_val);
                break;
            case 0x45:
            case 0x5D:
                return resp.set_ng().get_packet();
            default:
                return civ::detail::set_unsupported(request, resp);
        }
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x19 — Read transceiver ID
// ============================================================================

std::string_view handle_rd_trxid_x19(const CivPacketView &request, CivTxPacker &resp) {
    if ((request.get_command_data().size() == 1) && (request.get_subcommand() == 0)) {
        return resp.set_command(request.get_command()).set_subcommand(0x00).append_byte(LOCAL_ADDRESS).get_packet();
    }
    return civ::detail::set_unsupported(request, resp);
}

// ============================================================================
// 0x1A — Misc memory/bank/rig control
// ============================================================================

std::string_view handle_ctl_mem_x1a(const CivPacketView &request, CivTxPacker &resp) {
    size_t       data_size = request.get_command_data().size();
    x6100_mode_t mode      = (x6100_mode_t)cfg.cur.mode()->get();

    if (data_size == 1) {
        auto after_cmd = resp.set_command(request.get_command()).set_subcommand(request.get_subcommand());
        switch (request.get_subcommand()) {
            case MEM_BS_REG:
                return after_cmd.append_byte(get_ci_band_id()).append_byte(0x02).get_packet();

            case MEM_IF_FW:
                return after_cmd.append_byte(get_if_bandwidth()).get_packet();

            case MEM_DM_FG:
                return after_cmd.append_byte(x_mode_is_data_mode(mode)).append_byte(0x01).get_packet();

            default:
                return civ::detail::set_unsupported(request, resp);
        }
    } else {
        switch (request.get_subcommand()) {
            case MEM_BS_REG:
                set_ci_band(request.get_subcommand_data()[0]);
                return resp.set_ok().get_packet();
            // case MEM_IF_FW:
            //     cfg.filter.bw()->set(if_bandwidth_from_ci(request.get_subcommand_data()[0]));
            //     return resp.set_ok().get_packet();
            case MEM_LOCK:
                return resp.set_ng().get_packet();
            case MEM_DM_FG:
                // Payload: data mode, filter_id
                mode = ci_data_mode_2_x_mode(mode, request.get_subcommand_data()[0]);
                cfg.cur.mode()->set(mode);
                return resp.set_ok().get_packet();

            default:
                return civ::detail::set_unsupported(request, resp);
        }
    }
}

// ============================================================================
// 0x1C — Control PTT
// ============================================================================

std::string_view handle_ctl_ptt_x1c(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    if ((data_size >= 1) && (request.get_subcommand() == 0x00)) {
        if (data_size == 1) {
            return resp.set_command(request.get_command()).set_subcommand(0x00).append_byte(static_cast<uint8_t>(g_ports->radio->is_rx() ? 0 : 1)).get_packet();
        } else {
            switch (static_cast<uint8_t>(request.get_command_data()[1])) {
                case 0:
                    g_ports->radio->set_ptt(false);
                    break;
                case 1:
                    g_ports->radio->set_ptt(true);
                    break;
            }
            return resp.set_command(request.get_command()).set_subcommand(0x00).append_byte(CODE_OK).get_packet();
        }
    }
    return civ::detail::set_unsupported(request, resp);
}

// ============================================================================
// 0x25 — Send/Recv select/unselect VFO frequency
// ============================================================================

std::string_view handle_send_sel_freq_x25(const CivPacketView &request, CivTxPacker &resp) {
    size_t data_size = request.get_command_data().size();
    ComputedParameter<int32_t> *freq;

    if (request.get_vfo() == 0) {
        freq = cfg.cur.fg_freq();
    } else {
        freq = cfg.cur.bg_freq();
    }

    if (data_size == 1) {
        uint8_t bcd[5];
        to_bcd(bcd, freq->get(), 10);
        return resp.set_command(request.get_command())
            .set_subcommand(request.get_vfo())
            .append_data(bcd, 5).get_packet();
    } else if (data_size == 6) {
        freq->set(from_bcd(
            request.get_command_data().substr(1), 10));
        return resp.set_ok().get_packet();
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x26 — Send/Recv select/unselect VFO mode
// ============================================================================

std::string_view handle_send_sel_mode_x26(const CivPacketView &request, CivTxPacker &resp) {
    size_t              data_size = request.get_command_data().size();
    Parameter<int32_t> *mode_par;

    if (request.get_vfo() == 0) {
        mode_par =
            cfg.band.current_vfo()->get() == X6100_VFO_A ? cfg.band.vfoa_mode() : cfg.band.vfob_mode();
    } else {
        mode_par =
            cfg.band.current_vfo()->get() == X6100_VFO_B ? cfg.band.vfoa_mode() : cfg.band.vfob_mode();
    }
    x6100_mode_t mode = static_cast<x6100_mode_t>(mode_par->get());

    auto after_vfo = resp.set_command(request.get_command()).set_vfo(request.get_vfo());
    switch (data_size) {
        case 1:
            return after_vfo.append_byte(x_mode_2_ci_mode(mode))
                .append_byte(x_mode_is_data_mode(mode))
                .append_byte(0x01) // Default filter(1)
                .get_packet();
        case 4:
        case 3:
            mode = ci_mode_2_x_mode(mode, request.get_subcommand_data()[0]);
            mode = ci_data_mode_2_x_mode(mode, request.get_subcommand_data()[1]);
            mode_par->set(mode);
            return resp.set_ok().get_packet();
        case 2:
            mode = ci_mode_2_x_mode(mode, request.get_subcommand_data()[0]);
            mode_par->set(mode);
            return resp.set_ok().get_packet();
        default:
            return civ::detail::set_unsupported(request, resp);
    }
}

// ============================================================================
// 0x27 — Scope control & data
// ============================================================================

std::string_view handle_ctl_scp_x27(const CivPacketView &request, CivTxPacker &resp) {
    civ::detail::log_frame_raw(request.raw(), "req ");
    auto resp_data = scope_streamer_handle_27(request, resp);
    civ::detail::log_frame_raw(resp_data, "resp");
    return resp_data;


    size_t data_size = request.get_command_data().size();
    if (data_size >= 1) {
        switch (request.get_subcommand()) {
            case 0x10: // Send/read the Scope ON/OFF
                if (data_size == 1) {
                    return resp.set_command(request.get_command()).set_subcommand(request.get_subcommand()).append_byte(0x01).get_packet();
                } else {
                    return resp.set_command(request.get_command())
                        .set_subcommand(request.get_subcommand())
                        .get_packet();
                }
                break;
            case 0x11: // Send/read the Scope wave data output
                if (data_size == 1) {
                    return resp.set_command(request.get_command()).set_subcommand(request.get_subcommand()).append_byte(0x01).get_packet();
                } else {
                    return resp.set_command(request.get_command())
                        .set_subcommand(request.get_subcommand())
                        .get_packet();
                }
                break;
            case 0x13: // Send/read the Single/Dual scope setting
                return resp.set_ng().get_packet();
                break;
            case 0x14: // Send/read the Scope Center mode,
                if (data_size == 1) {
                    uint8_t d[] = {request.get_subcommand(), 0x00, 0x00};
                    return resp.set_command(request.get_command()).append_data(d, 3).get_packet();
                } else {
                    return resp.set_command(request.get_command())
                        .set_subcommand(request.get_subcommand())
                        .get_packet();
                }
                break;
            case 0x15:
                if (data_size >= 3) {
                    uint8_t bcd[7] = {request.get_subcommand(), 0, 0, 0, 0, 0, 0};
                    to_bcd(&bcd[2], 50000, 10);
                    return resp.set_command(request.get_command())
                        .set_subcommand(request.get_subcommand())
                        .append_data(&bcd[1], 6)
                        .get_packet();
                } else {
                    return resp.set_command(request.get_command())
                        .set_subcommand(request.get_subcommand())
                        .get_packet();
                }
                break;
            case 0x17:
                return resp.set_ng().get_packet();
                break;
            case 0x19:
                {
                    uint8_t d[] = {request.get_subcommand(), 0x00, 0x00, 0x00, 0x00};
                    return resp.set_command(request.get_command()).append_data(d, 5).get_packet();
                }
            case 0x1A:
                return resp.set_ng().get_packet();
            default:
                return civ::detail::set_unsupported(request, resp);
        }
    } else {
        return civ::detail::set_unsupported(request, resp);
    }
}

} // anonymous namespace


// ============================================================================
// Logging + set_unsupported
// ============================================================================
namespace civ::detail {

    void log_frame_raw(std::string_view raw, const char *prefix) {
    std::string s;
    char tmp[32];
    size_t len = raw.size();
    s.reserve(len * 3 + 16);

    // header: first 4 bytes (FE FE addr_radio addr_ctrl)
    snprintf(tmp, sizeof(tmp), "[%02X:%02X:%02X:%02X]-[",
                static_cast<uint8_t>(raw[0]), static_cast<uint8_t>(raw[1]),
                static_cast<uint8_t>(raw[2]), static_cast<uint8_t>(raw[3]));
    s += tmp;

    // data: bytes 4 .. len-2 (cmd + optional payload)
    for (size_t i = 4; i + 1 < len; i++) {
        snprintf(tmp, sizeof(tmp), "%02X:", static_cast<uint8_t>(raw[i]));
        s += tmp;
    }
    if (len > 4) s.pop_back();

    // footer: last byte (FRAME_END)
    snprintf(tmp, sizeof(tmp), "]-[%02X]", static_cast<uint8_t>(raw[len - 1]));
    s += tmp;

    LV_LOG_USER("%s\t: %s\t(Len %zu)", prefix, s.c_str(), len);
}

std::string_view set_unsupported(const CivPacketView &req, CivTxPacker &resp) {
    log_frame_raw(req.raw(), "unsupported");
    return resp.set_ng()
                .get_packet();
}

} // namespace civ::detail


// ============================================================================
// Public API
// ============================================================================

void civ_set_ports(const app_ports_t *ports) {
    g_ports = ports;
}

std::string_view process_civ_message(const CivPacketView &request, CivTxPacker &response_packer) {
    init_cmd_handlers();

    response_packer.set_dst_addr(request.get_src_address());
    cmd_handler_t handler = cmd_handlers[request.get_command()];
    if (handler) {
        return handler(request, response_packer);
    } else {
        return civ::detail::set_unsupported(request, response_packer);
    }
}

std::string_view pack_fg_freq_notify_00(int32_t freq, CivTxPacker &response_packer) {
    uint8_t bcd[5];
    to_bcd(bcd, freq, 10);
    return response_packer.set_command(C_SND_FREQ).append_data(bcd, 5).get_packet();
}

std::string_view pack_mode_notify_01(x6100_mode_t mode, CivTxPacker &response_packer) {
    return response_packer.set_command(C_SND_MODE).append_byte(x_mode_2_ci_mode(mode)).get_packet();
}

std::string_view pack_vfo_notify_07(x6100_vfo_t vfo, CivTxPacker &response_packer) {
    return response_packer.set_command(C_SET_VFO).append_byte(vfo).get_packet();
}
