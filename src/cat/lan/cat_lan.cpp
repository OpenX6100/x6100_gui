#include "cat_lan.h"
#include "../../diag_log.h"
#include "cat_lan_packets.h"
#include "cat/cat.h"
#include "cat/civ_protocol.h"
#include "cat/civ_processor.h"
#include "cat/scope_streamer.h"
#include "common/queue.h"
#include "dsp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cfg/cfg_api.h"
#include "lvgl/lvgl.h"

#include "common/vector.h"

#define CONTROL_PORT     50001
#define CIV_PORT         50002
#define TX_BUF_SIZE      500
#define PURGE_MS         10000
#define POLL_TIMEOUT_MS  100
#define AUTH_USERNAME    "root"
#define AUTH_PASSWORD    "root"

// Audio port
#define AUDIO_PORT           50003

enum AuthState {
    ST_LISTENING,
    ST_AYT_SENT,
    ST_LOGIN_PENDING,
    ST_AUTH_PENDING,
    ST_TOKEN_SENT,
    ST_CIV_ACTIVE
};

struct TxEntry {
    uint16_t                                    seq;
    std::vector<uint8_t>                        data;
    std::chrono::steady_clock::time_point       sent_at;
    int                                         retries;
};

static int                fd_control        = -1;
static int                fd_civ            = -1;
static int                fd_event          = -1;
static struct sockaddr_in client_ctrl;
static bool               client_ctrl_valid = false;
static struct sockaddr_in client_civ;
static bool               client_civ_valid  = false;

static AuthState          auth_state    = ST_LISTENING;
static std::thread*       thread        = nullptr;
static std::atomic<bool>  keep_running(false);

static uint32_t           my_id        = 0;
static uint32_t           remote_id    = 0;
static uint16_t           send_seq     = 1;
static uint16_t           civ_send_seq = 0;
static uint16_t           auth_seq     = 0x30;
static uint16_t           tok_request  = 0;
static uint32_t           token        = 0;

static std::map<uint16_t, TxEntry> tx_buffer;
static std::mutex                  tx_mutex;
static std::map<uint16_t, int>     rx_missing;
static std::mutex                  missing_mutex;
static TSQueue<std::vector<uint8_t>> send_queue;

static Subscription                sub_freq;
static Subscription                sub_mode;
static Subscription                sub_vfo;

static std::chrono::steady_clock::time_point last_ping_time{
    std::chrono::steady_clock::now()};

static int                fd_audio            = -1;
static struct sockaddr_in client_audio;
static bool               client_audio_valid  = false;
static uint16_t           audio_send_seq      = 0;
static int                audio_rx_rate       = 16000;

static audio_port_player_t *wfview_player       = nullptr;

static uint32_t          dsp_audio_sub_id     = DSP_AUDIO_SUB_INVALID;

// Application ports injected by cat_lan_init().
static const app_ports_t *g_ports = nullptr;

// DSP PSD subscription backing the CI-V scope streamer.
static uint32_t scope_psd_sub_id = PSD_SUB_INVALID;

static void scope_psd_cb(const float *psd_db, size_t size, uint32_t base_freq, uint32_t width_hz,
                         float min, float max, void *user_data) {
    (void)user_data;
    scope_streamer_push_data(psd_db, size, base_freq, width_hz, min, max);
}

// The scope subscription only accumulates/delivers while scope data is enabled
// and a transport (serial or LAN) has registered a notify callback.
static void scope_active_cb(bool active) {
    if (g_ports && scope_psd_sub_id != PSD_SUB_INVALID) {
        g_ports->psd->set_active(scope_psd_sub_id, active);
    }
}

static void scope_cadence_cb(uint16_t chunks_per_frame) {
    if (g_ports && scope_psd_sub_id != PSD_SUB_INVALID) {
        g_ports->psd->set_chunks_per_frame(scope_psd_sub_id, chunks_per_frame);
    }
}

static bool udp_send(int fd, const void *data, size_t len, const sockaddr_in *dst);
static bool send_control(int fd, uint16_t type, uint16_t seq, bool tracked, const sockaddr_in *dst);
static bool civ_data_send(const uint8_t *civ_data, size_t civ_len);
static bool send_tracked(int fd, const uint8_t *data, size_t len, const sockaddr_in *dst);
static void process_control_packet(const uint8_t *buf, size_t len, const sockaddr_in *src);
static void process_civ_packet(const uint8_t *buf, size_t len, const sockaddr_in *src, CivTxPacker &resp_packer);
static void send_capabilities(void);
static void send_conninfo(void);
static void handle_retransmit(int fd, const uint8_t *buf, size_t len, const sockaddr_in *dst);
static void check_retransmit(void);
static void purge_buffer(void);
static void on_fg_freq_change_cb(Subject *s, void *user_data);
static void on_mode_change_cb(Subject *s, void *user_data);
static void on_vfo_change_cb(Subject *s, void *user_data);
static void handle_login_packet(const uint8_t *buf, size_t len);
static void handle_token_packet(const uint8_t *buf, size_t len);
static void handle_conninfo_packet(const uint8_t *buf, size_t len);
static void process_audio_packet(const uint8_t *buf, size_t len, const sockaddr_in *src);
static void audio_lan_tx_cb(size_t n, float *samples);
static void cleanup_audio();

