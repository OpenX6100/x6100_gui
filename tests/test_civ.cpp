#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <sstream>
#include <iomanip>

#include "cat/civ_processor.h"
#include "cat/civ_protocol.h"
#include "cfg/db.h"
#include "cfg/settings_internal.h"
#include "cfg/settings_manager.h"

#include "cat_test_db.h"

// Fake application ports, injected via civ_set_ports(): CI-V handlers use them
// instead of the application's radio/telemetry objects.
namespace {

bool fake_tx_info_refresh(uint8_t *, float *alc, float *pwr, float *vswr) {
    *alc = 0.0f;
    *pwr = 2.0f;
    *vswr = 1.0f;
    return false;
}

float fake_s_meter_get_raw_db() { return -73.0f; }
bool    fake_is_rx() { return true; }
void    fake_set_ptt(bool) {}

const radio_port_t test_radio_port = {
    .is_rx = &fake_is_rx,
    .set_ptt = &fake_set_ptt,
    .set_freq = nullptr,
    .set_modem = nullptr,
    .set_pwr = nullptr,
};

const telemetry_port_t test_telemetry_port = {
    .tx_info_refresh = &fake_tx_info_refresh,
    .s_meter_get_raw_db = &fake_s_meter_get_raw_db,
};

const app_ports_t test_ports = {
    .radio = &test_radio_port,
    .telemetry = &test_telemetry_port,
    .audio = nullptr,
    .dsp_audio = nullptr,
};

} // namespace

// ---- protocol constants (CI-V / X6100) --------------------------------------

#define CODE_OK 0xFB
#define CODE_NG 0xFA

#define C_RD_FREQ 0x03
#define C_RD_MODE 0x04
#define C_SET_FREQ 0x05
#define C_SET_MODE 0x06
#define C_SET_VFO 0x07
#define C_CTL_SPLT 0x0f
#define C_SET_TS 0x10
#define C_CTL_ATT 0x11
#define C_CTL_LVL 0x14
#define C_RD_SQSM 0x15
#define C_CTL_FUNC 0x16
#define C_RD_TRXID 0x19
#define C_CTL_MEM 0x1a
#define C_CTL_PTT 0x1c
#define C_SEND_SEL_FREQ 0x25
#define C_SEND_SEL_MODE 0x26
#define C_CTL_SCP 0x27

#define S_VFOA 0x00
#define S_VFOB 0x01
#define S_XCHNG 0xb0

#define M_USB 0x01

#define MEM_DM_FG 0x06

// ---- helpers ---------------------------------------------------------------

namespace Catch {
    template <>
    struct StringMaker<std::vector<uint8_t>> {
        static std::string convert(const std::vector<uint8_t>& v) {
            std::ostringstream oss;
            oss << "Hex: [ ";
            oss << std::hex << std::uppercase << std::setfill('0');
            for (auto b : v) {
                oss << "0x" << std::setw(2) << static_cast<int>(b) << " ";
            }
            oss << "]";
            return oss.str();
        }
    };
}

static std::vector<uint8_t> to_bytes(std::string_view sv) {
    return std::vector<uint8_t>(sv.begin(), sv.end());
}

static constexpr int32_t kHz = 1000;

// Build a raw CI-V frame vector from a command byte and payload
static std::vector<uint8_t> ci_v_frame(uint8_t cmd, const std::vector<uint8_t> &payload = {}) {
    std::vector<uint8_t> raw(payload.size() + 6);
    raw[0] = FRAME_PRE;
    raw[1] = FRAME_PRE;
    raw[2] = LOCAL_ADDRESS;
    raw[3] = 0xE0;
    raw[4] = cmd;
    std::copy(payload.begin(), payload.end(), raw.begin() + 5);
    raw.back() = FRAME_END;
    return raw;
}

// ---- helpers ---------------------------------------------------------------

// Pre-populate band 5 with basic VFO settings.
static void set_band_5(TestDbGuard &db) {
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfo", X6100_VFO_A) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_freq", 7'100 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_freq", 7'150 * kHz) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfoa_mode", x6100_mode_usb) == SUCCESS);
    REQUIRE(store_save<int32_t>(StorageType::BAND, 5, "vfob_mode", x6100_mode_nfm) == SUCCESS);
}

