#!/usr/bin/env python3
"""measure.py: run a command and report what row 550 step 2 and 3 ask of it on Linux.

    python3 measure.py [--every 0.005] -- cmd args...

Prints one line: wall seconds, the child's peak resident set (getrusage), its resident set at the last sample before it ended, the
peak line count of /proc/<pid>/maps (the mapping count against vm.max_map_count, usually 65,530; a decommit by madvise plus PROT_NONE
makes a mapping of each alternation of committed and decommitted pages), and the peak VmRSS and last VmRSS read from
/proc/<pid>/status. The command's own output goes through unchanged.
"""
import os, resource, subprocess, sys, time

def main():
    argv = sys.argv[1:]
    every = 0.005
    if argv and argv[0] == "--every":
        every = float(argv[1]); argv = argv[2:]
    if argv and argv[0] == "--":
        argv = argv[1:]
    t0 = time.time()
    p = subprocess.Popen(argv)
    peak_maps = 0; peak_rss = 0; last_rss = 0; samples = 0
    while p.poll() is None:
        try:
            with open(f"/proc/{p.pid}/maps", "rb") as f:
                n = f.read().count(b"\n")
            if n > peak_maps: peak_maps = n
            with open(f"/proc/{p.pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        last_rss = int(line.split()[1])
                        peak_rss = max(peak_rss, last_rss)
            samples += 1
        except OSError:
            pass
        time.sleep(every)
    wall = time.time() - t0
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    print(f"measure: rc={p.returncode} wall={wall:.2f}s maxrss={ru.ru_maxrss / 1024:.1f}MB sampled_peak_rss={peak_rss / 1024:.1f}MB "
          f"last_rss={last_rss / 1024:.1f}MB peak_maps_lines={peak_maps} samples={samples}", flush=True)
    return p.returncode

if __name__ == "__main__":
    sys.exit(main())