static uint32_t make_my_id(uint16_t port) {
    return (uint32_t)port;
}

static bool udp_send(int fd, const void *data, size_t len, const sockaddr_in *dst) {
    ssize_t ret = sendto(fd, data, len, 0, (const sockaddr *)dst, sizeof(*dst));
    return ret == (ssize_t)len;
}

static bool send_control(int fd, uint16_t type, uint16_t seq, bool tracked, const sockaddr_in *dst) {
    control_packet_t pkt;
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.len    = sizeof(pkt);
    pkt.type   = type;
    pkt.seq    = seq;
    pkt.sentid = my_id;
    pkt.rcvdid = remote_id;
    if (tracked) {
        return send_tracked(fd, (const uint8_t *)&pkt, sizeof(pkt), dst);
    }
    return udp_send(fd, &pkt, sizeof(pkt), dst);
}

static bool send_tracked(int fd, const uint8_t *data, size_t len, const sockaddr_in *dst) {
    if (send_seq == 0) send_seq = 1;
    uint16_t seq = send_seq++;
    uint8_t *mutable_data = const_cast<uint8_t *>(data);
    mutable_data[6] = seq & 0xFF;
    mutable_data[7] = (seq >> 8) & 0xFF;
    TxEntry entry;
    entry.seq = seq;
    entry.data.assign(data, data + len);
    entry.sent_at = std::chrono::steady_clock::now();
    entry.retries = 0;
    {
        std::lock_guard<std::mutex> lock(tx_mutex);
        if (tx_buffer.size() >= TX_BUF_SIZE) {
            tx_buffer.erase(tx_buffer.begin());
        }
        tx_buffer[seq] = std::move(entry);
    }
    return udp_send(fd, data, len, dst);
}

static bool civ_data_send(const uint8_t *civ_data, size_t civ_len) {
    if (!client_civ_valid) return false;
    std::vector<uint8_t> buf(sizeof(ping_packet_t) + civ_len);
    ping_packet_t *pkt = (ping_packet_t *)buf.data();
    pkt->len     = (uint32_t)(sizeof(ping_packet_t) + civ_len);
    pkt->type    = 0;
    pkt->sentid  = my_id;
    pkt->rcvdid  = remote_id;
    pkt->reply   = 0xC1;
    pkt->datalen = (uint16_t)civ_len;
    pkt->sendseq = htons(civ_send_seq++);
    if (civ_len > 0) {
        std::memcpy(buf.data() + sizeof(ping_packet_t), civ_data, civ_len);
    }
    return send_tracked(fd_civ, buf.data(), buf.size(), &client_civ);
}

// ---- Control packet processing -------------------------------------------

static void handle_retransmit(int fd, const uint8_t *buf, size_t len, const sockaddr_in *dst) {
    const control_packet_t *in = (const control_packet_t *)buf;
    if (len == CONTROL_SIZE) {
        std::lock_guard<std::mutex> lock(tx_mutex);
        auto it = tx_buffer.find(in->seq);
        if (it != tx_buffer.end()) {
            it->second.retries++;
            udp_send(fd, it->second.data.data(), it->second.data.size(), dst);
        }
    } else {
        const uint8_t *p = buf + CONTROL_SIZE;
        const uint8_t *end = buf + len;
        std::lock_guard<std::mutex> lock(tx_mutex);
        while (p + 4 <= end) {
            uint16_t rseq = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
            auto it = tx_buffer.find(rseq);
            if (it != tx_buffer.end()) {
                it->second.retries++;
                udp_send(fd, it->second.data.data(), it->second.data.size(), dst);
            }
            p += 4;
        }
    }
}

