#!/bin/bash
# Validation cost vs input count, on the fanout chain (3M spendable UTXOs).
# Runs on ports 20898/20899 so it never collides with the swarm node.
set -uo pipefail
BIN=/data/rtm-2204/src
D=/data/rtm-perf/datadirs/fanout4
export XDG_RUNTIME_DIR=/run/user/$(id -u)
CLI="$BIN/raptoreum-cli -regtest -datadir=$D -rpcport=20898"

systemctl --user stop rtm-bench-node 2>/dev/null || true; sleep 3
cat > $D/raptoreum.conf <<CONF
regtest=1
server=1
discover=0
dnsseed=0
listen=0
checkmempool=0
# Lift standardness so the sweep can pass 100kB and show where the cap bites.
# MAX_STANDARD_TX_SIGOPS (4000) is NOT gated on this and still applies.
acceptnonstdtxn=1
maxmempool=8000
[regtest]
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
port=20899
rpcport=20898
maxtipage=999999999
CONF
systemctl --user reset-failed rtm-bench-node 2>/dev/null || true
systemd-run --user --unit=rtm-bench-node $BIN/raptoreumd -regtest -datadir=$D -printtoconsole=0 >/dev/null 2>&1
for i in $(seq 90); do $CLI getblockcount >/dev/null 2>&1 && break; sleep 1; done
echo "### node up at height $($CLI getblockcount), acceptnonstdtxn=1"
echo "### $(date -Is) sweeping input counts"
/home/mike/.pyenv/versions/3.11.9/bin/python /data/rtm-perf/bench_inputs.py \
  --utxos /data/rtm-perf/corpus/v4/utxos.json --port 20898 \
  --cookie $D/regtest/.cookie \
  --counts 1,2,5,10,25,50,100,200,400,800,1600,3200 --reps 3
echo "### $(date -Is) done"
$CLI stop >/dev/null 2>&1 || true
