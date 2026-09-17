#!/bin/bash
# Phase 1: close the loop -- mining, compact-block reconstruction and connect
# cost under real WAN latency, with load offered simultaneously by every node.
#
#   run_phase1.sh <tag> <total_tx_per_s> <block_interval_s> <duration_s>
#
# Rate is the NETWORK total; each node offers total/12 from its own disjoint
# corpus shard, so transactions originate everywhere rather than at one point.
set -uo pipefail
SW="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PW=$(cat "$SW/conf/.rpcpassword")
TAG=${1:?tag}; TOTAL=${2:?total tx/s}; INTERVAL=${3:?block interval s}; DUR=${4:?duration s}
MINER=${MINER:-eur}
OUT="$SW/runs/$TAG"; mkdir -p "$OUT"
# Each node's shard is consumed in order across runs. Without a cursor a second
# run re-offers transactions the first already spent, and every one is rejected
# as already-in-chain -- a run that looks like it executed but measured nothing.
CURSOR="$SW/runs/.cursor"
SKIP=$(cat "$CURSOR" 2>/dev/null || echo 0)
ALIASES=$(awk '!/^#/&&NF{print $1}' "$SW/hosts.tsv")
N=$(echo "$ALIASES" | wc -l)
SUBS=$N; [ "${SKIP_MINER_LOAD:-0}" = "1" ] && SUBS=$((N-1))
PER=$(python3 -c "print(round($TOTAL/$SUBS,2))")

sshq() { timeout "${2:-60}" ssh -o BatchMode=yes -o ConnectTimeout=10 "$1" "${@:3}"; }
tgt()  { awk -v a="$1" '!/^#/&&$1==a{print $2}' "$SW/hosts.tsv"; }
bas()  { awk -v a="$1" '!/^#/&&$1==a{print $6}' "$SW/hosts.tsv"; }
cli()  { local a=$1; shift; sshq "$(tgt $a)" 60 "$(bas $a)/bin/raptoreum-cli -regtest -datadir=$(bas $a)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW $*"; }

echo "=== phase1 [$TAG] total=${TOTAL}tx/s per-node=${PER}tx/s block=${INTERVAL}s dur=${DUR}s miner=$MINER skip=$SKIP ==="

echo "--- clock offsets before ---"
for a in $ALIASES; do
  ( o=$(sshq "$(tgt $a)" 40 'python3 -' < "$SW/sntp.py" 2>/dev/null | grep -oE '^\s*[+-][0-9.]+' | tr -d ' ')
    echo "$a $(tgt $a) ${o:-0}" ) &
done > "$OUT/offsets-before.txt"; wait
sed 's/^/  /' "$OUT/offsets-before.txt"

