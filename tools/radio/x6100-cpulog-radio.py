#!/usr/bin/env python3
"""cpulog: what the X6100's CPU cores and the GUI program's threads do, once
a second. Runs ON THE RADIO (copied there by x6100-cpulog on the PC).

Reads /proc and a few /sys files only. Never touches i2c: thermal zones whose
device sits on an i2c bus are not read (the RTC on i2c-0 shares the bus with
the base board, see the console notes).

Output (CSV, one record per line, t = Unix time on the radio):
  #  header / comments
  S,t,dt_ms,self%          a sample; dt_ms > interval = the logger ran late
  C,t,c0%,c1%..,iow%,load1,memavail_MB,freq_MHz[/..]
  N,t,tid,name,nice,policy,rtprio   a GUI thread seen for the first time
  H,t,tid,cpu%,core,wait_ms,nvcsw   a GUI thread: its CPU, last core, time
                           spent runnable but waiting for a core (schedstat),
                           involuntary context switches (preempted)
  P,t,pid,name,cpu%        another process using >= 2 %
  L,t,text                 a new line in the GUI's log (filtered)
  K,t,zone,type,temp_mC    temperature, once a minute
  M,t,rss_kB               the GUI's memory in use (VmRSS): a steady climb over
                           hours is a leak
Every 10 s a one-line summary goes to now.txt next to the output.
"""

import argparse
import os
import re
import signal
import sys
import time

LOG_RE = re.compile(r"lost|full|fail|error|warn|gap|realign|js8|drm|assert|segv|abort", re.I)


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return None


def cpu_times():
    """[(busy, total, iowait)] per core from /proc/stat."""
    out = []
    for line in read("/proc/stat").splitlines():
        if line.startswith("cpu") and line[3:4].isdigit():
            v = [int(x) for x in line.split()[1:]]
            idle, iow = v[3], v[4] if len(v) > 4 else 0
            total = sum(v[:8])
            out.append((total - idle - iow, total, iow))
    return out


def find_pid(name):
    for d in os.listdir("/proc"):
        if d.isdigit() and (read("/proc/%s/comm" % d) or "").strip() == name:
            return int(d)
    return None


def parse_stat(text):
    """(comm, fields after the comm) of a /proc/.../stat line."""
    a, b = text.index("("), text.rindex(")")
    return text[a + 1:b], text[b + 2:].split()


def thread_sample(pid):
    """{tid: (name, ticks, core, nice, policy, rtprio, wait_ns, nvcsw)}"""
    out = {}
    base = "/proc/%d/task" % pid
    try:
        tids = os.listdir(base)
    except OSError:
        return None
    for tid in tids:
        st = read("%s/%s/stat" % (base, tid))
        if not st:
            continue
        name, f = parse_stat(st)
        # f[0] = field 3 (state): utime 14, stime 15, nice 19, processor 39,
        # rt_priority 40, policy 41
        ticks = int(f[11]) + int(f[12])
        core = int(f[36]) if len(f) > 36 else -1
        nice = int(f[16])
        rtprio = int(f[37]) if len(f) > 37 else 0
        policy = int(f[38]) if len(f) > 38 else 0
        ss = read("%s/%s/schedstat" % (base, tid))
        wait_ns = int(ss.split()[1]) if ss else 0
        nvcsw = 0
        status = read("%s/%s/status" % (base, tid)) or ""
        m = re.search(r"nonvoluntary_ctxt_switches:\s*(\d+)", status)
        if m:
            nvcsw = int(m.group(1))
        out[int(tid)] = (name, ticks, core, nice, policy, rtprio, wait_ns, nvcsw)
    return out


def proc_sample(skip_pid):
    out = {}
    for d in os.listdir("/proc"):
        if not d.isdigit() or int(d) == skip_pid:
            continue
        st = read("/proc/%s/stat" % d)
        if not st:
            continue
        name, f = parse_stat(st)
        out[int(d)] = (name, int(f[11]) + int(f[12]))
    return out


