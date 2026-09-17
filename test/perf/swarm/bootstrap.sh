#!/bin/bash
# Two-phase bring-up. A node sends exactly one getheaders, to whichever peer
# connected first; if that peer is empty the sync latches and never retries.
# So: sync against the seed alone, then restart into the full mesh.
set -uo pipefail
SW="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PW=$(cat "$SW/conf/.rpcpassword")
SEED_IP=${SEED_IP:-45.85.249.86}
for a in "$@"; do
  t=$(awk -v a="$a" '!/^#/&&$1==a{print $2}' "$SW/hosts.tsv")
  b=$(awk -v a="$a" '!/^#/&&$1==a{print $6}' "$SW/hosts.tsv")
  (
    C="$b/bin/raptoreum-cli -regtest -datadir=$b/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW"
    timeout 60 ssh -o BatchMode=yes "$t" "$C stop" >/dev/null 2>&1; sleep 10
    # bootstrap conf: identical minus the mesh, plus a single seed peer
    grep -v '^addnode=' "$SW/conf/$a.conf" > /tmp/boot-$a.conf
    echo "connect=$SEED_IP:19899" >> /tmp/boot-$a.conf
    timeout 60 ssh -o BatchMode=yes "$t" "cat > $b/data/bootstrap.conf" < /tmp/boot-$a.conf
    timeout 60 ssh -o BatchMode=yes "$t" "$b/bin/raptoreumd -regtest -datadir=$b/data -conf=$b/data/bootstrap.conf -daemon" >/dev/null 2>&1
    h=0
    for i in $(seq 1 40); do
      sleep 15
      h=$(timeout 30 ssh -o BatchMode=yes "$t" "$C getblockcount" 2>/dev/null | tr -d '\r')
      [ "${h:-0}" -ge 427 ] 2>/dev/null && break
    done
    echo "  $a: bootstrap height=$h"
    timeout 60 ssh -o BatchMode=yes "$t" "$C stop" >/dev/null 2>&1; sleep 10
    timeout 60 ssh -o BatchMode=yes "$t" "$b/bin/raptoreumd -regtest -datadir=$b/data -conf=$b/data/raptoreum.conf -daemon" >/dev/null 2>&1
    echo "  $a: rejoined mesh"
  ) &
done
wait
