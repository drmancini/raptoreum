#!/bin/bash
# Build corpus v4: 3M fanout UTXOs -> 1.5M independent (depth-1) transactions.
# Sized for runs at RTM's real 2-minute cadence: 125k tx/node, ~8 block intervals.
#
# Two constraints learned the hard way:
#  - fanout.py authenticates with the datadir cookie, and setting rpcuser/
#    rpcpassword in the conf stops the node writing one. Leave them out.
#  - regtest RPC/P2P default to 19898/19899, which bowser's swarm node holds.
#    Stop it for the build and bring it back afterwards.
set -e
R=/data/forge/projects/raptoreum
BIN=/data/rtm-2204/src
D=/data/rtm-perf/datadirs/fanout4
C=/data/rtm-perf/corpus/v4
PY=/home/mike/.pyenv/versions/3.11.9/bin/python
CLI="$BIN/raptoreum-cli -regtest -datadir=$D"
SWARM=/data/rtmswarm
export XDG_RUNTIME_DIR=/run/user/$(id -u)

echo "### $(date -Is) stopping bowser's swarm node (ports 19898/19899)"
$BIN/raptoreum-cli -regtest -datadir=$SWARM/data -rpcport=19898 -rpcuser=swarm \
  -rpcpassword="$(cat $SWARM/rpcpass 2>/dev/null)" stop >/dev/null 2>&1 || true
pkill -u "$(id -u)" -f "raptoreumd.*rtmswarm" >/dev/null 2>&1 || true
sleep 8

systemctl --user stop rtm-fanout4-node 2>/dev/null || true
sleep 2
rm -rf $D $C; mkdir -p $D $C
cat > $D/raptoreum.conf <<CONF
regtest=1
server=1
discover=0
dnsseed=0
checkmempool=0
[regtest]
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
port=19899
rpcport=19898
maxtipage=999999999
maxmempool=8000
CONF
systemctl --user reset-failed rtm-fanout4-node 2>/dev/null || true
systemd-run --user --unit=rtm-fanout4-node $BIN/raptoreumd -regtest -datadir=$D -printtoconsole=0 >/dev/null 2>&1
for i in $(seq 90); do $CLI getblockcount >/dev/null 2>&1 && break; sleep 1; done
echo "  node up at height $($CLI getblockcount)"
$CLI createwallet perf >/dev/null

echo "### $(date -Is) PHASE 1: fan-out to 3,000,000 UTXOs"
time $PY $R/test/perf/fanout.py --datadir $D --out $C --utxos 3000000 --per-tx 1000

echo "### $(date -Is) SNAPSHOT"
$CLI stop; sleep 8
tar -C $D -cf $C/datadir-fanout.tar regtest
ls -la $C/datadir-fanout.tar | awk '{printf "  chain snapshot: %.1f MB\n", $5/1e6}'
systemd-run --user --unit=rtm-fanout4-node $BIN/raptoreumd -regtest -datadir=$D -printtoconsole=0 >/dev/null 2>&1
for i in $(seq 90); do $CLI getblockcount >/dev/null 2>&1 && break; sleep 1; done

echo "### $(date -Is) PHASE 2: 1,500,000 transactions, depth 1, variable fees"
time $PY $R/test/perf/build_corpus.py --corpus $C --count 1500000 --depth 1 \
     --fee-min 1000 --fee-max 20000 --workers 8

echo "### $(date -Is) PHASE 3: split into 12 shards"
$PY /data/rtm-perf/split_corpus.py --corpus $C --out $C-shards --shards 12

echo "### $(date -Is) stopping fanout node, restoring the swarm node"
$CLI stop >/dev/null 2>&1 || true; sleep 8
$SWARM/bin/raptoreumd -regtest -datadir=$SWARM/data -conf=$SWARM/data/raptoreum.conf -daemon
echo "### $(date -Is) DONE"
du -sh $C $C-shards