static void process_control_packet(const uint8_t *buf, size_t len, const sockaddr_in *src) {
    if (len < sizeof(control_packet_t)) return;
    const control_packet_t *in = (const control_packet_t *)buf;

    if (in->type == 0x01) {
        handle_retransmit(fd_control, buf, len, src);
        return;
    }

    // Ping request
    if (len == PING_SIZE) {
        const ping_packet_t *pin = (const ping_packet_t *)buf;
        if (pin->type == 0x07 && pin->reply == 0x00) {
            last_ping_time = std::chrono::steady_clock::now();
            ping_packet_t resp;
            std::memset(&resp, 0, sizeof(resp));
            resp.len    = sizeof(resp);
            resp.type   = 0x07;
            resp.seq    = pin->seq;
            resp.sentid = my_id;
            resp.rcvdid = remote_id;
            resp.reply  = 0x01;
            resp.time   = pin->time;
            udp_send(fd_control, &resp, sizeof(resp), &client_ctrl);
        }
        return;
    }

    // Close on control port
    if (in->type == CTL_TYPE_CLOSE) {
        LV_LOG_INFO("LAN: close received on control port");
        cleanup_audio();
        client_ctrl_valid = false;
        client_civ_valid  = false;
        scope_streamer_set_notify(nullptr);
        auth_state = ST_LISTENING;
        send_queue.clear();
        { std::lock_guard<std::mutex> lock(tx_mutex); tx_buffer.clear(); }
        { std::lock_guard<std::mutex> lock(missing_mutex); rx_missing.clear(); }
        return;
    }

    switch (auth_state) {
    case ST_LISTENING:
        if (in->type == 0x03) {
            LV_LOG_INFO("LAN: are-you-there received");
            client_ctrl        = *src;
            client_ctrl_valid  = true;
            remote_id          = in->sentid;
            last_ping_time     = std::chrono::steady_clock::now();
            send_control(fd_control, 0x04, 0, false, &client_ctrl);
            send_control(fd_control, 0x06, 0x01, false, &client_ctrl);
            auth_state = ST_AYT_SENT;
        }
        break;

    case ST_AYT_SENT:
        if (in->type == 0x06) {
            LV_LOG_INFO("LAN: I-am-ready received");
            remote_id = in->sentid;
            auth_state = ST_LOGIN_PENDING;
        } else {
            send_control(fd_control, 0x04, 0, false, &client_ctrl);
            send_control(fd_control, 0x06, 0x01, false, &client_ctrl);
        }
        break;

    default:
        if (in->type == 0x03) {
            LV_LOG_INFO("LAN: re-AYT received, restarting auth");
            cleanup_audio();
            client_ctrl       = *src;
            client_ctrl_valid = true;
            remote_id         = in->sentid;
            last_ping_time    = std::chrono::steady_clock::now();
            client_civ_valid  = false;
            scope_streamer_set_notify(nullptr);
            token             = 0;
            send_seq          = 1;
            civ_send_seq      = 0;
            auth_seq          = 0x30;
            tok_request       = 0;
            {
                std::lock_guard<std::mutex> lock(tx_mutex);
                tx_buffer.clear();
            }
            {
                std::lock_guard<std::mutex> lock(missing_mutex);
                rx_missing.clear();
            }
            send_queue.clear();
            send_control(fd_control, 0x04, 0, false, &client_ctrl);
            send_control(fd_control, 0x06, 0x01, false, &client_ctrl);
            auth_state = ST_AYT_SENT;
        }
        break;
    }
}

// ---- Login / Token / ConnInfo handlers -----------------------------------

static void handle_login_packet(const uint8_t *buf, size_t len) {
    if (len < sizeof(login_packet_t)) return;
    // New login means old connection is dead — clean up scope callback and audio
    cleanup_audio();
    scope_streamer_set_notify(nullptr);
    const login_packet_t *in = (const login_packet_t *)buf;

    uint8_t expected_user[16], expected_pass[16];
    int ulen, plen;
    passcode_encode(AUTH_USERNAME, (int)std::strlen(AUTH_USERNAME), expected_user, &ulen);
    passcode_encode(AUTH_PASSWORD, (int)std::strlen(AUTH_PASSWORD), expected_pass, &plen);

    bool user_ok = (ulen <= 16 && std::memcmp(in->username, expected_user, (size_t)ulen) == 0);
    bool pass_ok = (plen <= 16 && std::memcmp(in->password, expected_pass, (size_t)plen) == 0);

    if (!user_ok || !pass_ok) {
        LV_LOG_WARN("LAN: invalid credentials");
    }

    remote_id   = in->sentid;
    tok_request = in->tokrequest;
    token       = 0;
    auth_seq    = in->innerseq + 1;

    login_response_packet_t resp;
    std::memset(&resp, 0, sizeof(resp));
    resp.len          = sizeof(resp);
    resp.sentid       = my_id;
    resp.rcvdid       = remote_id;
    resp.payloadsize  = htonl((uint32_t)(sizeof(resp) - 0x10));
    resp.requesttype  = 0x00;
    resp.requestreply = 0x02;
    resp.innerseq     = htons(auth_seq++);
    resp.tokrequest   = tok_request;
    resp.error        = 0;
    std::strncpy(resp.connection, "WFVIEW", sizeof(resp.connection) - 1);

    send_tracked(fd_control, (const uint8_t *)&resp, sizeof(resp), &client_ctrl);
    auth_state = ST_AUTH_PENDING;
    LV_LOG_INFO("LAN: login response sent");
}

static void handle_token_packet(const uint8_t *buf, size_t len) {
    if (len < sizeof(token_packet_t)) return;
    const token_packet_t *in = (const token_packet_t *)buf;

    bool is_new_token = (in->requesttype == 0x02);
    if (is_new_token) {
        LV_LOG_INFO("LAN: new token request");
    } else {
        LV_LOG_INFO("LAN: token renewal");
    }

    remote_id = in->sentid;
    token     = in->token;
    token++;
    auth_seq = in->innerseq + 1;

    token_packet_t resp;
    std::memset(&resp, 0, sizeof(resp));
    resp.len          = sizeof(resp);
    resp.sentid       = my_id;
    resp.rcvdid       = remote_id;
    resp.payloadsize  = htonl((uint32_t)(sizeof(resp) - 0x10));
    resp.requestreply = 0x02;
    resp.requesttype  = 0x05;
    resp.innerseq     = htons(auth_seq++);
    resp.tokrequest   = tok_request;
    resp.token        = token;
    resp.resetcap     = htons((uint16_t)0x0798);
    resp.response     = 0;

    send_tracked(fd_control, (const uint8_t *)&resp, sizeof(resp), &client_ctrl);
    auth_state = ST_CIV_ACTIVE;
    if (is_new_token) {
        send_capabilities();
        send_conninfo();
        LV_LOG_INFO("LAN: token confirmed, caps+conninfo sent");
    } else {
        LV_LOG_INFO("LAN: token renewal confirmed");
    }
}

