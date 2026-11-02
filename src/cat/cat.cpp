/*
 * X6100 CAT I/O — UART + Bluetooth + event queue dispatcher.
 *
 * Frame parsing, command handler dispatch, and BCD conversion live in
 * cat_frame.cpp; this file owns the poll loop, socket management, and
 * frequency-change echo.
 */

#include "cat.h"
#include "../diag_log.h"
#include "civ_processor.h"
#include "civ_protocol.h"
#include "scope_streamer.h"

#include <mutex>
#include <thread>
#include <vector>
#include <cmath>
#include <chrono>
#include <string>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include "lvgl/lvgl.h"

#include "../cfg/cfg_api.h"
#include "../common/queue.h"

extern "C" {
    #include <aether_radio/x6100_control/low/gpio.h>
    #include <fcntl.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <sys/poll.h>
    #include <termios.h>
    #include <unistd.h>
}

static int fd_wire = -1;
static int fd_bt = -1;
static int fd_queue_event = -1;

static TSQueue<std::vector<char>> send_queue;
static std::thread* thread = nullptr;
static std::atomic<bool> keep_running(false);

static void on_fg_freq_change(Subject *s, void *user_data);
static void on_mode_change(Subject *s, void *user_data);
static void on_vfo_change(Subject *s, void *user_data);
static void on_cat_baud_change(Subject *s, void *user_data);

static void push_civ_notify(std::string_view resp) {
    send_queue.push(std::vector<char>(resp.begin(), resp.end()));
    if (fd_queue_event >= 0) {
        uint64_t u = 1;
        ssize_t ret = write(fd_queue_event, &u, sizeof(u));
        (void)ret;
    }
}

struct FeedResult {
    int status;
    std::optional<CivPacketView> frame;
};

class Connection {
    int       *fd;
    uint8_t    buf[1024];
    const unsigned char header[2] = {FRAME_PRE, FRAME_PRE};
    size_t     len       = 0;

  protected:
    bool write_buf(const char *buf, size_t len) {
        ssize_t l;
        while (len) {
            l = write(*fd, buf, len);
            if (l < 0) {
                perror("Error during writing message");
                return false;
            }
            len -= l;
        }
        return true;
    }

  public:
    Connection(int *fd) : fd(fd) {};

    FeedResult feed() {
        int res = read(*fd, buf + len, sizeof(buf) - len);
        if (res < 0) {
            return {res, std::nullopt};
        }
        len += res;
        uint8_t *frame_start = (uint8_t *)memmem(buf, len, header, sizeof(header));
        if (frame_start == NULL) {
            buf[0] = buf[len - 1];
            len    = 1;
            return {0, std::nullopt};
        }
        while (*(frame_start + 2) == FRAME_PRE) {
            frame_start++;
            len--;
            if (!len) {
                return {0, std::nullopt};
            }
        }
        if (frame_start != buf) {
            memmove(buf, frame_start, len);
        }
        if (len > FRAME_ADD_LEN) {
            uint8_t *frame_end = (uint8_t *)memchr(buf + FRAME_ADD_LEN, FRAME_END, len - FRAME_ADD_LEN);
            if (frame_end) {
                size_t frame_len = frame_end - buf + 1;
                len              = 0;
                return {res, CivPacketView{buf, frame_len}};
            }
        }
        return {0, std::nullopt};
    }

    bool send(const char * data, size_t len) {
        return write_buf(data, len);
    }
    bool send(std::string_view &data) {
        return write_buf(data.data(), data.size());
    }
};

