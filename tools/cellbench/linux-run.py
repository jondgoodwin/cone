#!/usr/bin/env python3
"""One cellbench run on the Linux box (row 528): the server and <procs> client processes, loopback, one table.
   linux-run.py <exe> <port> <form> <players> <procs> <secs> <serverWorkers> <clientWorkers> <beings> <crowd> <zones> <inputhz> <outdir>
Every process it starts is in its own session and is killed by pid when the run ends or fails. Per-thread CPU is read from
/proc/<pid>/task/*/stat (utime+stime) at the server's RUNSTART and RUNEND lines, as tools/cellbench/run.ps1 does on Windows."""
import os, re, signal, subprocess, sys, time

exe, port, form, n, procs, secs, sw, cw, beings, crowd, zones, hz, out = sys.argv[1:14]
n, procs, secs = int(n), int(procs), int(secs)
os.makedirs(out, exist_ok=True)
HZ = os.sysconf("SC_CLK_TCK")
CAP = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=1200M", "-p", "MemorySwapMax=0",
       "nice", "-n", "19", "ionice", "-c3"]
started = []


def launch(role, args):
    log = open(f"{out}/{role}.log", "wb")
    p = subprocess.Popen(CAP + [exe] + args, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    started.append(p)
    return p


def exe_pid(p):
    for _ in range(300):
        if p.poll() is not None:
            return None
        try:
            if os.path.realpath(f"/proc/{p.pid}/exe") == os.path.realpath(exe):
                return p.pid
        except OSError:
            pass
        time.sleep(0.1)
    return None


def snap(pid):
    r = {}
    try:
        for t in os.listdir(f"/proc/{pid}/task"):
            try:
                f = open(f"/proc/{pid}/task/{t}/stat").read()
            except OSError:
                continue
            fld = f[f.rindex(")") + 2:].split()
            r[t] = int(fld[11]) + int(fld[12])
    except OSError:
        pass
    return r


def text(f):
    try:
        return open(f, errors="replace").read()
    except OSError:
        return ""


def killall():
    for p in started:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass


def cpustat():
    f = open("/proc/stat").readline().split()[1:]
    v = [int(x) for x in f]
    return sum(v), v[3] + v[4]   # total, idle+iowait


common = [f"form={form}", f"secs={secs}", f"beings={beings}", f"crowd={crowd}", f"players={n}"]
try:
    load0 = open("/proc/loadavg").read().split()[:3]
    srv = launch("srv", ["server", port, "127.0.0.1"] + common + [f"workers={sw}", f"zones={zones}"])
    for _ in range(600):
        if "LISTENING" in text(f"{out}/srv.log") or srv.poll() is not None:
            break
        time.sleep(0.2)
    if "LISTENING" not in text(f"{out}/srv.log"):
        print("server did not listen:\n" + text(f"{out}/srv.log")[-500:])
        sys.exit(1)
    cli, left = [], n
    for k in range(procs):
        share = left // (procs - k)
        left -= share
        cli.append((k, share, launch(f"cli{k}", ["clients", port, "127.0.0.1"] + common + [f"n={share}", f"workers={cw}", f"inputhz={hz}"])))
    pids = {"server": exe_pid(srv)}
    for k, share, p in cli:
        pids[f"client{k}"] = exe_pid(p)
    a = b = None
    ta = tb = 0.0
    ca = cb = None
    deadline = time.time() + 900 + 4 * secs
    while time.time() < deadline and srv.poll() is None:
        t = text(f"{out}/srv.log")
        if a is None and "RUNSTART" in t:
            a = {k: snap(v) for k, v in pids.items() if v}
            ta = time.time()
            ca = cpustat()
        if a is not None and b is None and "RUNEND" in t:
            b = {k: snap(v) for k, v in pids.items() if v}
            tb = time.time()
            cb = cpustat()
        time.sleep(0.05)
    for p in [srv] + [c[2] for c in cli]:
        try:
            p.wait(timeout=120)
        except subprocess.TimeoutExpired:
            print("process pid", p.pid, "did not end in 120 s (killed)")
    load1 = open("/proc/loadavg").read().split()[:3]
    win = tb - ta
    busy = 100.0 * (1 - (cb[1] - ca[1]) / max(cb[0] - ca[0], 1)) if ca and cb else 0.0
    print(f"cellbench linux form={form} N={n} zones={zones} beings={beings} crowd={crowd} procs={procs} serverWorkers={sw} clientWorkers={cw} "
          f"seconds={secs} inputHz={hz} window={win*1000:.0f}ms")
    print(f"machine load: loadavg before={' '.join(load0)} after={' '.join(load1)}; busy during the window={busy:.1f}% (2 vCPU)")

    def cpu(name, extra=""):
        if a is None or b is None or name not in a or name not in b or win <= 0:
            return f"{name:<8} no sample"
        d = [(b[name][t] - a[name].get(t, 0)) / HZ for t in b[name]]
        return f"{name:<8} total {100*sum(d)/win:5.0f}% of a core  busiest thread {100*max(d)/win:4.0f}%  {extra}"

    print(cpu("server"))
    heard = exp = 0
    hist = {}
    for k, share, p in cli:
        t = text(f"{out}/cli{k}.log")
        m = re.search(r"CLIENTS own-region updates heard=(\d+) expected=(\d+)", t)
        h = int(m.group(1)) if m else 0
        e = int(m.group(2)) if m else 0
        heard += h
        exp += e
        print(cpu(f"client{k}", f"conns {share}  own-region heard {h} of {e} ({100.0*h/max(e,1):.1f}%)"))
        for line in t.splitlines():
            if line.startswith("HIST"):
                for kk, vv in re.findall(r"(\d+):(\d+)", line):
                    hist[int(kk)] = hist.get(int(kk), 0) + int(vv)
    print(f"all clients: own-region updates heard {heard} of {exp} ({100.0*heard/max(exp,1):.1f}%)")
    tot = sum(hist.values())

    def pct(q):
        want = -(-tot * q // 100)
        run = 0
        for k in sorted(hist):
            run += hist[k]
            if run >= want:
                return (k + 1) * 100
        return 0

    print(f"AGE own-region state age, all clients: n={tot} p50_us={pct(50)} p99_us={pct(99)} max_us={pct(100)}")
    for line in text(f"{out}/srv.log").splitlines():
        if line.startswith(("WORLD", "JOINED", "IOTIMES", "ACTORSTATS")):
            print(line)
finally:
    killall()