static void send_capabilities(void) {
    uint8_t buf[CAPABILITIES_SIZE + RADIO_CAP_SIZE];
    std::memset(buf, 0, sizeof(buf));

    capabilities_packet_t *caps = (capabilities_packet_t *)buf;
    caps->len          = sizeof(buf);
    caps->sentid       = my_id;
    caps->rcvdid       = remote_id;
    caps->payloadsize  = htonl((uint32_t)(sizeof(buf) - 0x10));
    caps->requesttype  = 0x00;
    caps->requestreply = 0x02;
    caps->innerseq     = htons(auth_seq++);
    caps->tokrequest   = tok_request;
    caps->token        = token;
    caps->numradios    = htons(1);

    radio_cap_packet_t *rad = (radio_cap_packet_t *)(buf + CAPABILITIES_SIZE);
    rad->commoncap = 0x8010;
    uint8_t mac[6] = {0x00, 0x1E, 0xC0, 0xFF, 0xEE, 0x01};
    std::memcpy(rad->macaddress, mac, 6);
    std::strncpy(rad->name, "X6100", sizeof(rad->name) - 1);
    rad->civ = LOCAL_ADDRESS;
    rad->baudrate = htonl(19200U);

    send_tracked(fd_control, buf, sizeof(buf), &client_ctrl);
}

static void send_conninfo(void) {
    conninfo_packet_t pkt;
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.len          = sizeof(pkt);
    pkt.sentid       = my_id;
    pkt.rcvdid       = remote_id;
    pkt.payloadsize  = htonl((uint32_t)(sizeof(pkt) - 0x10));
    pkt.requesttype  = 0x03;
    pkt.requestreply = 0x02;
    pkt.innerseq     = htons(auth_seq++);
    pkt.tokrequest   = tok_request;
    pkt.token        = token;
    pkt.commoncap    = 0x8010;
    uint8_t mac[6] = {0x00, 0x1E, 0xC0, 0xFF, 0xEE, 0x01};
    std::memcpy(pkt.macaddress, mac, 6);
    std::strncpy(pkt.name, "X6100", sizeof(pkt.name) - 1);
    pkt.busy    = 0;
    pkt.ipaddress = 0;

    send_tracked(fd_control, (const uint8_t *)&pkt, sizeof(pkt), &client_ctrl);
}

static void handle_conninfo_packet(const uint8_t *buf, size_t len) {
    LV_LOG_INFO("LAN: stream request received");

    // Parse client-requested audio sample rate from conninfo
    if (len >= sizeof(conninfo_packet_t)) {
        const conninfo_packet_t *in = (const conninfo_packet_t *)buf;
        if (in->rxsample != 0) {
            audio_rx_rate = (int)ntohl(in->rxsample);
            LV_LOG_INFO("LAN: client requests audio rx rate %d Hz", audio_rx_rate);
        }
    }

    // Clear any stale scope callback before registering new one
    scope_streamer_set_notify(nullptr);

    status_packet_t resp;
    std::memset(&resp, 0, sizeof(resp));
    resp.len          = sizeof(resp);
    resp.sentid       = my_id;
    resp.rcvdid       = remote_id;
    resp.payloadsize  = htonl((uint32_t)(sizeof(resp) - 0x10));
    resp.requesttype  = 0x03;
    resp.requestreply = 0x02;
    resp.innerseq     = htons(auth_seq++);
    resp.tokrequest   = tok_request;
    resp.token        = token;
    resp.error        = 0;
    resp.disc         = 0;
    resp.civport      = htons(CIV_PORT);
    resp.audioport    = htons(AUDIO_PORT);
    resp.commoncap    = 0x8010;

    send_tracked(fd_control, (const uint8_t *)&resp, sizeof(resp), &client_ctrl);
    auth_state = ST_CIV_ACTIVE;
    LV_LOG_INFO("LAN: CIV stream active on port %d, audio on port %d",
                CIV_PORT, AUDIO_PORT);

    // LAN waterfall streaming overrides serial
    scope_streamer_set_notify([](std::string_view pkt) {
        if (auth_state >= ST_CIV_ACTIVE && client_civ_valid) {
            civ_data_send(reinterpret_cast<const uint8_t *>(pkt.data()), pkt.size());
        }
    });
}

// ---- Audio port processing ------------------------------------------------

static audio_packet_t make_audio_header(uint16_t seq, uint16_t sendseq, uint16_t datalen) {
    audio_packet_t pkt;
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.len     = sizeof(pkt) + datalen;
    pkt.sentid  = my_id;
    pkt.rcvdid  = remote_id;
    pkt.ident   = 0x0080;
    pkt.datalen = htons(datalen);
    pkt.sendseq = htons(sendseq);
    pkt.seq     = seq;
    return pkt;
}

