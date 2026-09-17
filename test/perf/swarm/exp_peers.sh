#!/bin/bash
# Does relay cost scale with peer count?
#
# RelayTransaction does one PushInventory per peer -- a lock, a bloom lookup and
# a std::set insert each -- so cost should scale with connections if the per-peer
# path dominates. Comparing throughput alone would confound this with topology
# (a sparser mesh needs more hops), so the metric is msghand CPU per accepted
# transaction: microseconds of msghand spent per transaction, which is
# independent of how many hops the transaction took to arrive.
set -uo pipefail
SW="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SW"
PW=$(cat conf/.rpcpassword)
RATE=${RATE:-1500}; DUR=${DUR:-150}
tgt() { awk -v a="$1" '!/^#/&&$1==a{print $2}' hosts.tsv; }
bas() { awk -v a="$1" '!/^#/&&$1==a{print $6}' hosts.tsv; }
ALIASES=$(awk '!/^#/&&NF{print $1}' hosts.tsv)
PROBES="c4 ase cor bow"        # 4-thread, 4-thread+prod, 12-thread, LAN

deploy() {
  python3 gen_configs.py --password "$PW" --peers "$1" >/dev/null
  for a in $ALIASES; do
    ( t=$(tgt $a); b=$(bas $a)
      timeout 60 ssh -o BatchMode=yes "$t" "cat > $b/data/raptoreum.conf" < conf/$a.conf 2>/dev/null
      timeout 60 ssh -o BatchMode=yes "$t" "$b/bin/raptoreum-cli -regtest -datadir=$b/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW stop" >/dev/null 2>&1
      sleep 12
      # peers.dat remembers every peer ever seen and the node redials them on
      # startup, which silently restores a full mesh however few we configure.
      timeout 30 ssh -o BatchMode=yes "$t" "rm -f $b/data/regtest/peers.dat" >/dev/null 2>&1
      timeout 90 ssh -o BatchMode=yes "$t" "$b/bin/raptoreumd -regtest -datadir=$b/data -conf=$b/data/raptoreum.conf -daemon" >/dev/null 2>&1 ) &
  done; wait
  sleep 40
  echo "  connections per node:"
  for a in $PROBES; do
    n=$(timeout 30 ssh -o BatchMode=yes "$(tgt $a)" "$(bas $a)/bin/raptoreum-cli -regtest -datadir=$(bas $a)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW getconnectioncount" 2>/dev/null)
    printf "    %-4s %s\n" "$a" "${n:-?}"
  done
}

drain() {
  local M=eur
  local A; A=$(timeout 40 ssh -o BatchMode=yes "$(tgt $M)" "$(bas $M)/bin/raptoreum-cli -regtest -datadir=$(bas $M)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW getnewaddress" 2>/dev/null | tail -1)
  timeout 900 ssh -o BatchMode=yes "$(tgt $M)" "$(bas $M)/bin/raptoreum-cli -regtest -datadir=$(bas $M)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW generatetoaddress 80 $A" >/dev/null 2>&1
  sleep 45
}

run_one() {   # run_one <tag> <peers>
  echo "=== peers=$2 ==="
  deploy "$2"; drain
  # A sparse mesh propagates blocks over more hops, so the drain's blocks need
  # longer to reach everyone. Without this the preflight gate sees mismatched
  # heights and (correctly) refuses to run.
  echo "  waiting for height convergence..."
  for i in $(seq 40); do
    hs=$(for a in $ALIASES; do
           timeout 20 ssh -o BatchMode=yes "$(tgt $a)" "$(bas $a)/bin/raptoreum-cli -regtest -datadir=$(bas $a)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW getblockcount" 2>/dev/null
         done | sort -u | wc -l)
    [ "$hs" = "1" ] && { echo "  converged after $((i*10))s"; break; }
    sleep 10
  done
  bash poll_mempool.sh "runs/poll-$1.txt" $((DUR+60)) 3 &
  local POLL=$!
  ( MINER=cor SKIP_MINER_LOAD=1 bash run_phase1.sh "$1" "$RATE" 999 "$DUR" > "/tmp/$1.out" 2>&1 ) &
  local RUN=$!
  sleep 75      # let preflight+launch finish, then sample mid-load
  for a in $PROBES; do
    ( b=$(bas $a)
      r=$(timeout 60 ssh -o BatchMode=yes "$(tgt $a)" "python3 - $b 20" < tidcpu.py 2>&1 | grep -E "rtm-msghand|TOTAL")
      echo "  cpu[$a] $(echo "$r" | tr '\n' ' ')" ) &
  done; wait
  wait $RUN $POLL 2>/dev/null
  grep -E 'accepted .* of|ABORT|LOW ACCEPT' "/tmp/$1.out" | sed 's/^/  /'
}

# CONDITIONS lets a single condition be re-run without repeating the other.
for spec in ${CONDITIONS:-"peers11:11 peers3:3"}; do
  run_one "${spec%%:*}" "${spec##*:}"
done
echo "=== done ==="
