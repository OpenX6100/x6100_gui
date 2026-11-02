/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 * bt_spp.c - Bluetooth SPP (RFCOMM) frame server for the X6100 GUI.
 *
 * Wire format, one frame per datagram:
 *
 *     uint32 be   payload length n, 1 .. BT_SPP_MAX_FRAME
 *     n bytes     payload, byte for byte the datagram ft8_udp sends
 *
 * Three things here are load-bearing:
 *
 *  - bt_spp_send() is called from the UI thread and only copies into a
 *    queue. A blocking write from that thread froze the interface once
 *    when turning the knob, and RFCOMM blocks far more readily than UDP
 *    because of its credit window.
 *
 *  - a partial write advances an offset into the current frame. Resending
 *    the head of a buffer after a short write would, on a framed stream,
 *    not merely duplicate data but desynchronise the reader permanently.
 *
 *  - every frame is freed, on send, on disconnect and on stop.
 */

#include "bt_spp.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#define QUEUE_LEN 64

typedef struct {
    uint8_t *data; /* length prefix and payload in one allocation */
    size_t   len;
} frame_t;

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    bool            thread_valid;
    bool            running;

    int             wake[2]; /* self pipe, [0] read, [1] write */
    int             srv;
    int             cli;

    frame_t         q[QUEUE_LEN];
    int             head;
    int             tail;
    int             count;
    size_t          out_off; /* bytes of q[head] already written */

    bt_spp_state_t  state;
    char            peer[18];
    uint32_t        sent;
    uint32_t        dropped;
} S = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .srv  = -1,
    .cli  = -1,
    .wake = { -1, -1 },
};

/* ------------------------------------------------------------------ */

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/* Caller holds the lock. */
static void queue_clear(void)
{
    while (S.count > 0) {
        free(S.q[S.head].data);
        S.q[S.head].data = NULL;
        S.head = (S.head + 1) % QUEUE_LEN;
        S.count--;
    }
    S.head = S.tail = 0;
    S.out_off = 0;
}

/*
 * Closing an RFCOMM socket can block in the kernel (__lock_sock, in
 * uninterruptible sleep) when the Bluetooth stack is busy - seen with a
 * phone reconnecting next to a headset. So no Bluetooth socket is ever
 * closed with S.lock held (the GUI thread takes S.lock for every frame
 * it queues), and the GUI thread never closes one itself: bt_spp_stop()
 * hands them to a short-lived detached thread.
 */
static void *closer_thread(void *arg)
{
    int *fds = arg;
    int  i;

    for (i = 0; fds[i] != -2; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    free(fds);
    return NULL;
}

/* Closes up to three descriptors in the background. */
static void close_async(int a, int b, int c)
{
    pthread_t      t;
    pthread_attr_t at;
    int           *fds = malloc(4 * sizeof(int));

    if (fds == NULL) {
        return;     /* leak the descriptors rather than block */
    }
    fds[0] = a;
    fds[1] = b;
    fds[2] = c;
    fds[3] = -2;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, closer_thread, fds) != 0) {
        free(fds);
    }
    pthread_attr_destroy(&at);
}

/* Caller holds the lock. Detaches the client and returns its descriptor
 * for the caller to close AFTER unlocking (-1 if there was none). */
static int close_client(void)
{
    int fd = S.cli;

    S.cli = -1;
    S.peer[0] = '\0';
    queue_clear();
    if (S.state == BT_SPP_CONNECTED) {
        S.state = BT_SPP_LISTENING;
    }
    return fd;
}

static void wake_worker(void)
{
    char c = 1;
    ssize_t n = write(S.wake[1], &c, 1);

    (void)n; /* a full pipe already means the worker is about to run */
}

/* ------------------------------------------------------------------ */

static void do_accept(void)
{
    struct sockaddr_rc rem;
    socklen_t          len = sizeof(rem);
    int                fd;

    memset(&rem, 0, sizeof(rem));
    fd = accept(S.srv, (struct sockaddr *)&rem, &len);
    if (fd < 0) {
        return;
    }

    pthread_mutex_lock(&S.lock);
    if (S.cli >= 0) {
        /* One consumer is the whole point. Refuse the second politely
         * rather than interleaving two readers on one frame stream. */
        pthread_mutex_unlock(&S.lock);
        close(fd);
        return;
    }

    set_nonblock(fd);
    S.cli = fd;
    S.state = BT_SPP_CONNECTED;
    ba2str(&rem.rc_bdaddr, S.peer);
    S.out_off = 0;
    pthread_mutex_unlock(&S.lock);
}