static void process_audio_packet(const uint8_t *buf, size_t len, const sockaddr_in *src) {
    if (len < sizeof(control_packet_t)) return;
    const control_packet_t *hdr = (const control_packet_t *)buf;

    if (hdr->type == 0x01) {
        handle_retransmit(fd_audio, buf, len, src);
        return;
    }

    // Ping on audio port
    if (len == PING_SIZE) {
        const ping_packet_t *pin = (const ping_packet_t *)buf;
        if (pin->type == 0x07 && pin->reply == 0x00) {
            ping_packet_t resp;
            std::memset(&resp, 0, sizeof(resp));
            resp.len    = sizeof(resp);
            resp.type   = 0x07;
            resp.seq    = pin->seq;
            resp.sentid = my_id;
            resp.rcvdid = remote_id;
            resp.reply  = 0x01;
            resp.time   = pin->time;
            udp_send(fd_audio, &resp, sizeof(resp), &client_audio);
        }
        return;
    }

    // Handshake: associate audio client with control client by IP
    if (len == CONTROL_SIZE) {
        if (hdr->type == 0x03) {
            // Must have an authenticated control client from the same IP
            if (!client_ctrl_valid || client_ctrl.sin_addr.s_addr != src->sin_addr.s_addr) {
                LV_LOG_WARN("LAN/AUDIO: AYT from unknown IP, ignoring");
                return;
            }
            client_audio       = *src;
            client_audio_valid = true;
            remote_id          = hdr->sentid;
            audio_send_seq     = 0;

            // Create PulseAudio playback stream for incoming network audio
            if (!wfview_player) {
                wfview_player = g_ports->audio->create_player((uint32_t)audio_rx_rate, 1);
                if (!wfview_player) {
                    LV_LOG_ERROR("LAN/AUDIO: failed to create audio player");
                } else {
                    LV_LOG_INFO("LAN/AUDIO: created player at %d Hz", audio_rx_rate);
                }
            }

            // Register DSP callback to forward RX audio to network
            if (dsp_audio_sub_id == DSP_AUDIO_SUB_INVALID) {
                dsp_audio_sub_id = g_ports->dsp_audio->subscribe_float(audio_lan_tx_cb, audio_rx_rate);
            }
            g_ports->dsp_audio->set_active(dsp_audio_sub_id, true);

            send_control(fd_audio, 0x04, 0, false, &client_audio);
            send_control(fd_audio, 0x06, 0x01, false, &client_audio);
            LV_LOG_INFO("LAN/AUDIO: handshake");
        } else if (hdr->type == 0x06) {
            remote_id = hdr->sentid;
            LV_LOG_INFO("LAN/AUDIO: ready");
        } else if (hdr->type == CTL_TYPE_CLOSE) {
            LV_LOG_INFO("LAN/AUDIO: close received");
            cleanup_audio();
        }
        return;
    }

    // Audio data packet
    if (len >= AUDIO_SIZE) {
        if (!client_audio_valid) return;

        const audio_packet_t *ap = (const audio_packet_t *)buf;
        if (ap->ident != 0x0080) return;

        uint16_t datalen = ntohs(ap->datalen);
        size_t payload_offset = AUDIO_SIZE;
        if (payload_offset + datalen > len) return;

        if (wfview_player) {
            const int16_t *pcm = (const int16_t *)(buf + payload_offset);
            size_t nsamples = datalen / 2;
            g_ports->audio->player_send(wfview_player, const_cast<int16_t *>(pcm), nsamples);
        }
    }
}

// ---- Audio TX thread, callbacks, cleanup -----------------------------------

static void cleanup_audio() {
    if (dsp_audio_sub_id != DSP_AUDIO_SUB_INVALID){
        g_ports->dsp_audio->unsubscribe(dsp_audio_sub_id);
        dsp_audio_sub_id = DSP_AUDIO_SUB_INVALID;
    }

    if (wfview_player) {
        g_ports->audio->player_release(wfview_player);
        wfview_player = nullptr;
    }

    client_audio_valid = false;
}

static void audio_lan_tx_cb(size_t n, float *samples) {
    if (!client_audio_valid) return;

    int16_t pkt_buf[n];
    vector_f_to_s16(samples, pkt_buf, n);

    audio_packet_t hdr = make_audio_header(0, audio_send_seq++,
        n * 2);
    uint8_t out[sizeof(hdr) + n * 2];
    std::memcpy(out, &hdr, sizeof(hdr));
    std::memcpy(out + sizeof(hdr), pkt_buf, sizeof(pkt_buf));
    sendto(fd_audio, out, sizeof(out), 0,
            (const sockaddr *)&client_audio, sizeof(client_audio));
}

// ---- CI-V port processing ------------------------------------------------

