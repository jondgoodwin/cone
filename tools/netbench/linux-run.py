#!/usr/bin/env python3
"""One netbench run on the Linux box (row 496): the server and <procs> client processes, loopback, one table.
   linux-run.py <exe> <port> <clients> <procs> <seconds> <bytes> <serverWorkers> <clientWorkers> <outdir>
Every process it starts is in its own session and is killed by pid when the run ends or fails.
<exe> is a netbench built by congo (the clients run the same file). Per-thread CPU is read from
/proc/<pid>/task/*/stat (utime+stime) at the server's RUNSTART and RUNEND lines, as run.ps1 does on Windows."""
import os, re, signal, subprocess, sys, time

exe, port, n, procs, secs, nbytes, sw, cw, out = sys.argv[1:10]
n, procs, secs = int(n), int(procs), int(secs)
os.makedirs(out, exist_ok=True)
HZ = os.sysconf("SC_CLK_TCK")
CAP = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=1200M", "-p", "MemorySwapMax=0",
       "nice", "-n", "19", "ionice", "-c3"]
started = []  # Popen objects, killed by pid at the end


def launch(role, args):
    log = open(f"{out}/{role}.log", "wb")
    p = subprocess.Popen(CAP + [exe] + args, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    started.append(p)
    return p


def exe_pid(p):
    """the netbench process: the wrapper chain (systemd-run, nice, ionice) execs into the exe, keeping the pid."""
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
            fld = f[f.rindex(")") + 2:].split()  # after "(comm) ": state is fld[0], utime fld[11], stime fld[12]
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
            os.killpg(p.pid, signal.SIGKILL)  # its own session: pid == pgid
        except OSError:
            pass


try:
    load0 = open("/proc/loadavg").read().split()[:3]
    srv = launch("srv", ["server", port, str(n), str(secs), nbytes, sw, "127.0.0.1"])
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
        cli.append((k, share, launch(f"cli{k}", ["clients", port, str(share), str(secs), cw, "127.0.0.1"])))
    pids = {"server": exe_pid(srv)}
    for k, share, p in cli:
        pids[f"client{k}"] = exe_pid(p)
    a = b = None
    ta = tb = 0.0
    deadline = time.time() + 240 + 4 * secs
    while time.time() < deadline and srv.poll() is None:
        t = text(f"{out}/srv.log")
        if a is None and "RUNSTART" in t:
            a = {k: snap(v) for k, v in pids.items() if v}
            ta = time.time()
        if a is not None and b is None and "RUNEND" in t:
            b = {k: snap(v) for k, v in pids.items() if v}
            tb = time.time()
        time.sleep(0.05)
    for p in [srv] + [c[2] for c in cli]:
        try:
            p.wait(timeout=90)
        except subprocess.TimeoutExpired:
            print("process pid", p.pid, "did not end in 90 s (killed)")
    load1 = open("/proc/loadavg").read().split()[:3]
    win = tb - ta
    print(f"netbench linux N={n} procs={procs} serverWorkers={sw} clientWorkers={cw} seconds={secs} bytes={nbytes} "
          f"window={win*1000:.0f}ms load before={' '.join(load0)} after={' '.join(load1)} (2 cores)")

    def cpu(name, extra=""):
        if a is None or b is None or name not in a or name not in b or win <= 0:
            return f"{name:<8} no sample"
        d = [(b[name][t] - a[name].get(t, 0)) / HZ for t in b[name]]
        return f"{name:<8} total {100*sum(d)/win:5.0f}% of a core  busiest thread {100*max(d)/win:4.0f}%  {extra}"

    print(cpu("server"))
    sent = heard = 0
    for k, share, p in cli:
        m = re.search(r"CLIENTS updates heard=(\d+)", text(f"{out}/cli{k}.log"))
        h = int(m.group(1)) if m else 0
        e = share * secs * 30
        sent += e
        heard += h
        print(cpu(f"client{k}", f"conns {share}  heard {h} of ~{e} ({100.0*h/max(e,1):.1f}%)"))
    print(f"all clients: heard {heard} of ~{sent} ({100.0*heard/max(sent,1):.1f}%)")
    for line in text(f"{out}/srv.log").splitlines():
        if line.startswith(("SERVER", "JOINED", "IOTIMES")):
            print(line)
finally:
    killall()