static void cat_thread() {
    diag_set_source("CAT USB/BT");
    Connection conn_wire{&fd_wire};
    Connection conn_bt{&fd_bt};

    Connection *conn = &conn_wire;

    // TX (egress) buffres
    uint8_t tx_buf[64];
    CivTxPacker resp{tx_buf, 0, LOCAL_ADDRESS};

    // Setup BT socket
    int fd_bt_sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);

    struct sockaddr_rc addr = { 0 };
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = 1;
    // mask 00:00:00:00:00:00
    for(int i=0; i<6; i++) addr.rc_bdaddr.b[i] = 0;

    // Paired (bonded) devices only: CAT can key the transmitter. The
    // kernel default lets anyone in range connect without pairing.
    // New devices pair through the Bluetooth window.
    struct bt_security sec = {};
    sec.level = BT_SECURITY_MEDIUM;
    if (fd_bt_sock >= 0 && setsockopt(fd_bt_sock, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) < 0) {
        // Not open to anyone instead: no CAT over Bluetooth (poll skips
        // fd -1), CAT over USB as usual.
        LV_LOG_ERROR("Bluetooth CAT off: can't require pairing (%s)", strerror(errno));
        close(fd_bt_sock);
        fd_bt_sock = -1;
    }

    if (fd_bt_sock >= 0) {
        bind(fd_bt_sock, (struct sockaddr *)&addr, sizeof(addr));
        listen(fd_bt_sock, 1);
        fcntl(fd_bt_sock, F_SETFL, O_NONBLOCK);
    }

    struct pollfd fds[4];

    while (keep_running) {
        // Setup polls
        fds[0].fd = fd_wire;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        fds[1].fd = fd_bt;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        fds[2].fd = fd_queue_event;
        fds[2].events = POLLIN;
        fds[2].revents = 0;

        fds[3].fd = fd_bt_sock;
        fds[3].events = POLLIN;
        fds[3].revents = 0;

        int ret = poll(fds, 4, -1);
        if (ret < 0) {
            perror("poll error");
            break;
        }

        // New BT connection
        if (fds[3].revents & POLLIN) {
            struct sockaddr_rc rem_addr = { 0 };
            socklen_t opt = sizeof(rem_addr);

            int new_fd = accept(fd_bt_sock, (struct sockaddr *)&rem_addr, &opt);
            if (new_fd >= 0) {
                if (fd_bt >= 0) close(fd_bt);
                fd_bt = new_fd;
                fcntl(fd_bt, F_SETFL, O_NONBLOCK);
                char bt_addr[18] = {0};
                ba2str(&rem_addr.rc_bdaddr, bt_addr);
                LV_LOG_USER("New Bluetooth connection %s", bt_addr);
                conn = &conn_bt;
            }
        }

        // Wire data
        if (fds[0].revents & POLLIN) {
            auto res = conn_wire.feed();
            if (res.frame) {
                auto raw_resp = process_civ_message(res.frame.value(), resp);
                conn_wire.send(raw_resp);
                conn = &conn_wire;
            }
        }

        // BT data
        if (fd_bt >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            auto res = conn_bt.feed();
            if (res.frame) {
                auto raw_resp = process_civ_message(res.frame.value(), resp);
                conn_bt.send(raw_resp);
                conn = &conn_bt;
            }
            else if (res.status <= 0) {
                LV_LOG_USER("Bluetooth disconnected");
                close(fd_bt);
                fd_bt = -1;
                conn = &conn_wire;
            }
        }

        // Queue
        if (fds[2].revents & POLLIN) {
            uint64_t u;
            ssize_t ret;
            ret = read(fd_queue_event, &u, sizeof(uint64_t)); // Reset trigger

            auto data = send_queue.pop();
            bool ok = conn->send(data.data(), data.size());
            if (!ok && conn == &conn_bt) {
                LV_LOG_ERROR("Bluetooth write error, switch to wire");
                conn = &conn_wire;
                conn->send(data.data(), data.size());
            }
        }
    }
}

void cat_init(const app_ports_t *ports) {
    civ_set_ports(ports);

    /* UART */
    x6100_gpio_set(x6100_pin_usb, 1); /* USB -> CAT */

    fd_wire = open("/dev/ttyS2", O_RDWR | O_NONBLOCK | O_NOCTTY);

    fd_bt = -1;

    fd_queue_event = eventfd(0, EFD_NONBLOCK);

    if (fd_wire > 0) {
        struct termios attr;

        tcgetattr(fd_wire, &attr);

        speed_t speed = (cfg.cat_baud()->get() >= 115200) ? B115200 : B19200;
        cfsetispeed(&attr, speed);
        cfsetospeed(&attr, speed);
        cfmakeraw(&attr);

        if (tcsetattr(fd_wire, 0, &attr) < 0) {
            close(fd_wire);
            LV_LOG_ERROR("UART set speed");
            return;
        }
    } else {
        LV_LOG_ERROR("UART open");
        return;
    }

    cfg.cur.fg_freq()->subscribe(on_fg_freq_change);
    cfg.cur.mode()->subscribe(on_mode_change);
    cfg.band.current_vfo()->subscribe(on_vfo_change);
    cfg.cat_baud()->subscribe(on_cat_baud_change);

    // CI-V waterfall streaming notify: only if baud >= 115200
    // (LAN connection registration in cat/lan will override this)
    if (cfg.cat_baud()->get() >= 115200) {
        scope_streamer_set_notify(push_civ_notify);
    }

    /* * */
    if (!thread) {
        keep_running = true;
        thread = new std::thread(cat_thread);
    }
}

void cat_destruct() {
    keep_running = false;
    thread->join();
    close(fd_wire);
    close(fd_queue_event);
    if (fd_bt >= 0) close(fd_bt);
}

static void on_fg_freq_change(Subject *s, void *user_data) {
    int32_t freq = cfg.cur.fg_freq()->get();
    scope_streamer_set_center_freq(freq);
    uint8_t buf[16];
    CivTxPacker packer{buf, 0, LOCAL_ADDRESS};
    push_civ_notify(pack_fg_freq_notify_00(freq, packer));
}

static void on_mode_change(Subject *s, void *user_data) {
    uint8_t buf[16];
    CivTxPacker packer{buf, 0, LOCAL_ADDRESS};
    push_civ_notify(pack_mode_notify_01(
        static_cast<x6100_mode_t>(cfg.cur.mode()->get()), packer));
}

static void on_vfo_change(Subject *s, void *user_data) {
    uint8_t buf[16];
    CivTxPacker packer{buf, 0, LOCAL_ADDRESS};
    push_civ_notify(pack_vfo_notify_07(
        static_cast<x6100_vfo_t>(cfg.band.current_vfo()->get()), packer));
}

static void on_cat_baud_change(Subject *s, void *user_data) {
    if (fd_wire < 0) return;
    struct termios attr;
    tcgetattr(fd_wire, &attr);
    speed_t speed = (cfg.cat_baud()->get() >= 115200) ? B115200 : B19200;
    cfsetispeed(&attr, speed);
    cfsetospeed(&attr, speed);
    tcsetattr(fd_wire, 0, &attr);
}
