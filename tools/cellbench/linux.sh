#!/bin/bash
# cellbench on the Linux test machine (row 528), server and clients both on the box over its loopback. From Git Bash:
#   bash tools/cellbench/linux.sh <gitHash> <seconds> <serverWorkers> <clientWorkers> <spec> [<spec> ...]
#   spec = form:players:procs[:crowd[:zones[:beings[:inputhz]]]] (e.g. c:1000:2  m1:1000:2:1  m1:8:1:0:1:28:60), or inproc:m1 / inproc:c
# Push the hash first: the box clones it from GitHub into its own folder ~/zz-cb819-<time>, builds the compiler there
# (about 10 minutes), cellbench once, and runs each spec with linux-run.py. Takes linuxq's lock through linuxlock.sh (waits at most
# 30 minutes, renews the lease while it waits for the run, always releases it), and waits for any zz-linuxone.sh / congo.py job to end.
set -u
H=$1; SECS=$2; SW=$3; CW=$4; shift 4
HERE="$(cd "$(dirname "$0")" && pwd)"
KEY="$HOME/.ssh/id_ed25519_azure_jondgoodwin"
K="-i $KEY -o ConnectTimeout=30 ubuntu@40.87.21.70"
LL=/c/src/zz-cone-module-tools/linuxlock.sh
W=zz-cb819-$(date +%m%d-%H%M%S)
RPID=
T=$(bash $LL take "lane 819 cellbench" 45 240) || { echo "linux lock not taken in 240 minutes: Linux part not done"; exit 2; }
cleanup() {
  if [ -n "$RPID" ]; then ssh $K "kill -9 -- -$RPID 2>/dev/null; rm -rf ~/$W"; fi
  bash $LL release "$T"
}
trap cleanup EXIT
waited=0
until ! ssh $K 'pgrep -f "zz-linuxon[e].sh|congo.p[y]|zz-716-ru[n]" >/dev/null'; do
  sleep 30; waited=$((waited + 30)); bash $LL renew "$T" >/dev/null
  if [ $waited -ge 1800 ]; then echo "server busy for 30 minutes: Linux part not done"; exit 2; fi
done
ssh $K "mkdir -p ~/$W" || exit 1
scp -q -i "$KEY" "$HERE/linux-remote.sh" "$HERE/linux-run.py" ubuntu@40.87.21.70:$W/ || exit 1
PORT=$((47400 + RANDOM % 500 * 10))
RPID=$(ssh $K "cd ~/$W && setsid nohup bash linux-remote.sh \$HOME/$W $H $SECS $SW $CW $PORT $* </dev/null >/dev/null 2>&1 & echo \$!")
echo "remote shell pid $RPID in ~/$W, started $(date +%T)"
waited=0
until ssh $K "grep -q '^DONE' ~/$W/out.txt 2>/dev/null"; do
  sleep 45; waited=$((waited + 45)); bash $LL renew "$T" >/dev/null
  if [ $waited -ge 5400 ]; then echo "not finished in 90 minutes: killed"; break; fi
done
ssh $K "cat ~/$W/out.txt; echo; echo server load after: \$(cut -d' ' -f1-3 /proc/loadavg)"
