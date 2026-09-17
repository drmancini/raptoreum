#!/bin/bash
# Reset every node to the seed chain (height 426) and rewind the corpus cursor.
# The corpus spends fanout UTXOs, so once they are spent the corpus is dead;
# restoring the snapshot makes the whole corpus valid again.
set -uo pipefail
SW="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PW=$(cat "$SW/conf/.rpcpassword")
SEED_TGZ=${SEED_TGZ:?path to seed-chain.tgz on this host}
tgt() { awk -v a="$1" '!/^#/&&$1==a{print $2}' "$SW/hosts.tsv"; }
bas() { awk -v a="$1" '!/^#/&&$1==a{print $6}' "$SW/hosts.tsv"; }
ALIASES=$(awk '!/^#/&&NF{print $1}' "$SW/hosts.tsv")

echo "--- stopping all nodes ---"
for a in $ALIASES; do
  ( timeout 60 ssh -o BatchMode=yes "$(tgt $a)" \
      "$(bas $a)/bin/raptoreum-cli -regtest -datadir=$(bas $a)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW stop" >/dev/null 2>&1
    echo "  $a stopped" ) &
done; wait
sleep 12

echo "--- wiping and reseeding (parallel) ---"
for a in $ALIASES; do
  ( b=$(bas $a); t=$(tgt $a)
    timeout 900 ssh -o BatchMode=yes "$t" "rm -rf $b/data/regtest && cat > $b/seed.tgz" < "$SEED_TGZ" 2>/dev/null
    timeout 300 ssh -o BatchMode=yes "$t" "tar xzf $b/seed.tgz -C $b/data/ && rm -f $b/seed.tgz && ls $b/data/regtest >/dev/null" 2>/dev/null \
      && echo "  $a reseeded" || echo "  $a FAILED" ) &
done; wait

echo "--- starting all nodes ---"
for a in $ALIASES; do
  ( timeout 90 ssh -o BatchMode=yes "$(tgt $a)" \
      "$(bas $a)/bin/raptoreumd -regtest -datadir=$(bas $a)/data -conf=$(bas $a)/data/raptoreum.conf -daemon" >/dev/null 2>&1
    echo "  $a started" ) &
done; wait
sleep 30

# A node whose tip is older than nMaxTipAge stays in initial block download and
# refuses to serve headers or relay. One freshly mined block releases every node.
echo "--- mining one block to clear IBD network-wide ---"
M=${MINER:-eur}
ADDR=$(timeout 60 ssh -o BatchMode=yes "$(tgt $M)" "$(bas $M)/bin/raptoreum-cli -regtest -datadir=$(bas $M)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW getnewaddress" 2>/dev/null | tail -1)
timeout 120 ssh -o BatchMode=yes "$(tgt $M)" "$(bas $M)/bin/raptoreum-cli -regtest -datadir=$(bas $M)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW generatetoaddress 1 $ADDR" >/dev/null 2>&1
sleep 25

echo 0 > "$SW/runs/.cursor"
echo "--- cursor reset to 0 ---"
bash "$SW/swarmctl.sh" status 2>&1 | grep -E 'height=' | sort