static void init_band_5(TestDbGuard &db, SettingsManager &mgr) {
    set_band_5(db);
    REQUIRE(store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5) == SUCCESS);
    mgr.init_load();
    civ_set_ports(&test_ports);
}

// ---- unknown command -------------------------------------------------------

TEST_CASE("Frame::process returns CODE_NG for unknown command", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto raw = ci_v_frame(0xFF);
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_NG, 0xFD
    }));
}

// ---- C_RD_FREQ (0x03) ------------------------------------------------------

TEST_CASE("C_RD_FREQ reads current frequency as 5-byte BCD", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    REQUIRE(mgr.cp_fg_freq.get() == 7'100'000);

    auto raw = ci_v_frame(C_RD_FREQ);
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_RD_FREQ,
        0x00, 0x00, 0x10, 0x07, 0x00,
        0xFD
    }));
}

// ---- C_SET_FREQ (0x05) ------------------------------------------------------

TEST_CASE("C_SET_FREQ sets frequency from 5-byte BCD", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    // 14.2 MHz in little-endian BCD
    std::vector<uint8_t> payload = {0x00, 0x00, 0x20, 0x14, 0x00};

    auto raw = ci_v_frame(C_SET_FREQ, payload);
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
    }));
    REQUIRE(mgr.cp_fg_freq.get() == 14'200'000);
}

// ---- C_RD_MODE (0x04) ------------------------------------------------------

TEST_CASE("C_RD_MODE reads current mode as 2 bytes", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto raw = ci_v_frame(C_RD_MODE);
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_RD_MODE, M_USB, M_USB, 0xFD
    }));
}

// ---- C_SET_MODE (0x06) ------------------------------------------------------

TEST_CASE("C_SET_MODE sets mode", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto raw = ci_v_frame(C_SET_MODE, {0x03});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
    }));
    REQUIRE(mgr.cp_cur_mode.get() == x6100_mode_cw);
}

// ---- C_SET_VFO (0x07) ------------------------------------------------------

TEST_CASE("C_SET_VFO switches VFO", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    // Read current VFO (empty payload)
    {
        auto raw = ci_v_frame(C_SET_VFO, {});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_SET_VFO, S_VFOA, 0xFD
        }));
    }

    // Switch to B
    {
        auto raw = ci_v_frame(C_SET_VFO, {S_VFOB});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
        }));
        REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
    }

    // Switch back to A
    {
        auto raw = ci_v_frame(C_SET_VFO, {S_VFOA});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
        }));
        REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_A);
    }
}

TEST_CASE("C_SET_VFO S_XCHNG swaps VFO", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_A);

    auto raw = ci_v_frame(C_SET_VFO, {S_XCHNG});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
    }));
    REQUIRE(mgr.p_band_current_vfo.get() == X6100_VFO_B);
}

// ---- C_CTL_SPLT (0x0F) ------------------------------------------------------

TEST_CASE("C_CTL_SPLT read split status", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    mgr.p_band_split.set(0);
    {
        auto raw = ci_v_frame(C_CTL_SPLT, {});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_SPLT, 0x00, 0xFD
        }));
    }

    mgr.p_band_split.set(1);
    {
        auto raw = ci_v_frame(C_CTL_SPLT, {});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_SPLT, 0x01, 0xFD
        }));
    }
}

// ---- C_SET_TS (0x10) -------------------------------------------------------

TEST_CASE("C_SET_TS read/write tuning step", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    // Read default (500 Hz -> 0x02)
    {
        auto raw = ci_v_frame(C_SET_TS, {});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_SET_TS, 0x02, 0xFD
        }));
    }

    // Write 100 Hz (0x01)
    {
        auto raw = ci_v_frame(C_SET_TS, {0x01});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
        }));
        REQUIRE(mgr.p_mode_freq_step.get() == 100);
    }
}

// ---- C_CTL_ATT (0x11) ------------------------------------------------------

