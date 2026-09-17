#!/bin/bash
# Does the relay ceiling move when the trickle interval changes?
#
# Condition A: shipped INVENTORY_BROADCAST_INTERVAL (5s)
# Condition B: -perfinvinterval=1
#
# S is read directly from the miner's mempool growth rate: it offers no load, so
# everything in its mempool arrived over relay. No per-transaction logging, so no
# observer effect.
set -uo pipefail
SW="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SW"
PW=$(cat conf/.rpcpassword)
RATE=${RATE:-1500}; DUR=${DUR:-150}
tgt() { awk -v a="$1" '!/^#/&&$1==a{print $2}' hosts.tsv; }
bas() { awk -v a="$1" '!/^#/&&$1==a{print $6}' hosts.tsv; }
ALIASES=$(awk '!/^#/&&NF{print $1}' hosts.tsv)

deploy() {   # deploy <invinterval>
  python3 gen_configs.py --password "$PW" --invinterval "$1" >/dev/null
  for a in $ALIASES; do
    ( t=$(tgt $a); b=$(bas $a)
      timeout 60 ssh -o BatchMode=yes "$t" "cat > $b/data/raptoreum.conf" < conf/$a.conf 2>/dev/null
      timeout 60 ssh -o BatchMode=yes "$t" "$b/bin/raptoreum-cli -regtest -datadir=$b/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW stop" >/dev/null 2>&1
      sleep 12
      timeout 90 ssh -o BatchMode=yes "$t" "$b/bin/raptoreumd -regtest -datadir=$b/data -conf=$b/data/raptoreum.conf -daemon" >/dev/null 2>&1 ) &
  done; wait
  sleep 35
  echo "  applied interval=$1; node confirms:"
  timeout 30 ssh -o BatchMode=yes "$(tgt c1)" "grep 'PERF: relay trickle' $(bas c1)/data/regtest/debug.log | tail -1" 2>/dev/null | sed 's/^/    /'
}

drain() {
  local M=eur
  local A; A=$(timeout 40 ssh -o BatchMode=yes "$(tgt $M)" "$(bas $M)/bin/raptoreum-cli -regtest -datadir=$(bas $M)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW getnewaddress" 2>/dev/null | tail -1)
  timeout 600 ssh -o BatchMode=yes "$(tgt $M)" "$(bas $M)/bin/raptoreum-cli -regtest -datadir=$(bas $M)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW generatetoaddress 60 $A" >/dev/null 2>&1
  sleep 40
}

run_one() {  # run_one <tag> <invinterval>
  echo "=== condition $1 (perfinvinterval=$2) ==="
  deploy "$2"
  drain
  bash poll_mempool.sh "runs/poll-$1.txt" $((DUR+70)) 3 &
  POLL=$!
  MINER=cor SKIP_MINER_LOAD=1 bash run_phase1.sh "$1" "$RATE" 999 "$DUR" > "/tmp/$1.out" 2>&1
  wait $POLL 2>/dev/null
  grep -E 'ABORT|accepted .* of|LOW ACCEPT' "/tmp/$1.out" | sed 's/^/  /'
}

run_one trickle5 0
run_one trickle1 1
echo "=== done ==="
