#!/bin/bash
# Build corpus v4: 3M fanout UTXOs -> 1.5M independent (depth-1) transactions.
# Sized for runs at RTM's real 2-minute cadence: 125k tx per node, ~1000s of
# load at 1500 tx/s, about 8 block intervals.
set -e
R=/data/forge/projects/raptoreum
BIN=/data/rtm-2204/src            # same build the swarm runs
D=/data/rtm-perf/datadirs/fanout4
C=/data/rtm-perf/corpus/v4
PY=/home/mike/.pyenv/versions/3.11.9/bin/python
CLI="$BIN/raptoreum-cli -datadir=$D"
export XDG_RUNTIME_DIR=/run/user/$(id -u)

systemctl --user stop rtm-fanout4-node 2>/dev/null || true
sleep 2
rm -rf $D $C; mkdir -p $D $C
cat > $D/raptoreum.conf <<CONF
regtest=1
[regtest]
rpcuser=perf
rpcpassword=perf
CONF
systemctl --user reset-failed rtm-fanout4-node 2>/dev/null || true
systemd-run --user --unit=rtm-fanout4-node $BIN/raptoreumd -regtest -datadir=$D -printtoconsole=0 >/dev/null 2>&1
for i in $(seq 90); do $CLI -regtest getblockcount >/dev/null 2>&1 && break; sleep 1; done
$CLI -regtest createwallet perf >/dev/null

echo "### $(date -Is) PHASE 1: fan-out to 3,000,000 UTXOs"
time $PY $R/test/perf/fanout.py --datadir $D --out $C --utxos 3000000 --per-tx 1000

echo "### $(date -Is) SNAPSHOT"
$CLI -regtest stop; sleep 6
tar -C $D -cf $C/datadir-fanout.tar regtest
ls -la $C/datadir-fanout.tar | awk '{printf "  chain snapshot: %.1f MB\n", $5/1e6}'
systemd-run --user --unit=rtm-fanout4-node $BIN/raptoreumd -regtest -datadir=$D -printtoconsole=0 >/dev/null 2>&1
for i in $(seq 90); do $CLI -regtest getblockcount >/dev/null 2>&1 && break; sleep 1; done
echo "  node back at height $($CLI -regtest getblockcount)"

echo "### $(date -Is) PHASE 2: 1,500,000 transactions, depth 1, variable fees"
time $PY $R/test/perf/build_corpus.py --corpus $C --count 1500000 --depth 1 \
     --fee-min 1000 --fee-max 20000 --workers 8

echo "### $(date -Is) PHASE 3: split into 12 shards"
$PY /data/rtm-perf/split_corpus.py --corpus $C --out $C-shards --shards 12

echo "### $(date -Is) DONE"
du -sh $C $C-shards
