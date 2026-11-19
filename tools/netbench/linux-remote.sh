#!/bin/bash
# The remote half of linux.sh (it copies this and linux-run.py to the box and starts this under setsid):
#   linux-remote.sh <folder> <gitHash> <seconds> <serverWorkers> <clientWorkers> <port> <N:procs>...
# In <folder> (its own, per run; never shared): clones the hash, builds the compiler there (the prebuilt one in
# cone-full-master is older than the packages), compiles netbench once, then one linux-run.py per <N:procs>.
# Writes <folder>/out.txt and ends it with a line DONE.
set -u
W=$1; H=$2; SECS=$3; SW=$4; CW=$5; PORT=$6; shift 6
CAP="systemd-run --user --scope -q -p MemoryMax=1200M -p MemorySwapMax=0 nice -n 19 ionice -c3"
exec >"$W/out.txt" 2>&1
trap 'echo DONE' EXIT
git clone -q --reference "$HOME/cone-full-master" --dissociate https://github.com/jondgoodwin/cone.git "$W/cone" || exit 1
cd "$W/cone" && git checkout -q "$H" || exit 1
$CAP cmake -S . -B build/x64-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=/usr/lib/llvm-23/lib/cmake/llvm >/dev/null 2>&1
timeout 1800 $CAP cmake --build build/x64-release -j 2 >"$W/build-compiler.txt" 2>&1 || { echo "compiler build failed"; tail -n 5 "$W/build-compiler.txt"; exit 1; }
echo "compiler built $(date +%T)"
mkdir -p "$W/nb/bench" && cp packages/netmsg/examples/netbench.cone "$W/nb/bench/"
cd packages
timeout 1800 $CAP python3 ../tools/congo/congo.py run --release "$W/nb/bench/netbench.cone" -- help >"$W/nb/build.log" 2>&1 </dev/null
EXE=$(grep -o "Running .*" "$W/nb/build.log" | head -1 | cut -d' ' -f2-)
[ -x "$EXE" ] || { echo "no exe built:"; tail -n 8 "$W/nb/build.log"; exit 1; }
echo "netbench built $(date +%T): $EXE"
i=0
for spec in "$@"; do
  i=$((i + 1)); N=${spec%%:*}; P=${spec##*:}
  echo "== run $i: N=$N procs=$P"
  timeout 1200 python3 "$W/linux-run.py" "$EXE" $((PORT + i)) $N $P $SECS 1171 $SW $CW "$W/nb/run$i" </dev/null
  echo "run $i exit $?"
done