# A node whose tip is older than nMaxTipAge stays in initial block download, and
# in IBD it ignores transaction announcements (net_processing.cpp:3055) while
# still accepting everything offered locally over RPC. The result is a node that
# reports 100% acceptance while holding only its own transactions -- a run that
# executes perfectly and measures nothing. Refuse to start unless every node is
# out of IBD and on the same height.
echo "--- preflight: mesh health ---"
PRE_FAIL=0; PRE_H=""
for a in $ALIASES; do
  info=$(cli "$a" getblockchaininfo | tr -d ' "' | awk -F: '/^blocks/{b=$2} /^initialblockdownload/{i=$2} END{print b"|"i}' | tr -d ',')
  h=${info%%|*}; ibd=${info##*|}
  [ -z "$PRE_H" ] && PRE_H=$h
  if [ "$ibd" != "false" ] || [ "$h" != "$PRE_H" ]; then
    echo "  !! $a height=$h ibd=$ibd (expected height=$PRE_H ibd=false)"; PRE_FAIL=1
  else
    echo "  ok $a height=$h"
  fi
done
if [ "$PRE_FAIL" = "1" ]; then
  echo "ABORT: mesh is not healthy. Bootstrap the offending nodes first."
  exit 1
fi

echo "--- marking log positions ---"
for a in $ALIASES; do
  ( echo "$a $(sshq "$(tgt $a)" 40 "wc -l < $(bas $a)/data/regtest/debug.log" 2>/dev/null | tr -d ' ')" ) &
done > "$OUT/logmark.txt"; wait
START_H=$(cli "$MINER" getblockcount | tr -d '\r')
echo "  start height: $START_H"

echo "--- launching submitters on all $N nodes ---"
# SKIP_MINER_LOAD=1 excludes the miner from offering load. The miner is
# otherwise mining transactions it submitted milliseconds earlier, which no
# peer can possibly have yet -- that manufactures reconstruction round trips
# and is indistinguishable from genuine propagation lag.
T0=$(date +%s.%N)
for a in $ALIASES; do
  if [ "${SKIP_MINER_LOAD:-0}" = "1" ] && [ "$a" = "$MINER" ]; then
    echo "  (miner $a offers no load)"; continue
  fi
  ( sshq "$(tgt $a)" $((DUR+180)) \
      "cd $(bas $a) && python3 submit.py --shard corpus/shard.bin --password '$PW' \
         --rate $PER --duration $DUR --threads 4 --skip $SKIP --log $(bas $a)/logs/$TAG.log" \
      > "$OUT/submit-$a.txt" 2>&1 ) &
done

echo "--- mining every ${INTERVAL}s on $MINER for ${DUR}s ---"
ADDR=$(cli "$MINER" getnewaddress | tr -d '\r')
# Fixed cadence: sleeping INTERVAL *after* each generatetoaddress makes the real
# interval INTERVAL + template-construction time, which at a large mempool is not
# small (getblocktemplate runs a full ConnectBlock). Schedule against a fixed
# start instead, and log each block's wall time so the actual cadence is checkable.
( sshq "$(tgt $MINER)" $((DUR+180)) \
    "start=\$(date +%s); i=0; end=\$((start+$DUR)); \
     while [ \$(date +%s) -lt \$end ]; do \
       echo \"tick \$i \$(date +%s.%N)\"; \
       $(bas $MINER)/bin/raptoreum-cli -regtest -datadir=$(bas $MINER)/data -rpcport=19898 \
         -rpcuser=swarm -rpcpassword=$PW generatetoaddress 1 $ADDR >/dev/null 2>&1; \
       i=\$((i+1)); t=\$((start + i*$INTERVAL)); now=\$(date +%s); \
       [ \$t -gt \$end ] && t=\$end; \
       [ \$t -gt \$now ] && sleep \$((t-now)); \
     done" ) > "$OUT/mine.txt" 2>&1 &
wait
T1=$(date +%s.%N)
echo "  wall: $(python3 -c "print('%.1f s' % ($T1-$T0))")"

# advance the cursor past everything this run offered
OFFERED=$(grep -h "^offered" "$OUT"/submit-*.txt 2>/dev/null | awk '{print $2}' | sort -rn | head -1)
echo $(( SKIP + ${OFFERED:-0} )) > "$CURSOR"
echo "  cursor: $SKIP -> $(cat "$CURSOR")"
ACC=$(grep -h "^offered" "$OUT"/submit-*.txt 2>/dev/null | awk -F'accepted ' '{print $2}' | awk '{s+=$1} END{print s+0}')
OFF=$(grep -h "^offered" "$OUT"/submit-*.txt 2>/dev/null | awk '{s+=$2} END{print s+0}')
echo "  accepted $ACC of $OFF offered ($(python3 -c "print('%.1f%%' % (100.0*$ACC/max($OFF,1)))"))"
[ "$OFF" -gt 0 ] && [ $((100*ACC/OFF)) -lt 90 ] && echo "  !! LOW ACCEPTANCE -- this run is not measuring what you think"
echo "--- settling 30s ---"; sleep 30
echo "--- final state ---"
for a in $ALIASES; do
  ( h=$(cli $a getblockcount | tr -d '\r')
    m=$(cli $a getmempoolinfo | tr -d ' ",' | awk -F: '/^size/{s=$2} /^usage/{u=$2} END{print s" "u}')
    echo "$a $h $m" ) &
done > "$OUT/final.txt"; wait
sort "$OUT/final.txt" | awk '{printf "  %-4s height=%-6s mempool=%-8s %.0f MB\n",$1,$2,$3,$4/1048576}'

echo "--- clock offsets after ---"
for a in $ALIASES; do
  ( o=$(sshq "$(tgt $a)" 40 'python3 -' < "$SW/sntp.py" 2>/dev/null | grep -oE '^\s*[+-][0-9.]+' | tr -d ' ')
    echo "$a $(tgt $a) ${o:-0}" ) &
done > "$OUT/offsets-after.txt"; wait

echo "--- pulling log tails ---"
for a in $ALIASES; do
  ( mk=$(awk -v a="$a" '$1==a{print $2}' "$OUT/logmark.txt"); mk=${mk:-0}
    sshq "$(tgt $a)" 120 "tail -n +$((mk+1)) $(bas $a)/data/regtest/debug.log" > "$OUT/debug-$a.log" 2>/dev/null
    sshq "$(tgt $a)" 120 "cat $(bas $a)/logs/$TAG.log" > "$OUT/sent-$a.log" 2>/dev/null ) &
done; wait
echo "  collected: $(ls -la $OUT/debug-*.log 2>/dev/null | awk '{s+=$5} END{printf "%.1f MB", s/1e6}')"
echo "=== done: $OUT ==="