static void process_civ_packet(const uint8_t *buf, size_t len, const sockaddr_in *src, CivTxPacker &resp_packer) {
    if (len < sizeof(control_packet_t)) return;
    const control_packet_t *hdr = (const control_packet_t *)buf;

    if (hdr->type == 0x01) {
        handle_retransmit(fd_civ, buf, len, src);
        return;
    }

    switch (len) {
    case CONTROL_SIZE:
        if (hdr->type == 0x03) {
            client_civ       = *src;
            client_civ_valid = true;
            remote_id        = hdr->sentid;
            send_control(fd_civ, 0x04, 0, false, &client_civ);
            send_control(fd_civ, 0x06, 0x01, false, &client_civ);
            LV_LOG_INFO("LAN/CIV: handshake");
        } else if (hdr->type == 0x06) {
            remote_id = hdr->sentid;
            LV_LOG_INFO("LAN/CIV: ready");
        }
        break;

    case PING_SIZE: {
        // Ping on CIV port
        const ping_packet_t *pin = (const ping_packet_t *)buf;
        if (pin->type == 0x07 && pin->reply == 0x00) {
            ping_packet_t resp;
            std::memset(&resp, 0, sizeof(resp));
            resp.len    = sizeof(resp);
            resp.type   = 0x07;
            resp.seq    = pin->seq;
            resp.sentid = my_id;
            resp.rcvdid = remote_id;
            resp.reply  = 0x01;
            resp.time   = pin->time;
            udp_send(fd_civ, &resp, sizeof(resp), &client_civ);
        }
        break;
    }

    case OPENCLOSE_SIZE: {
        const openclose_packet_t *oc = (const openclose_packet_t *)buf;
        if (oc->magic == MAGIC_OPEN) {
            LV_LOG_INFO("LAN/CIV: open");
            openclose_packet_t resp;
            std::memset(&resp, 0, sizeof(resp));
            resp.len     = sizeof(resp);
            resp.sentid  = my_id;
            resp.rcvdid  = remote_id;
            resp.data    = oc->data;
            resp.sendseq = htons(civ_send_seq++);
            resp.magic   = MAGIC_OPEN;
            send_tracked(fd_civ, (const uint8_t *)&resp, sizeof(resp), &client_civ);
        } else if (oc->magic == MAGIC_CLOSE) {
            LV_LOG_INFO("LAN/CIV: close received");
            client_civ_valid = false;
            scope_streamer_set_notify(nullptr);
        }
        break;
    }

    default:
        if (len > PING_SIZE) {
            const ping_packet_t *in = (const ping_packet_t *)buf;
            if (in->type != 0x01 && in->reply == (uint8_t)0xC1) {
                uint16_t civ_dlen = in->datalen;
                size_t payload_offset = PING_SIZE;
                if (payload_offset + civ_dlen <= len && civ_dlen >= FRAME_ADD_LEN + 2) {
                    const uint8_t *civ_payload = buf + payload_offset;
                    CivPacketView req(civ_payload, civ_dlen);
                    auto resp = process_civ_message(req, resp_packer);
                    if (!resp.empty()) {
                        civ_data_send(reinterpret_cast<const uint8_t *>(resp.data()), resp.size());
                    }
                }
            }
        }
        break;
    }
}

// ---- Retransmit / maintenance --------------------------------------------

static void check_retransmit(void) {
    if (rx_missing.empty()) return;
    if (rx_missing.size() > MAX_MISSING) {
        std::lock_guard<std::mutex> lock(missing_mutex);
        rx_missing.clear();
        return;
    }

    std::vector<uint16_t> to_request;
    {
        std::lock_guard<std::mutex> lock(missing_mutex);
        for (auto it = rx_missing.begin(); it != rx_missing.end(); ) {
            if (it->second < 4) {
                it->second++;
                to_request.push_back(it->first);
                ++it;
            } else {
                it = rx_missing.erase(it);
            }
        }
    }

    if (to_request.empty()) return;

    std::vector<uint8_t> pkt(CONTROL_SIZE);
    control_packet_t *hdr = (control_packet_t *)pkt.data();
    hdr->len    = CONTROL_SIZE;
    hdr->type   = 0x01;
    hdr->sentid = my_id;
    hdr->rcvdid = remote_id;

    if (to_request.size() == 1) {
        hdr->seq = to_request[0];
        if (client_ctrl_valid)
            udp_send(fd_control, pkt.data(), pkt.size(), &client_ctrl);
    } else {
        pkt.reserve(CONTROL_SIZE + to_request.size() * 4);
        for (uint16_t s : to_request) {
            pkt.push_back((uint8_t)(s & 0xFF));
            pkt.push_back((uint8_t)(s >> 8));
            pkt.push_back((uint8_t)(s & 0xFF));
            pkt.push_back((uint8_t)(s >> 8));
        }
        hdr = (control_packet_t *)pkt.data();
        hdr->len = (uint32_t)pkt.size();
        hdr->seq = 0;
        if (client_ctrl_valid)
            udp_send(fd_control, pkt.data(), pkt.size(), &client_ctrl);
    }
}

