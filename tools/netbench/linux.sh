#!/bin/bash
# netbench clients on the Linux test machine (row 522), run from Git Bash on the laptop:
#   bash tools/netbench/linux.sh <serverIPv4|local> <port> <clients> <procs> <seconds> <gitHash> [workers]
# <serverIPv4> is a server the Linux box can reach by UDP (started beforehand, e.g.
# `congo run --release netbench.cone -- server <port> <clients> <seconds> 1171 4 0.0.0.0`; it must be reachable from
# outside the laptop's network, which a home network is not without a forwarded UDP port). "local" starts the server on
# the Linux box too and the clients reach it over its loopback: it tests this script and the program there, and
# measures the box, not the laptop.
# It takes the shared linuxq lock (waits at most 30 minutes, always removes it), clones <gitHash> from GitHub (push it
# first) into ~/zz-nb714, compiles with the compiler already built in ~/cone-full-master, runs <procs> client processes
# (each its own copy of the file, for congo's build folder), samples each process's CPU from /proc between "joined" and
# "heard" lines, prints the table, and deletes ~/zz-nb714. Every remote job is capped at 30 minutes.
set -u
SRV=$1; PORT=$2; N=$3; P=$4; SECS=$5; H=$6; CW=${7:-4}
K="-i $HOME/.ssh/id_ed25519_azure_jondgoodwin -o ConnectTimeout=30 ubuntu@40.87.21.70"
LOCK="/c/src/zz-cone-module-tools/linuxq.lock"
waited=0
until mkdir "$LOCK" 2>/dev/null; do
  sleep 15; waited=$((waited + 15))
  if [ $waited -ge 1800 ]; then echo "linux lock not got in 30 minutes: Linux part not done"; exit 2; fi
done
trap 'rmdir "$LOCK"' EXIT
waited=0
until ! ssh $K 'pgrep -f "zz-linuxon[e].sh|congo.p[y]" >/dev/null'; do
  sleep 30; waited=$((waited + 30))
  if [ $waited -ge 1800 ]; then echo "server busy for 30 minutes: Linux part not done"; exit 2; fi
done
ssh $K "SRV=$SRV PORT=$PORT N=$N P=$P SECS=$SECS H=$H CW=$CW bash -s" <<'REMOTE'
set -u
CAP="systemd-run --user --scope -q -p MemoryMax=1200M -p MemorySwapMax=0 nice -n 19 ionice -c3"
W=$HOME/zz-nb714
trap 'rm -rf "$W"' EXIT
rm -rf "$W"
git clone -q --reference "$HOME/cone-full-master" --dissociate https://github.com/jondgoodwin/cone.git "$W" || exit 1
cd "$W" && git checkout -q "$H" || exit 1
# the compiler is built as zz-linuxone.sh builds it (the prebuilt one in cone-full-master is older than the packages)
$CAP cmake -S . -B build/x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-23/lib/cmake/llvm >/dev/null 2>&1
timeout 1800 $CAP cmake --build build/x64-release -j 2 >"$W/build-compiler.txt" 2>&1 || { echo "compiler build failed"; tail -n 5 "$W/build-compiler.txt"; exit 1; }
echo "compiler built $(date +%T)"
SRC=$W/packages/netmsg/examples/netbench.cone
R=$W/build/nb; mkdir -p "$R"
run() { # role, args: runs one copy under the caps, logs to $R/<role>.log
  mkdir -p "$R/$1"; cp "$SRC" "$R/$1/netbench.cone"
  (cd "$W/packages" && timeout 1800 $CAP python3 ../tools/congo/congo.py run --release "$R/$1/netbench.cone" -- "${@:2}" >"$R/$1.log" 2>&1 </dev/null) &   # not the script's stdin: a child reading it eats the rest of this script
}
pidof_exe() { pgrep -f "release/netbench.exe|release/netbench" | head -50; }
echo "load before: $(cut -d' ' -f1-3 /proc/loadavg)"
if [ "$SRV" = local ]; then
  run srv server $PORT $N $SECS 1171 4
  SRV=127.0.0.1
  until grep -q LISTENING "$R/srv.log" 2>/dev/null; do sleep 2; done
fi
left=$N
declare -A PID A B
for k in $(seq 0 $((P - 1))); do
  share=$((left / (P - k))); left=$((left - share))
  run cli$k clients $PORT $share $SECS $CW $SRV
  echo $share > "$R/cli$k.share"
  # one client's compile at a time (two compiling at once corrupted a read of core.cone, 18 Nov): wait for its exe
  for t in $(seq 1 600); do
    PID[$k]=$(pgrep -f "nb/cli$k/" | while read q; do [ -n "$(ls -l /proc/$q/exe 2>/dev/null | grep netbench)" ] && echo $q; done | head -1)
    [ -n "${PID[$k]}" ] && break; sleep 1
  done
done
tick() { awk '{print $14+$15}' /proc/$1/stat 2>/dev/null; }  # utime+stime, clock ticks
waited=0
until [ $(grep -l "CLIENTS joined" $R/cli*.log 2>/dev/null | wc -l) -ge $P ]; do
  sleep 1; waited=$((waited + 1)); [ $waited -ge 1800 ] && { echo "clients never joined"; exit 1; }
done
t0=$(date +%s.%N); for k in $(seq 0 $((P - 1))); do A[$k]=$(tick ${PID[$k]}); done
waited=0
until [ $(grep -l "CLIENTS updates heard" $R/cli*.log 2>/dev/null | wc -l) -ge $P ]; do
  sleep 1; waited=$((waited + 1)); [ $waited -ge 1800 ] && { echo "clients never finished"; exit 1; }
done
t1=$(date +%s.%N); for k in $(seq 0 $((P - 1))); do B[$k]=$(tick ${PID[$k]}); done
win=$(echo "$t1 - $t0" | bc -l); hz=$(getconf CLK_TCK)
echo "netbench clients on the Linux box: N=$N procs=$P seconds=$SECS window=${win}s (joined to heard; includes the wait for the server; box shared, CPU approximate)"
heard=0
for k in $(seq 0 $((P - 1))); do
  share=$(cat $R/cli$k.share); h=$(grep -o "heard=[0-9]*" $R/cli$k.log | head -1 | cut -d= -f2); heard=$((heard + h))
  cpu=$(echo "(${B[$k]:-0} - ${A[$k]:-0}) / $hz / $win * 100" | bc -l)
  printf "client%d conns %d  cpu %.0f%% of a core  heard %d of ~%d\n" $k $share $cpu $h $((share * SECS * 30))
done
echo "all clients: heard $heard of ~$((N * SECS * 30))"
echo "load after: $(cut -d' ' -f1-3 /proc/loadavg)"
grep -h "^SERVER\|^IOTIMES" "$R/srv.log" 2>/dev/null
wait
REMOTE
echo "linux part ended: lock released, ~/zz-nb714 removed"
