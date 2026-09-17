#!/bin/bash
# Sample every node's mempool size on a fixed cadence. The miner offers no load,
# so its growth rate is the relay delivery rate -- S measured directly, with no
# per-transaction logging and therefore no observer effect.
set -uo pipefail
SW="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PW=$(cat "$SW/conf/.rpcpassword")
OUT=${1:?out file}; SECS=${2:-200}; EVERY=${3:-3}
tgt() { awk -v a="$1" '!/^#/&&$1==a{print $2}' "$SW/hosts.tsv"; }
bas() { awk -v a="$1" '!/^#/&&$1==a{print $6}' "$SW/hosts.tsv"; }
ALIASES=$(awk '!/^#/&&NF{print $1}' "$SW/hosts.tsv")
: > "$OUT"
end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$end" ]; do
  now=$(date +%s.%N)
  for a in $ALIASES; do
    ( n=$(timeout 20 ssh -o BatchMode=yes "$(tgt $a)" \
        "$(bas $a)/bin/raptoreum-cli -regtest -datadir=$(bas $a)/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW getmempoolinfo" 2>/dev/null \
        | tr -d ' ",' | awk -F: '/^size/{print $2}')
      [ -n "$n" ] && echo "$now $a $n" >> "$OUT" ) &
  done
  wait
  sleep "$EVERY"
done