/* Returns false when the client went away. */
static bool drain_input(void)
{
    uint8_t scratch[64];

    for (;;) {
        ssize_t n = read(S.cli, scratch, sizeof(scratch));

        if (n > 0) {
            continue; /* the phone does not talk back; discard anything */
        }
        if (n == 0) {
            return false; /* orderly close */
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
}

/* Returns false when the client went away. */
static bool flush_output(void)
{
    for (;;) {
        uint8_t *data;
        size_t   len;
        size_t   off;
        ssize_t  n;

        pthread_mutex_lock(&S.lock);
        if (S.count == 0 || S.cli < 0) {
            pthread_mutex_unlock(&S.lock);
            return true;
        }
        data = S.q[S.head].data;
        len  = S.q[S.head].len;
        off  = S.out_off;
        pthread_mutex_unlock(&S.lock);

        n = send(S.cli, data + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            pthread_mutex_lock(&S.lock);
            S.out_off += (size_t)n;
            if (S.out_off >= S.q[S.head].len) {
                free(S.q[S.head].data);
                S.q[S.head].data = NULL;
                S.head = (S.head + 1) % QUEUE_LEN;
                S.count--;
                S.out_off = 0;
                S.sent++;
            }
            pthread_mutex_unlock(&S.lock);
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true; /* window full, poll will say when it opens */
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

static void *worker(void *arg)
{
    (void)arg;

    while (true) {
        struct pollfd p[3];
        int           nfds = 0;
        int           i_wake, i_srv = -1, i_cli = -1;
        bool          have_out;
        int           cli;

        pthread_mutex_lock(&S.lock);
        if (!S.running) {
            pthread_mutex_unlock(&S.lock);
            break;
        }
        cli = S.cli;
        have_out = (S.count > 0);
        pthread_mutex_unlock(&S.lock);

        i_wake = nfds;
        p[nfds].fd = S.wake[0];
        p[nfds].events = POLLIN;
        nfds++;

        if (cli < 0) {
            i_srv = nfds;
            p[nfds].fd = S.srv;
            p[nfds].events = POLLIN;
            nfds++;
        } else {
            i_cli = nfds;
            p[nfds].fd = cli;
            p[nfds].events = POLLIN | (have_out ? POLLOUT : 0);
            nfds++;
        }

        if (poll(p, nfds, 1000) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (p[i_wake].revents & POLLIN) {
            char scratch[64];
            while (read(S.wake[0], scratch, sizeof(scratch)) > 0) {
                ;
            }
        }

        if (i_srv >= 0 && (p[i_srv].revents & POLLIN)) {
            do_accept();
        }

        if (i_cli >= 0 && p[i_cli].revents) {
            bool alive = true;

            if (p[i_cli].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                alive = false;
            }
            if (alive && (p[i_cli].revents & POLLIN)) {
                alive = drain_input();
            }
            if (alive && (p[i_cli].revents & POLLOUT)) {
                alive = flush_output();
            }
            if (!alive) {
                int fd;

                pthread_mutex_lock(&S.lock);
                fd = close_client();
                pthread_mutex_unlock(&S.lock);
                if (fd >= 0) {
                    close(fd);      /* worker thread, lock released */
                }
            }
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */

bool bt_spp_start(uint8_t channel)
{
    struct sockaddr_rc loc;
    struct bt_security sec;

    pthread_mutex_lock(&S.lock);
    if (S.running) {
        pthread_mutex_unlock(&S.lock);
        return true;
    }

    S.srv = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (S.srv < 0) {
        S.state = BT_SPP_ERROR;
        pthread_mutex_unlock(&S.lock);
        return false;
    }

    /* Low security means the phone can open the channel without bonding.
     * With BT_SECURITY_MEDIUM the connection silently waits for a pairing
     * agent that may not be running. */
    memset(&sec, 0, sizeof(sec));
    sec.level = BT_SECURITY_LOW;
    if (setsockopt(S.srv, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) < 0) {
        /* Not fatal on every kernel; carry on. */
    }

    memset(&loc, 0, sizeof(loc));
    loc.rc_family = AF_BLUETOOTH;
    loc.rc_bdaddr = *BDADDR_ANY;
    loc.rc_channel = channel;

    if (bind(S.srv, (struct sockaddr *)&loc, sizeof(loc)) < 0 ||
        listen(S.srv, 1) < 0) {
        close(S.srv);
        S.srv = -1;
        S.state = BT_SPP_ERROR;
        pthread_mutex_unlock(&S.lock);
        return false;
    }
    set_nonblock(S.srv);

    if (pipe(S.wake) < 0) {
        close(S.srv);
        S.srv = -1;
        S.state = BT_SPP_ERROR;
        pthread_mutex_unlock(&S.lock);
        return false;
    }
    set_nonblock(S.wake[0]);
    set_nonblock(S.wake[1]);

    S.head = S.tail = S.count = 0;
    S.out_off = 0;
    S.peer[0] = '\0';
    S.running = true;
    S.state = BT_SPP_LISTENING;
    pthread_mutex_unlock(&S.lock);

    if (pthread_create(&S.thread, NULL, worker, NULL) != 0) {
        pthread_mutex_lock(&S.lock);
        S.running = false;
        S.state = BT_SPP_ERROR;
        close(S.srv);
        S.srv = -1;
        close(S.wake[0]);
        close(S.wake[1]);
        S.wake[0] = S.wake[1] = -1;
        pthread_mutex_unlock(&S.lock);
        return false;
    }
    S.thread_valid = true;
    return true;
}

void bt_spp_stop(void)
{
    pthread_mutex_lock(&S.lock);
    if (!S.running) {
        pthread_mutex_unlock(&S.lock);
        return;
    }
    S.running = false;
    pthread_mutex_unlock(&S.lock);

    wake_worker();

    if (S.thread_valid) {
        pthread_join(S.thread, NULL);
        S.thread_valid = false;
    }

    {
        int cli;
        int srv;

        pthread_mutex_lock(&S.lock);
        cli = close_client();
        srv = S.srv;
        S.srv = -1;
        if (S.wake[0] >= 0) {
            close(S.wake[0]);       /* a pipe: never blocks */
            close(S.wake[1]);
            S.wake[0] = S.wake[1] = -1;
        }
        queue_clear();
        S.state = BT_SPP_STOPPED;
        pthread_mutex_unlock(&S.lock);

        /* May run on the GUI thread (FT8 window closing): the RFCOMM
         * sockets are closed in the background. */
        close_async(cli, srv, -1);
    }
}

bt_spp_state_t bt_spp_state(void)
{
    bt_spp_state_t st;

    pthread_mutex_lock(&S.lock);
    st = S.state;
    pthread_mutex_unlock(&S.lock);
    return st;
}

const char *bt_spp_peer(void)
{
    return S.peer;
}

bool bt_spp_send(const void *payload, size_t len)
{
    uint8_t *buf;

    if (payload == NULL || len == 0 || len > BT_SPP_MAX_FRAME) {
        return false;
    }

    pthread_mutex_lock(&S.lock);
    if (!S.running || S.cli < 0) {
        pthread_mutex_unlock(&S.lock);
        return false;
    }
    if (S.count >= QUEUE_LEN) {
        S.dropped++;
        pthread_mutex_unlock(&S.lock);
        return false;
    }

    buf = malloc(4 + len);
    if (buf == NULL) {
        S.dropped++;
        pthread_mutex_unlock(&S.lock);
        return false;
    }

    buf[0] = (uint8_t)((len >> 24) & 0xff);
    buf[1] = (uint8_t)((len >> 16) & 0xff);
    buf[2] = (uint8_t)((len >> 8) & 0xff);
    buf[3] = (uint8_t)(len & 0xff);
    memcpy(buf + 4, payload, len);

    S.q[S.tail].data = buf;
    S.q[S.tail].len = 4 + len;
    S.tail = (S.tail + 1) % QUEUE_LEN;
    S.count++;
    pthread_mutex_unlock(&S.lock);

    wake_worker();
    return true;
}

void bt_spp_stats(uint32_t *sent, uint32_t *dropped)
{
    pthread_mutex_lock(&S.lock);
    if (sent) {
        *sent = S.sent;
    }
    if (dropped) {
        *dropped = S.dropped;
    }
    pthread_mutex_unlock(&S.lock);
}