def freqs():
    vals = []
    base = "/sys/devices/system/cpu/cpufreq"
    try:
        pols = sorted(p for p in os.listdir(base) if p.startswith("policy"))
    except OSError:
        return ""
    for p in pols:
        v = read("%s/%s/scaling_cur_freq" % (base, p))
        if v:
            vals.append(str(int(v) // 1000))
    return "/".join(vals)


def safe_thermal_zones():
    """Zones whose sensor isn't on an i2c bus: [(zone, type)]."""
    out = []
    base = "/sys/class/thermal"
    try:
        names = sorted(z for z in os.listdir(base) if z.startswith("thermal_zone"))
    except OSError:
        return out
    for z in names:
        dev = os.path.realpath("%s/%s/device" % (base, z)) if os.path.exists("%s/%s/device" % (base, z)) else ""
        if "i2c" in dev:
            continue
        out.append((z, (read("%s/%s/type" % (base, z)) or "?").strip()))
    return out


def meminfo_mb():
    m = re.search(r"MemAvailable:\s*(\d+)", read("/proc/meminfo") or "")
    return int(m.group(1)) // 1024 if m else -1


class AppLog:
    """New lines of the GUI's log (x6100_daemon restarts it per app run)."""

    def __init__(self, path):
        self.path, self.ino, self.pos, self.buf = path, None, 0, ""
        try:
            st = os.stat(path)
            self.ino, self.pos = st.st_ino, st.st_size  # only what comes from now on
        except OSError:
            pass

    def new_lines(self):
        try:
            st = os.stat(self.path)
        except OSError:
            return []
        if st.st_ino != self.ino or st.st_size < self.pos:
            self.ino, self.pos, self.buf = st.st_ino, 0, ""
        if st.st_size == self.pos:
            return []
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            data = f.read(256 * 1024)
        self.pos += len(data)
        self.buf += data.decode("utf-8", "replace")
        lines = self.buf.split("\n")
        self.buf = lines.pop()
        return [l.strip() for l in lines if LOG_RE.search(l)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="/tmp/cpulog")
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--proc", default="x6100_gui")
    ap.add_argument("--applog", default="/tmp/x6100_log.txt")
    ap.add_argument("--max-mb", type=float, default=20)
    a = ap.parse_args()

    os.makedirs(a.dir, exist_ok=True)
    out_path = os.path.join(a.dir, "cpulog.csv")
    now_path = os.path.join(a.dir, "now.txt")
    with open(os.path.join(a.dir, "pid"), "w") as f:
        f.write(str(os.getpid()))
    try:
        os.nice(10)  # a starved logger shows up as late samples (dt_ms)
    except OSError:
        pass

    stop = [False]

    def on_term(sig, frm):
        stop[0] = True

    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)
    signal.signal(signal.SIGHUP, signal.SIG_IGN)  # the console may come and go

    hz = os.sysconf("SC_CLK_TCK")
    out = open(out_path, "a", buffering=1)
    zones = safe_thermal_zones()
    out.write("# cpulog v1 start %.2f proc %s clk_tck %d cores %d interval %.2f python %s\n"
              % (time.time(), a.proc, hz, os.cpu_count() or 0, a.interval, sys.version.split()[0]))
    out.write("# thermal zones read: %s\n" % (" ".join("%s=%s" % z for z in zones) or "none"))

    applog = AppLog(a.applog)
    pid = None
    prev_threads, prev_procs, named = {}, {}, set()
    prev_cpu = cpu_times()
    prev_mono = time.monotonic()
    prev_self = os.times()
    end = prev_mono + a.minutes * 60
    next_t = prev_mono + a.interval
    next_k = 0.0
    window = []  # (cores_avg, gui_main%, top_name, top%) for now.txt
    last_summary = prev_mono

    while not stop[0] and time.monotonic() < end:
        delay = next_t - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        next_t += a.interval
        mono = time.monotonic()
        if next_t < mono:  # far behind: don't try to catch up
            next_t = mono + a.interval
        t = time.time()
        dt = mono - prev_mono
        prev_mono = mono

        me = os.times()
        self_pct = 100.0 * ((me[0] + me[1]) - (prev_self[0] + prev_self[1])) / dt
        prev_self = me
        out.write("S,%.2f,%d,%.1f\n" % (t, dt * 1000, self_pct))

        cur = cpu_times()
        pct = []
        iow = 0.0
        for (b0, t0, w0), (b1, t1, w1) in zip(prev_cpu, cur):
            d = (t1 - t0) or 1
            pct.append(100.0 * (b1 - b0) / d)
            iow += 100.0 * (w1 - w0) / d
        prev_cpu = cur
        load1 = (read("/proc/loadavg") or "0").split()[0]
        out.write("C,%.2f,%s,%.0f,%s,%d,%s\n" % (t, ",".join("%.0f" % p for p in pct), iow, load1,
                                                meminfo_mb(), freqs()))

        if pid is None or not os.path.exists("/proc/%d" % pid):
            pid = find_pid(a.proc)
            prev_threads = {}
            if pid:
                out.write("# %s pid %d\n" % (a.proc, pid))
        gui_main = 0.0
        top = ("-", 0.0)
        if pid:
            m = re.search(r"VmRSS:\s*(\d+)", read("/proc/%d/status" % pid) or "")
            if m:
                out.write("M,%.2f,%s\n" % (t, m.group(1)))
            th = thread_sample(pid)
            if th is None:
                pid = None
            else:
                for tid, (name, ticks, core, nice, policy, rtprio, wait_ns, nvcsw) in th.items():
                    if tid not in named:
                        named.add(tid)
                        out.write("N,%.2f,%d,%s,%d,%d,%d\n" % (t, tid, "main" if tid == pid else name, nice, policy, rtprio))
                    p = prev_threads.get(tid)
                    if not p:
                        continue
                    cpu = 100.0 * (ticks - p[1]) / hz / dt
                    wait_ms = (wait_ns - p[6]) / 1e6
                    nv = nvcsw - p[7]
                    if tid == pid:
                        gui_main = cpu
                    if cpu > top[1]:
                        top = ("main" if tid == pid else "%s/%d" % (name, tid), cpu)
                    if cpu >= 1 or wait_ms >= 10 or tid == pid:
                        out.write("H,%.2f,%d,%.0f,%d,%.0f,%d\n" % (t, tid, cpu, core, wait_ms, nv))
                prev_threads = th

        procs = proc_sample(pid or -1)
        for p_id, (name, ticks) in procs.items():
            p = prev_procs.get(p_id)
            if p and p[0] == name:
                cpu = 100.0 * (ticks - p[1]) / hz / dt
                if cpu >= 2 and p_id != os.getpid():
                    out.write("P,%.2f,%d,%s,%.0f\n" % (t, p_id, name, cpu))
        prev_procs = procs

        for line in applog.new_lines()[:20]:
            out.write("L,%.2f,%s\n" % (t, line[:200].replace("\n", " ")))

        if mono >= next_k:
            next_k = mono + 60
            for z, typ in zones:
                v = read("/sys/class/thermal/%s/temp" % z)
                if v:
                    out.write("K,%.2f,%s,%s,%s\n" % (t, z, typ, v.strip()))

        window.append((sum(pct) / max(1, len(pct)), gui_main, top))
        if mono - last_summary >= 10:
            last_summary = mono
            n = len(window)
            peak = max(window, key=lambda w: w[2][1])
            with open(now_path, "w") as f:
                f.write("%s  cores avg %.0f%% (max %.0f%%)  GUI main avg %.0f%% max %.0f%%  busiest thread %s %.0f%%  file %.1f MB\n"
                        % (time.strftime("%H:%M:%SZ", time.gmtime(t)), sum(w[0] for w in window) / n,
                           max(w[0] for w in window), sum(w[1] for w in window) / n, max(w[1] for w in window),
                           peak[2][0], peak[2][1], out.tell() / 1e6))
            window = []
        if out.tell() > a.max_mb * 1e6:
            out.write("# stopped: file over %.0f MB\n" % a.max_mb)
            break

    out.write("# cpulog end %.2f\n" % time.time())
    out.close()


if __name__ == "__main__":
    main()
