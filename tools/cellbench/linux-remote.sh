#!/bin/bash
# The remote half of linux.sh (it copies this and linux-run.py to the box and starts this under setsid):
#   linux-remote.sh <folder> <gitHash> <seconds> <serverWorkers> <clientWorkers> <port> <spec>...
# a spec is form:players:procs[:crowd[:zones[:beings[:inputhz]]]], or inproc:<form> for case A's in-process run. In <folder>
# (its own, per run; never shared): clones the hash, builds the compiler there, compiles cellbench once, then one linux-run.py
# per spec. Writes <folder>/out.txt and ends it with a line DONE.
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
mkdir -p "$W/cb/bench" && cp packages/netmsg/examples/cellbench.cone "$W/cb/bench/"
cd packages
timeout 1800 $CAP python3 ../tools/congo/congo.py run --release "$W/cb/bench/cellbench.cone" -- help >"$W/cb/build.log" 2>&1 </dev/null
EXE=$(grep -o "Running .*" "$W/cb/build.log" | head -1 | cut -d' ' -f2-)
[ -x "$EXE" ] || { echo "no exe built:"; tail -n 8 "$W/cb/build.log"; exit 1; }
echo "cellbench built $(date +%T): $EXE"
i=0
for spec in "$@"; do
  i=$((i + 1))
  if [ "${spec%%:*}" = inproc ]; then
    F=${spec##*:}
    echo "== run $i: inproc form=$F"
    timeout 300 $CAP "$EXE" inproc form=$F frames=3000 </dev/null
    continue
  fi
  IFS=: read -r F N P CR Z B HZ <<<"$spec"
  echo "== run $i: $spec  (load $(cut -d' ' -f1-3 /proc/loadavg))"
  timeout 1500 python3 "$W/linux-run.py" "$EXE" $((PORT + i)) "$F" "$N" "$P" "$SECS" "$SW" "$CW" "${B:-8}" "${CR:-0}" "${Z:-1}" "${HZ:-30}" "$W/cb/run$i" </dev/null
  echo "run $i exit $?"
done