TEST_CASE("C_CTL_ATT read/write attenuator", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    // Read default
    {
        auto raw = ci_v_frame(C_CTL_ATT, {});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_ATT, 0x00, 0xFD
        }));
    }

    // Read when enabled: att=1 -> 0x20
    {
        mgr.cp_cur_att.set(1);
        auto raw = ci_v_frame(C_CTL_ATT, {});
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        auto sv = process_civ_message(req, packer);

        REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
            0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_ATT, 0x20, 0xFD
        }));
    }
}

// ---- C_CTL_LVL (0x14) ------------------------------------------------------

TEST_CASE("C_CTL_LVL volume read", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    mgr.p_volume.set(30);

    auto raw = ci_v_frame(C_CTL_LVL, {0x01});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    // 30 * 255 / 55 = 139 -> BE BCD 0x01 0x39
    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_LVL, 0x01, 0x01, 0x39, 0xFD
    }));
}

TEST_CASE("C_CTL_LVL RF gain read", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    mgr.p_rfgain.set(50);

    auto raw = ci_v_frame(C_CTL_LVL, {0x02});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    // 50 * 255 / 100 = 127 -> BE BCD 0x01 0x27
    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_LVL, 0x02, 0x01, 0x27, 0xFD
    }));
}

TEST_CASE("C_CTL_LVL squelch read", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    mgr.p_squelch.set(10);

    auto raw = ci_v_frame(C_CTL_LVL, {0x03});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    // 10 * 255 / 100 = 25 -> BE BCD 0x00 0x25
    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_LVL, 0x03, 0x00, 0x25, 0xFD
    }));
}

// CW pitch (0x09) used to fall through into TX power (0x0a): setting the
// pitch over CAT changed the power, and reading it returned the power.
TEST_CASE("C_CTL_LVL CW pitch write leaves TX power alone", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    mgr.p_pwr.set(5.0f);

    // 0128 = 600 Hz
    auto raw = ci_v_frame(C_CTL_LVL, {0x09, 0x01, 0x28});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    process_civ_message(req, packer);

    REQUIRE(mgr.p_key_tone.get() == 600);
    REQUIRE(mgr.p_pwr.get() == 5.0f);
}

TEST_CASE("C_CTL_LVL CW pitch read returns the pitch", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    mgr.p_key_tone.set(700);
    mgr.p_pwr.set(10.0f);

    auto raw = ci_v_frame(C_CTL_LVL, {0x09});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    // (700 - 300) * 0.425 = 170 -> BE BCD 0x01 0x70 (not the power's 0x02 0x55)
    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_LVL, 0x09, 0x01, 0x70, 0xFD
    }));
}

// ---- C_RD_SQSM (0x15) ------------------------------------------------------

TEST_CASE("C_RD_SQSM s-meter returns 3 bytes with stub meter == 0", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto raw = ci_v_frame(C_RD_SQSM, {0x02});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    // meter stub returns -73.0f -> val = -73.0 * 0.75 + 96 = 41 -> BE BCD 0x00 0x41
    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_RD_SQSM, 0x02, 0x00, 0x41, 0xFD
    }));
}

// ---- C_CTL_FUNC (0x16) -----------------------------------------------------

TEST_CASE("C_CTL_FUNC preamp read", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    mgr.cp_cur_pre.set(1);

    auto raw = ci_v_frame(C_CTL_FUNC, {0x02});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_FUNC, 0x02, 0x01, 0xFD
    }));
}

// ---- C_RD_TRXID (0x19) -----------------------------------------------------

TEST_CASE("C_RD_TRXID returns transceiver ID", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto raw = ci_v_frame(C_RD_TRXID, {0x00});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_RD_TRXID, 0x00, LOCAL_ADDRESS, 0xFD
    }));
}

// ---- C_CTL_MEM (0x1A) ------------------------------------------------------

TEST_CASE("C_CTL_MEM MEM_DM_FG read returns data mode info", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);
    mgr.cp_cur_mode.set(x6100_mode_lsb_dig);

    auto raw = ci_v_frame(C_CTL_MEM, {MEM_DM_FG});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_MEM,
        MEM_DM_FG,
        0x01, // Data mode
        0x01, // Default filter(1)
        0xFD
    }));
}

