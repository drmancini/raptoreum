#!/bin/bash
# Register every swarm node as a smartnode, so the mesh can form real quorums.
#
# Build-plan item 0.4. Until this runs the swarm has zero smartnodes and empty
# quorums, which is why every figure it has produced so far is an upper bound:
# nothing has been paying for islock verification, ChainLock signing, or the
# DKG traffic a real network carries.
#
# Three stages, separated because only the middle one changes the chain:
#
#   keys    generate a BLS keypair per node and write the manifest.  Harmless.
#   register  one protx register_fund per node, then mine to confirm.  CHANGES
#             THE CHAIN -- and mining also sweeps whatever corpus transactions
#             are sitting in the mempools into blocks, which consumes the
#             prepared corpus UTXOs. swarm-reset.sh restores the snapshot.
#   enable  write smartnodeblsprivkey + llmqtestparams into each node's conf and
#           restart.  Disruptive but reversible -- the previous conf is kept.
#
# Collateral is 10 RTM on regtest (consensus.nCollaterals in CRegTestParams), so
# twelve nodes cost 120 RTM against eur's balance of ~204,000.
set -uo pipefail
cd "$(dirname "$0")"

FUNDER=eur                       # the only node holding spendable coin
QSIZE=${QSIZE:-10}               # quorum size; 10 of 12 leaves real selection to do
QTHRESH=${QTHRESH:-6}            # threshold -- also sets minSize and dkgBadVotesThreshold
MANIFEST=smartnodes.json
IPS=/tmp/claude-1002/-home-mike-forge-projects/43306fc0-14f0-4b9a-aba5-5c3d8fa3b9a7/scratchpad/swarm-ips.json

rpc(){ ./swarmctl.sh rpc "$@" 2>/dev/null | tr -d '\r'; }
aliases(){ ./swarmctl.sh aliases 2>/dev/null; }

# ---- stage: keys -----------------------------------------------------------
# A BLS keypair per node. The secret goes in that node's own config; the public
# half goes on-chain in its ProRegTx. Nothing here touches the chain.
stage_keys(){
  echo "=== generating BLS keys ==="
  python3 - "$IPS" <<'PY' > "$MANIFEST.tmp"
import json,subprocess,sys
ips=json.load(open(sys.argv[1]))
out={}
for a in sorted(ips):
    r=subprocess.run(["./swarmctl.sh","rpc","eur","bls","generate"],capture_output=True,text=True)
    try: bls=json.loads(r.stdout)
    except Exception: sys.exit("bls generate failed for %s: %s"%(a,r.stdout[:200]+r.stderr[:200]))
    out[a]={"ip":ips[a],"bls_public":bls["public"],"bls_secret":bls["secret"]}
    print("  %-5s %s  pub=%s..."%(a,ips[a],bls["public"][:16]),file=sys.stderr)
json.dump(out,sys.stdout,indent=1)
PY
  [ -s "$MANIFEST.tmp" ] || { echo "no manifest produced"; exit 1; }
  mv "$MANIFEST.tmp" "$MANIFEST"
  echo "wrote $MANIFEST ($(python3 -c "import json;print(len(json.load(open('$MANIFEST'))))") nodes)"
}

# ---- stage: register -------------------------------------------------------
# register_fund creates the collateral output INSIDE the ProRegTx, so there is no
# separate funding transaction to track and no window where a 10 RTM output is
# sitting unlocked. The real collateral outpoint is read back from protx info,
# because it is not the one any sendtoaddress would have produced.
stage_register(){
  [ -f "$MANIFEST" ] || { echo "run '$0 keys' first"; exit 1; }
  echo "=== registering smartnodes (this changes the chain) ==="
  python3 - "$MANIFEST" <<'PY'
import json,subprocess,sys
m=json.load(open(sys.argv[1]))
def rpc(*a):
    r=subprocess.run(["./swarmctl.sh","rpc","eur",*a],capture_output=True,text=True)
    return r.stdout.strip(), r.returncode
for a in sorted(m):
    n=m[a]
    if n.get("protx"): print("  %-5s already registered"%a); continue
    addr,_=rpc("getnewaddress"); owner,_=rpc("getnewaddress")
    voting,_=rpc("getnewaddress"); rewards,_=rpc("getnewaddress")
    out,rc=rpc("protx","register_fund",addr,"10","%s:19899"%n["ip"],
               owner,n["bls_public"],voting,"0",rewards,addr,"true")
    if rc!=0 or len(out)!=64:
        print("  %-5s FAILED: %s"%(a,out[:180])); continue
    n.update(protx=out,owner=owner,voting=voting,rewards=rewards,collateral_addr=addr)
    print("  %-5s protx=%s"%(a,out[:16]+"..."))
    json.dump(m,open(sys.argv[1],"w"),indent=1)
PY
  echo "--- mining to confirm ---"
  ./swarmctl.sh mine "$FUNDER" 2 >/dev/null
  sleep 6
  echo "smartnode list: $(rpc "$FUNDER" smartnodelist json | grep -c proTxHash || echo 0) entries"
}

# ---- stage: enable ---------------------------------------------------------
# Each node needs its own BLS secret and the resized quorum parameters, then a
# restart. The previous config is kept beside it so this is reversible.
stage_enable(){
  [ -f "$MANIFEST" ] || { echo "run '$0 keys' first"; exit 1; }
  echo "=== enabling smartnode mode (quorum ${QSIZE}/${QTHRESH}) ==="
  for a in $(aliases); do
    sec=$(python3 -c "import json;print(json.load(open('$MANIFEST'))['$a']['bls_secret'])" 2>/dev/null)
    [ -n "$sec" ] || { echo "  $a: no key in manifest, skipping"; continue; }
    tgt=$(awk -v A="$a" '$1==A{print $2}' hosts.tsv)
    base=$(awk -v A="$a" '$1==A{print $6}' hosts.tsv)
    ssh -o BatchMode=yes "$tgt" "
      f=$base/raptoreum.conf
      cp -n \$f \$f.pre-smartnode 2>/dev/null
      grep -v '^smartnodeblsprivkey=\|^llmqtestparams=' \$f > \$f.new
      printf 'smartnodeblsprivkey=%s\nllmqtestparams=%s:%s\n' '$sec' '$QSIZE' '$QTHRESH' >> \$f.new
      mv \$f.new \$f
    " && echo "  $a: configured" || echo "  $a: FAILED"
  done
  echo "--- restarting ---"
  ./swarmctl.sh stop >/dev/null; sleep 8; ./swarmctl.sh start >/dev/null; sleep 20
  ./swarmctl.sh status
}

case "${1:-}" in
  keys)     stage_keys ;;
  register) stage_register ;;
  enable)   stage_enable ;;
  *) echo "usage: $0 {keys|register|enable}"
     echo "  keys      BLS keypair per node -> $MANIFEST      (no chain change)"
     echo "  register  protx register_fund per node + mine    (CHANGES THE CHAIN)"
     echo "  enable    write conf + restart the swarm         (disruptive, reversible)"
     exit 1 ;;
esac
