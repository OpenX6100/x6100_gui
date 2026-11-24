// The network CAT (src/cat/lan/cat_lan.cpp, wfview's protocol) and who may
// use it: the real server thread on its UDP ports 50001-50003, a fake radio
// that counts PTT and audio, and clients on two loopback addresses. The
// logged-in station is 127.0.0.1; "another computer" is 127.0.0.2.

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "cat/civ_processor.h"
#include "cat/lan/cat_lan.h"
#include "cat/lan/cat_lan_packets.h"

#include "cat_test_db.h"

// cat_lan.cpp names its thread for the log; the GUI's diag_log.c isn't built
// here.
extern "C" const char *diag_set_source(const char *name) { return name; }

namespace {

// ---- fake radio ---------------------------------------------------------------

std::atomic<int> ptt_on{0};
std::atomic<int> audio_sent{0};

bool  fake_is_rx() { return true; }
void  fake_set_ptt(bool on) { if (on) ptt_on++; }
bool  fake_tx_info(uint8_t *, float *alc, float *pwr, float *vswr) { *alc = 0; *pwr = 0; *vswr = 1; return false; }
float fake_s_meter() { return -73.0f; }

audio_port_player_t *fake_create_player(uint32_t, uint32_t) { return reinterpret_cast<audio_port_player_t *>(&audio_sent); }
int  fake_player_send(audio_port_player_t *, int16_t *, size_t) { audio_sent++; return 0; }
void fake_player_release(audio_port_player_t *) {}

uint32_t fake_dsp_subscribe(dsp_audio_float_cb_t, uint32_t) { return 1; }
void     fake_dsp_set_active(uint32_t, bool) {}
void     fake_dsp_unsubscribe(uint32_t) {}

uint32_t fake_psd_subscribe(psd_frame_cb_t, uint16_t, uint16_t, void *) { return 1; }
void     fake_psd_set_active(uint32_t, bool) {}
void     fake_psd_chunks(uint32_t, uint16_t) {}
void     fake_psd_unsubscribe(uint32_t) {}

const radio_port_t     radio_port = {&fake_is_rx, &fake_set_ptt, nullptr, nullptr, nullptr};
const telemetry_port_t telemetry_port = {&fake_tx_info, &fake_s_meter};
const audio_port_t     audio_port = {nullptr, &fake_create_player, &fake_player_send, nullptr,
                                     &fake_player_release, nullptr, nullptr, nullptr};
const dsp_audio_port_t dsp_audio_port = {&fake_dsp_subscribe, &fake_dsp_set_active, &fake_dsp_unsubscribe};
const psd_port_t       psd_port = {&fake_psd_subscribe, &fake_psd_set_active, &fake_psd_chunks, &fake_psd_unsubscribe};
const app_ports_t      ports = {&radio_port, &telemetry_port, &audio_port, &dsp_audio_port, &psd_port};

// ---- a UDP client ----------------------------------------------------------------

constexpr uint16_t CONTROL = 50001, CIV = 50002, AUDIO = 50003;

void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }

struct Client {
    int      fd;
    uint32_t id;