static void purge_buffer(void) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(tx_mutex);
    for (auto it = tx_buffer.begin(); it != tx_buffer.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.sent_at).count();
        if (age > PURGE_MS) {
            it = tx_buffer.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- CIV notification helper + callbacks ----------------------------------

static void push_civ_notify_lan(std::string_view resp) {
    send_queue.push(std::vector<uint8_t>(resp.begin(), resp.end()));
    if (fd_event >= 0) {
        uint64_t u = 1;
        ssize_t r = write(fd_event, &u, sizeof(u));
        (void)r;
    }
}

static void on_fg_freq_change_cb(Subject *s, void *user_data) {
    if (auth_state < ST_CIV_ACTIVE) return;
    int32_t freq = cfg.cur.fg_freq()->get();
    scope_streamer_set_center_freq(freq);
    uint8_t buf[16];
    CivTxPacker packer{buf, 0, LOCAL_ADDRESS};
    push_civ_notify_lan(pack_fg_freq_notify_00(freq, packer));
}

static void on_mode_change_cb(Subject *s, void *user_data) {
    if (auth_state < ST_CIV_ACTIVE) return;
    uint8_t buf[16];
    CivTxPacker packer{buf, 0, LOCAL_ADDRESS};
    push_civ_notify_lan(pack_mode_notify_01(
        static_cast<x6100_mode_t>(cfg.cur.mode()->get()), packer));
}

static void on_vfo_change_cb(Subject *s, void *user_data) {
    if (auth_state < ST_CIV_ACTIVE) return;
    uint8_t buf[16];
    CivTxPacker packer{buf, 0, LOCAL_ADDRESS};
    push_civ_notify_lan(pack_vfo_notify_07(
        static_cast<x6100_vfo_t>(cfg.band.current_vfo()->get()), packer));
}

// ---- Thread ---------------------------------------------------------------

static void cat_lan_thread() {
    diag_set_source("CAT LAN");
    uint8_t recv_buf[65536];

    uint8_t tx_buf[64];
    CivTxPacker resp_packer{tx_buf, 0, LOCAL_ADDRESS};

    while (keep_running) {
        struct pollfd fds[4];
        std::memset(fds, 0, sizeof(fds));

        fds[0].fd     = fd_control;
        fds[0].events = POLLIN;
        fds[1].fd     = fd_civ;
        fds[1].events = POLLIN;
        fds[2].fd     = fd_event;
        fds[2].events = POLLIN;
        fds[3].fd     = fd_audio;
        fds[3].events = POLLIN;

        int ret = poll(fds, 4, POLL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // Control port
        if (fds[0].revents & POLLIN) {
            sockaddr_in src;
            socklen_t src_len = sizeof(src);
            ssize_t n = recvfrom(fd_control, recv_buf, sizeof(recv_buf), 0,
                                 (sockaddr *)&src, &src_len);
            if (n > 0) {
                const control_packet_t *hdr = (const control_packet_t *)recv_buf;
                if (hdr->type == 0 && (size_t)n == LOGIN_SIZE) {
                    handle_login_packet(recv_buf, (size_t)n);
                } else if (hdr->type == 0 && (size_t)n == TOKEN_SIZE) {
                    handle_token_packet(recv_buf, (size_t)n);
                } else if (hdr->type == 0 && (size_t)n == CONNINFO_SIZE) {
                    handle_conninfo_packet(recv_buf, (size_t)n);
                } else {
                    process_control_packet(recv_buf, (size_t)n, &src);
                }
            }
        }

        // CIV port
        if (fds[1].revents & POLLIN) {
            sockaddr_in src;
            socklen_t src_len = sizeof(src);
            ssize_t n = recvfrom(fd_civ, recv_buf, sizeof(recv_buf), 0,
                                 (sockaddr *)&src, &src_len);
            if (n > 0) {
                process_civ_packet(recv_buf, (size_t)n, &src, resp_packer);
            }
        }

        // Event fd (queue trigger)
        if (fds[2].revents & POLLIN) {
            uint64_t u;
            ssize_t r = read(fd_event, &u, sizeof(u));
            (void)r;

            std::vector<uint8_t> data;
            while (send_queue.try_pop(data)) {
                if (!data.empty() && client_civ_valid) {
                    civ_data_send(data.data(), data.size());
                }
            }
        }

        // Audio port
        if (fd_audio >= 0 && fds[3].revents & POLLIN) {
            sockaddr_in src;
            socklen_t src_len = sizeof(src);
            ssize_t n = recvfrom(fd_audio, recv_buf, sizeof(recv_buf), 0,
                                 (sockaddr *)&src, &src_len);
            if (n > 0) {
                process_audio_packet(recv_buf, (size_t)n, &src);
            }
        }

        // Periodic maintenance
        if (auth_state >= ST_CIV_ACTIVE && client_ctrl_valid) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_ping_time).count();
            if (elapsed >= PING_TIMEOUT_SEC) {
                LV_LOG_WARN("LAN: ping timeout (%llds), disconnecting", (long long)elapsed);
                cleanup_audio();
                client_ctrl_valid = false;
                client_civ_valid  = false;
                scope_streamer_set_notify(nullptr);
                auth_state = ST_LISTENING;
                {
                    std::lock_guard<std::mutex> lock(tx_mutex);
                    tx_buffer.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(missing_mutex);
                    rx_missing.clear();
                }
                send_queue.clear();
            }
        }
        check_retransmit();
        purge_buffer();
    }
}

