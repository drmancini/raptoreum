#!/bin/bash
# Control the RTM regtest swarm. Every node is driven over ssh from here.
set -uo pipefail
SW="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOSTS="$SW/hosts.tsv"
PW=$(cat "$SW/conf/.rpcpassword" 2>/dev/null)

rows() { grep -vE '^\s*#|^\s*$' "$HOSTS"; }
ssh_of()  { rows | awk -v a="$1" '$1==a{print $2}'; }
base_of() { rows | awk -v a="$1" '$1==a{print $6}'; }
aliases() { rows | awk '{print $1}'; }

cli() {  # cli <alias> <rpc args...>
  local a=$1; shift
  local t b; t=$(ssh_of "$a"); b=$(base_of "$a")
  timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=10 "$t" \
    "$b/bin/raptoreum-cli -regtest -datadir=$b/data -rpcport=19898 -rpcuser=swarm -rpcpassword=$PW $*" 2>&1
}

each() {  # each <function> -- run across all nodes in parallel
  local fn=$1; shift
  for a in $(aliases); do ( $fn "$a" "$@" ) & done; wait
}

start_one() {
  local a=$1 t b; t=$(ssh_of "$a"); b=$(base_of "$a")
  local r
  r=$(timeout 90 ssh -o BatchMode=yes "$t" \
    "cd $b && ./bin/raptoreumd -regtest -datadir=$b/data -conf=$b/data/raptoreum.conf -daemon 2>&1 | head -2")
  printf "  %-4s %s\n" "$a" "${r:-started}"
}

stop_one() {
  local a=$1
  local r; r=$(cli "$a" stop)
  printf "  %-4s %s\n" "$a" "$r"
}

status_one() {
  local a=$1 t; t=$(ssh_of "$a")
  local bi mi pi
  bi=$(cli "$a" getblockcount)
  mi=$(cli "$a" getmempoolinfo | tr -d ' ",' | awk -F: '/^size/{s=$2} /^bytes/{b=$2} /^usage/{u=$2} END{printf "%s tx / %.0f MB", s, u/1048576}')
  pi=$(cli "$a" getconnectioncount)
  printf "  %-4s %-26s height=%-6s peers=%-4s mempool=%s\n" "$a" "$t" "${bi:-?}" "${pi:-?}" "${mi:-?}"
}

case "${1:-}" in
  start)   echo "=== starting swarm ==="; each start_one ;;
  stop)    echo "=== stopping swarm ==="; each stop_one ;;
  status)  echo "=== swarm status ==="; each status_one ;;
  rpc)     shift; a=$1; shift; cli "$a" "$@" ;;
  mine)    shift; a=$1; n=${2:-1}
           addr=$(cli "$a" getnewaddress)
           cli "$a" generatetoaddress "$n" "$addr" ;;
  aliases) aliases ;;
  *) echo "usage: $0 {start|stop|status|rpc <alias> <method...>|mine <alias> [n]|aliases}"; exit 1 ;;
esac
