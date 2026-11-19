#!/bin/bash
# netbench on the Linux test machine (row 496), server and clients both on the box over its loopback. From Git Bash:
#   bash tools/netbench/linux.sh <gitHash> <seconds> <serverWorkers> <clientWorkers> <N:procs> [<N:procs> ...]
#   e.g. bash tools/netbench/linux.sh 47bbde065... 10 4 4 1000:2 3000:3
# (a Linux client against the laptop's server is not possible from a home network: no UDP forward.)
# Push the hash first: the box clones it from GitHub into its own folder ~/zz-nb717-<time> (one per run, never
# shared), builds the compiler there (about 10 minutes) and netbench once, and runs each <N:procs> with
# linux-run.py: the server and <procs> client processes, per-thread CPU from /proc for each, one table.
# The remote part runs under setsid and is killed by its pid at the end (killing the local ssh does not kill it).
# Takes the shared linuxq lock (mkdir; waits at most 30 minutes, always removes it), and waits for any
# zz-linuxone.sh / congo.py job on the box to end (30 minutes at most). The whole run is capped at 90 minutes.
set -u
H=$1; SECS=$2; SW=$3; CW=$4; shift 4
HERE="$(cd "$(dirname "$0")" && pwd)"
KEY="$HOME/.ssh/id_ed25519_azure_jondgoodwin"
K="-i $KEY -o ConnectTimeout=30 ubuntu@40.87.21.70"
LOCK="/c/src/zz-cone-module-tools/linuxq.lock"
W=zz-nb717-$(date +%m%d-%H%M%S)
RPID=
cleanup() {
  if [ -n "$RPID" ]; then ssh $K "kill -9 -- -$RPID 2>/dev/null; rm -rf ~/$W"; fi
  rmdir "$LOCK" 2>/dev/null
}
waited=0
until mkdir "$LOCK" 2>/dev/null; do
  sleep 15; waited=$((waited + 15))
  if [ $waited -ge 1800 ]; then echo "linux lock not got in 30 minutes: Linux part not done"; exit 2; fi
done
trap cleanup EXIT
waited=0
until ! ssh $K 'pgrep -f "zz-linuxon[e].sh|congo.p[y]|zz-716-ru[n]" >/dev/null'; do
  sleep 30; waited=$((waited + 30))
  if [ $waited -ge 1800 ]; then echo "server busy for 30 minutes: Linux part not done"; exit 2; fi
done
ssh $K "mkdir -p ~/$W" || exit 1
scp -q -i "$KEY" "$HERE/linux-remote.sh" "$HERE/linux-run.py" ubuntu@40.87.21.70:$W/ || exit 1
PORT=$((47400 + RANDOM % 500 * 10))
RPID=$(ssh $K "cd ~/$W && setsid nohup bash linux-remote.sh \$HOME/$W $H $SECS $SW $CW $PORT $* </dev/null >/dev/null 2>&1 & echo \$!")
echo "remote shell pid $RPID in ~/$W, started $(date +%T)"
waited=0
until ssh $K "grep -q '^DONE' ~/$W/out.txt 2>/dev/null"; do
  sleep 45; waited=$((waited + 45))
  if [ $waited -ge 5400 ]; then echo "not finished in 90 minutes: killed"; break; fi
done
ssh $K "cat ~/$W/out.txt"