// ---- Public API -----------------------------------------------------------

int cat_lan_init(const app_ports_t *ports) {
    g_ports = ports;

    if (scope_psd_sub_id == PSD_SUB_INVALID) {
        scope_psd_sub_id = g_ports->psd->subscribe(scope_psd_cb, SCOPE_NBINS, DSP_FRAME_DEFAULT_CHUNKS, nullptr);
        scope_streamer_set_active_cb(scope_active_cb);
        scope_streamer_set_cadence_cb(scope_cadence_cb);
    }

    if (fd_control >= 0) {
        LV_LOG_WARN("LAN CAT already initialized");
        return 0;
    }

    fd_control = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_control < 0) { perror("cat_lan: socket control"); return -1; }

    int opt = 1;
    setsockopt(fd_control, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(CONTROL_PORT);

    if (bind(fd_control, (sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("cat_lan: bind control");
        close(fd_control); fd_control = -1; return -1;
    }
    fcntl(fd_control, F_SETFL, O_NONBLOCK);

    fd_civ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_civ < 0) { perror("cat_lan: socket civ"); close(fd_control); fd_control = -1; return -1; }

    setsockopt(fd_civ, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(CIV_PORT);

    if (bind(fd_civ, (sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("cat_lan: bind civ");
        close(fd_civ); fd_civ = -1;
        close(fd_control); fd_control = -1;
        return -1;
    }
    fcntl(fd_civ, F_SETFL, O_NONBLOCK);

    fd_event = eventfd(0, EFD_NONBLOCK);
    if (fd_event < 0) {
        perror("cat_lan: eventfd");
        close(fd_civ); fd_civ = -1;
        close(fd_control); fd_control = -1;
        return -1;
    }

    // Audio socket
    fd_audio = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_audio < 0) {
        perror("cat_lan: socket audio");
        close(fd_event); fd_event = -1;
        close(fd_civ); fd_civ = -1;
        close(fd_control); fd_control = -1;
        return -1;
    }
    setsockopt(fd_audio, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(AUDIO_PORT);
    if (bind(fd_audio, (sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("cat_lan: bind audio");
        close(fd_audio); fd_audio = -1;
        close(fd_event); fd_event = -1;
        close(fd_civ); fd_civ = -1;
        close(fd_control); fd_control = -1;
        return -1;
    }
    fcntl(fd_audio, F_SETFL, O_NONBLOCK);

    my_id      = make_my_id(CONTROL_PORT);
    auth_state = ST_LISTENING;

    LV_LOG_INFO("LAN CAT listening on control port %d, CIV port %d, audio port %d",
                CONTROL_PORT, CIV_PORT, AUDIO_PORT);

    sub_freq = Subscription(cfg.cur.fg_freq()->subscribe(on_fg_freq_change_cb));
    sub_mode = Subscription(cfg.cur.mode()->subscribe(on_mode_change_cb));
    sub_vfo  = Subscription(cfg.band.current_vfo()->subscribe(on_vfo_change_cb));

    keep_running = true;
    thread = new std::thread(cat_lan_thread);

    return 0;
}

void cat_lan_destruct(void) {
    scope_streamer_set_notify(nullptr);
    scope_streamer_set_active_cb(nullptr);
    scope_streamer_set_cadence_cb(nullptr);

    if (g_ports && scope_psd_sub_id != PSD_SUB_INVALID) {
        g_ports->psd->unsubscribe(scope_psd_sub_id);
        scope_psd_sub_id = PSD_SUB_INVALID;
    }

    if (!keep_running) return;

    g_ports->dsp_audio->unsubscribe(dsp_audio_sub_id);
    dsp_audio_sub_id = DSP_AUDIO_SUB_INVALID;

    cleanup_audio();

    if (thread) {
        keep_running = false;
        // Wake the poll loop so the thread exits promptly
        if (fd_event >= 0) {
            uint64_t u = 1;
            ssize_t res = write(fd_event, &u, sizeof(u));
            (void)res;
        }
        thread->join();
        delete thread;
        thread = nullptr;
    }
    if (fd_audio >= 0)         { close(fd_audio);         fd_audio         = -1; }
    if (fd_control >= 0)       { close(fd_control);       fd_control       = -1; }
    if (fd_civ >= 0)           { close(fd_civ);           fd_civ           = -1; }
    if (fd_event >= 0)         { close(fd_event);         fd_event         = -1; }

    client_ctrl_valid = false;
    client_civ_valid  = false;
    client_audio_valid = false;
    auth_state        = ST_LISTENING;
    token             = 0;
    send_seq          = 1;
    civ_send_seq      = 0;
    auth_seq          = 0x30;
    tok_request       = 0;

    sub_freq.reset();
    sub_mode.reset();
    sub_vfo.reset();

    {
        std::lock_guard<std::mutex> lock(tx_mutex);
        tx_buffer.clear();
    }
    {
        std::lock_guard<std::mutex> lock(missing_mutex);
        rx_missing.clear();
    }
    send_queue.clear();

    LV_LOG_INFO("LAN CAT shut down");
}