    Client(const char *ip, uint32_t id_) : id(id_) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, ip, &a.sin_addr);
        REQUIRE(bind(fd, (sockaddr *)&a, sizeof(a)) == 0);
        timeval tv{0, 300000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~Client() { close(fd); }

    void send(uint16_t port, const void *data, size_t n) {
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_port        = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(fd, data, n, 0, (sockaddr *)&a, sizeof(a));
    }

    // Every packet that arrives before a 300 ms gap
    std::vector<std::vector<uint8_t>> replies() {
        std::vector<std::vector<uint8_t>> out;
        uint8_t                           b[2048];
        for (;;) {
            ssize_t n = recv(fd, b, sizeof(b), 0);
            if (n <= 0) break;
            out.emplace_back(b, b + n);
        }
        return out;
    }

    static bool has(const std::vector<std::vector<uint8_t>> &r, size_t size, std::vector<uint8_t> *pkt = nullptr) {
        for (auto &p : r) {
            if (p.size() == size) {
                if (pkt) *pkt = p;
                return true;
            }
        }
        return false;
    }

    void control(uint16_t port, uint16_t type) {
        control_packet_t p{};
        p.len    = sizeof(p);
        p.type   = type;
        p.sentid = id;
        send(port, &p, sizeof(p));
    }

    // fill: what follows the text in each 16-byte field (wfview sends zeros)
    void login(const char *user, const char *pass, bool encode_pass = true, uint8_t fill = 0) {
        login_packet_t p{};
        p.len        = sizeof(p);
        p.sentid     = id;
        p.tokrequest = 0x1234;
        std::memset(p.username, fill, sizeof(p.username));
        std::memset(p.password, fill, sizeof(p.password));
        int     n;
        uint8_t enc[16];
        passcode_encode(user, (int)std::strlen(user), enc, &n);
        std::memcpy(p.username, enc, n);
        if (n < 16) p.username[n] = 0;
        if (encode_pass) {
            passcode_encode(pass, (int)std::strlen(pass), enc, &n);
            std::memcpy(p.password, enc, n);
        } else {
            n = (int)std::strlen(pass);
            std::memcpy(p.password, pass, n);
        }
        if (n < 16) p.password[n] = 0;
        send(CONTROL, &p, sizeof(p));
    }

    // Login's answer: true = accepted, false = refused, nothing = no answer
    int login_answer() {
        std::vector<uint8_t> p;
        if (!has(replies(), LOGIN_RESPONSE_SIZE, &p)) return -1;
        return ((const login_response_packet_t *)p.data())->error == 0 ? 1 : 0;
    }

    void hello() {
        control(CONTROL, 0x03);
        replies();
        control(CONTROL, 0x06);
        replies();
    }

    void token() {
        token_packet_t p{};
        p.len         = sizeof(p);
        p.sentid      = id;
        p.requesttype = 0x02;
        send(CONTROL, &p, sizeof(p));
    }

    void stream() {
        uint8_t           b[CONNINFO_SIZE] = {0};
        control_packet_t *h = (control_packet_t *)b;
        h->len    = sizeof(b);
        h->sentid = id;
        send(CONTROL, b, sizeof(b));
    }

    void ptt_on_frame() {
        const uint8_t  frame[] = {0xFE, 0xFE, 0xA4, 0xE0, 0x1C, 0x00, 0x01, 0xFD};
        uint8_t        b[PING_SIZE + sizeof(frame)] = {0};
        ping_packet_t *h = (ping_packet_t *)b;
        h->len     = sizeof(b);
        h->sentid  = id;
        h->reply   = 0xC1;
        h->datalen = sizeof(frame);
        std::memcpy(b + PING_SIZE, frame, sizeof(frame));
        send(CIV, b, sizeof(b));
    }

    void audio_data() {
        uint8_t         b[AUDIO_SIZE + 64] = {0};
        audio_packet_t *h = (audio_packet_t *)b;
        h->len     = sizeof(b);
        h->sentid  = id;
        h->ident   = 0x0080;
        h->datalen = htons(64);
        send(AUDIO, b, sizeof(b));
    }

    void retransmit_request(uint16_t port) {
        control_packet_t p{};
        p.len    = sizeof(p);
        p.type   = 0x01;
        p.seq    = 1;
        p.sentid = id;
        send(port, &p, sizeof(p));
    }
};

} // namespace