// ---- C_CTL_PTT (0x1C) ------------------------------------------------------

TEST_CASE("C_CTL_PTT read returns RX state from stub", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto raw = ci_v_frame(C_CTL_PTT, {0x00});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_PTT, 0x00, 0x00, 0xFD
    }));
}

// ---- C_SEND_SEL_FREQ (0x25) ------------------------------------------------

TEST_CASE("C_SEND_SEL_FREQ main/sub freq read", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    // Main VFO (sub 0) -> cp_fg_freq = vfoa_freq = 7.1 MHz
    REQUIRE(mgr.cp_fg_freq.get() == 7'100'000);

    auto raw = ci_v_frame(C_SEND_SEL_FREQ, {0x00});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_SEND_SEL_FREQ, 0x00,
        0x00, 0x00, 0x10, 0x07, 0x00,
        0xFD
    }));
}

// ---- C_SEND_SEL_MODE (0x26) ------------------------------------------------

TEST_CASE("C_SEND_SEL_MODE main/sub mode read", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    mgr.cp_cur_mode.set(x6100_mode_usb_dig);

    auto raw = ci_v_frame(C_SEND_SEL_MODE, {0x00});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_SEND_SEL_MODE,
        0x00,  // Foreground VFO
        M_USB, // Mode
        0x01,  // Mode data
        0x01,  // default filter(1)
        0xFD
    }));
}

// ---- C_CTL_SCP (0x27) ------------------------------------------------------

TEST_CASE("C_CTL_SCP sub 0x10 returns scope available", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto raw = ci_v_frame(C_CTL_SCP, {0x10});
    CivPacketView req(raw.data(), raw.size());
    uint8_t txBuf[256];
    CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
    auto sv = process_civ_message(req, packer);

    REQUIRE(to_bytes(sv) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_SCP, 0x10, 0x00, 0xFD
    }));
}

TEST_CASE("C_CTL_SCP sub 0x1A sweep speed write/read round-trip", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto send = [](const std::vector<uint8_t> &payload) {
        auto raw = ci_v_frame(C_CTL_SCP, payload);
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        return to_bytes(process_civ_message(req, packer));
    };

    // Write MID (0x01)
    REQUIRE(send({0x1A, 0x00, 0x01}) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
    }));
    // Read back MID
    REQUIRE(send({0x1A, 0x00}) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_SCP, 0x1A, 0x00, 0x01, 0xFD
    }));

    // Write SLOW (0x02)
    REQUIRE(send({0x1A, 0x00, 0x02}) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
    }));
    // Read back SLOW
    REQUIRE(send({0x1A, 0x00}) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_SCP, 0x1A, 0x00, 0x02, 0xFD
    }));
}

TEST_CASE("C_CTL_SCP sub 0x1A invalid sweep speed is ignored", "[cat]") {
    TestDbGuard db;
    SettingsManager mgr;
    init_band_5(db, mgr);
    cfg_set_instance(&mgr);

    auto send = [](const std::vector<uint8_t> &payload) {
        auto raw = ci_v_frame(C_CTL_SCP, payload);
        CivPacketView req(raw.data(), raw.size());
        uint8_t txBuf[256];
        CivTxPacker packer(txBuf, 0xE0, LOCAL_ADDRESS);
        return to_bytes(process_civ_message(req, packer));
    };

    // Establish a known state
    REQUIRE(send({0x1A, 0x00, 0x02}) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
    }));
    // Out-of-range value: acknowledged but ignored
    REQUIRE(send({0x1A, 0x00, 0x05}) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, CODE_OK, 0xFD
    }));
    REQUIRE(send({0x1A, 0x00}) == std::vector<uint8_t>({
        0xFE, 0xFE, 0xE0, LOCAL_ADDRESS, C_CTL_SCP, 0x1A, 0x00, 0x02, 0xFD
    }));
}

// ---- cleanup ---------------------------------------------------------------

TEST_CASE("cfg_set_instance(nullptr) restores production global", "[cat]") {
    cfg_set_instance(nullptr);
    SUCCEED();
}