TEST_CASE("Network CAT: nothing without the login, and only the logged-in station", "[cat][lan]") {
    TestDbGuard     db;
    SettingsManager mgr;
    REQUIRE(store_save<int32_t>(StorageType::GLOBAL, 0, "band_id", 5) == SUCCESS);
    mgr.init_load();
    cfg_set_instance(&mgr);
    civ_set_ports(&ports);
    REQUIRE(cat_lan_init(&ports) == 0);

    // Another computer, nobody logged in
    {
        Client other("127.0.0.2", 0xA1);
        other.ptt_on_frame();
        settle();
        CHECK(ptt_on == 0);

        other.control(CIV, 0x03);
        CHECK(other.replies().empty());
        other.ptt_on_frame();
        settle();
        CHECK(ptt_on == 0);
    }

    // Wrong password: refused, and no token, stream or CI-V after it
    {
        Client c("127.0.0.2", 0xB1);
        c.hello();
        c.login("root", "wrong");
        CHECK(c.login_answer() == 0);
        c.token();
        CHECK_FALSE(Client::has(c.replies(), TOKEN_SIZE));
        c.stream();
        CHECK_FALSE(Client::has(c.replies(), STATUS_SIZE));
        Client civ("127.0.0.2", 0xB2);
        civ.control(CIV, 0x03);
        CHECK(civ.replies().empty());
        civ.ptt_on_frame();
        settle();
        CHECK(ptt_on == 0);
    }

    // A username with a letter more
    {
        Client c("127.0.0.2", 0xC1);
        c.hello();
        c.login("rootx", "root");
        CHECK(c.login_answer() == 0);
    }

    // The password sent plain is taken, as wfview's server does; close
    // frees the radio for the next station
    {
        Client c("127.0.0.2", 0xD1);
        c.hello();
        c.login("root", "root", false);
        CHECK(c.login_answer() == 1);
        c.control(CONTROL, CTL_TYPE_CLOSE);
        settle();
    }

    // The station logs in as wfview does (bytes after the text needn't be
    // zero), opens CI-V and audio, keys the radio and sends audio
    Client ctrl("127.0.0.1", 0xE1), civ("127.0.0.1", 0xE2), audio("127.0.0.1", 0xE3);
    ctrl.hello();
    ctrl.login("root", "root", true, 0xAA);
    REQUIRE(ctrl.login_answer() == 1);
    ctrl.token();
    CHECK(Client::has(ctrl.replies(), TOKEN_SIZE));
    ctrl.stream();
    CHECK(Client::has(ctrl.replies(), STATUS_SIZE));
    civ.control(CIV, 0x03);
    CHECK_FALSE(civ.replies().empty());
    audio.control(AUDIO, 0x03);
    CHECK_FALSE(audio.replies().empty());
    civ.ptt_on_frame();
    audio.audio_data();
    settle();
    CHECK(ptt_on == 1);
    CHECK(audio_sent == 1);

    // While it is logged in, another computer gets nothing
    {
        Client other("127.0.0.2", 0xF1);
        other.control(CONTROL, 0x03);
        CHECK(other.replies().empty()); // no taking over the session
        other.login("root", "root");
        CHECK(other.login_answer() == -1);
        other.ptt_on_frame();
        other.audio_data();
        other.retransmit_request(CONTROL);
        other.retransmit_request(CIV);
        other.retransmit_request(AUDIO);
        CHECK(other.replies().empty()); // no copies of what the station got
        other.control(AUDIO, CTL_TYPE_CLOSE);
        other.control(CONTROL, CTL_TYPE_CLOSE);
        settle();
        CHECK(ptt_on == 1);
        CHECK(audio_sent == 1);
    }

    // Nor does another program on the station's computer, on its own ports
    {
        Client same_pc("127.0.0.1", 0xF2);
        same_pc.ptt_on_frame();
        same_pc.audio_data();
        settle();
        CHECK(ptt_on == 1);
        CHECK(audio_sent == 1);
    }

    // The station still works: the closes above didn't end its session
    civ.ptt_on_frame();
    audio.audio_data();
    settle();
    CHECK(ptt_on == 2);
    CHECK(audio_sent == 2);

    // After the station closes, the radio is free again
    ctrl.control(CONTROL, CTL_TYPE_CLOSE);
    settle();
    {
        Client next("127.0.0.2", 0xF3);
        next.control(CONTROL, 0x03);
        CHECK_FALSE(next.replies().empty());
    }

    cat_lan_destruct();
}
