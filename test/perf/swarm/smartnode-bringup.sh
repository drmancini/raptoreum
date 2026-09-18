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

FUNDER=eur                       # historical: the wallet now lives on mario, outside the set
QSIZE=${QSIZE:-10}               # quorum size; 10 of 11 smartnodes leaves selection something to do
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
# register_fund moves the collateral into an output it creates itself, so the
# collateral outpoint is not one any sendtoaddress produced. It draws the funds
# from fundAddress, which must already hold coin -- see the note inside.
stage_register(){
  [ -f "$MANIFEST" ] || { echo "run '$0 keys' first"; exit 1; }
  echo "=== registering smartnodes (this changes the chain) ==="
  python3 - "$MANIFEST" <<'PY'
import json,subprocess,sys,time
m=json.load(open(sys.argv[1]))
def rpc(*a):
    r=subprocess.run(["./swarmctl.sh","rpc","eur",*a],capture_output=True,text=True)
    return r.stdout.strip(), r.returncode
def mine(n):
    addr,_=rpc("getnewaddress")
    subprocess.run(["./swarmctl.sh","mine","eur",str(n)],capture_output=True,text=True)

# register_fund draws the collateral from fundAddress -- and if fundAddress is
# omitted it falls back to payoutAddress, so a funded address is required either
# way. One shared pot does not work: the first ProRegTx spends its UTXO and the
# change lands on a wallet change address, leaving the pot empty for the second.
# So give every node its own funded address, each with a single UTXO big enough
# for the 10 RTM collateral plus fee.
todo=[a for a in sorted(m) if not m[a].get("protx")]
for a in todo:
    if not m[a].get("fund_addr"):
        addr,_=rpc("getnewaddress")
        txid,rc=rpc("sendtoaddress",addr,"11")
        if rc!=0 or len(txid)!=64: sys.exit("funding %s failed: %s"%(a,txid[:200]))
        m[a]["fund_addr"]=addr
        print("  %-5s funded %s"%(a,addr))
json.dump(m,open(sys.argv[1],"w"),indent=1)
print("--- mining to confirm funding ---")
mine(1); time.sleep(5)

for a in todo:
    n=m[a]
    coll,_=rpc("getnewaddress"); owner,_=rpc("getnewaddress")
    voting,_=rpc("getnewaddress"); payout,_=rpc("getnewaddress")
    out,rc=rpc("protx","register_fund",coll,"10","%s:19899"%n["ip"],
               owner,n["bls_public"],voting,"0",payout,n["fund_addr"],"true")
    if rc!=0 or len(out)!=64:
        print("  %-5s FAILED: %s"%(a,out.replace(chr(10)," ")[:160])); continue
    n.update(protx=out,owner=owner,voting=voting,payout=payout,collateral_addr=coll)
    print("  %-5s protx=%s"%(a,out[:16]+"..."))
    json.dump(m,open(sys.argv[1],"w"),indent=1)
PY
  echo "--- mining to confirm registrations ---"
  ./swarmctl.sh mine "$FUNDER" 2 >/dev/null
  sleep 8
  echo "smartnode list: $(rpc "$FUNDER" smartnodelist json | grep -c proTxHash || echo 0) entries"
}

# ---- stage: enable ---------------------------------------------------------
# Each node needs its own BLS secret and the resized quorum parameters, then a
# restart. The previous config is kept beside it so this is reversible.
stage_enable(){
  [ -f "$MANIFEST" ] || { echo "run '$0 keys' first"; exit 1; }
  echo "=== enabling smartnode mode (quorum ${QSIZE}/${QTHRESH}) ==="
  # Every registered node must actually RUN as a smartnode. One that is in the
  # deterministic list but not in smartnode mode still gets selected for quorums
  # and then contributes nothing, which fails the DKG for everyone in it.
  #
  # So all twelve run as smartnodes and the wallet lives elsewhere: raptoreumd
  # refuses outright with "You can not start a smartnode with wallet enabled",
  # which no config can argue with. mario holds the wallet and the keys to every
  # collateral, and is deliberately not in the registered set.
  for a in $(aliases); do
    sec=$(python3 -c "import json;print(json.load(open('$MANIFEST'))['$a']['bls_secret'])" 2>/dev/null)
    [ -n "$sec" ] || { echo "  $a: no key in manifest, skipping"; continue; }
    tgt=$(awk -v A="$a" '$1==A{print $2}' hosts.tsv)
    base=$(awk -v A="$a" '$1==A{print $6}' hosts.tsv)
    # The datadir is $base/data and the config lives inside it -- see swarmctl.sh,
    # which passes -datadir=$b/data -conf=$b/data/raptoreum.conf. Writing to
    # $base/raptoreum.conf instead creates a file the node never reads, and every
    # step still succeeds, so the stage reports success while changing nothing.
    ip=$(python3 -c "import json;print(json.load(open('$MANIFEST'))['$a']['ip'])")
    conf="$base/data/raptoreum.conf"
    ssh -o BatchMode=yes "$tgt" "
      test -f $conf || { echo MISSING; exit 3; }
      cp -n $conf $conf.pre-smartnode 2>/dev/null
      grep -v '^smartnodeblsprivkey=\|^llmqtestparams=\|^disablewallet=\|^externalip=' $conf > $conf.new &&
      printf 'smartnodeblsprivkey=%s\nllmqtestparams=%s:%s\nexternalip=%s\n' '$sec' '$QSIZE' '$QTHRESH' '$ip' >> $conf.new &&
      mv $conf.new $conf &&
      grep -c '^smartnodeblsprivkey=' $conf
    " >/tmp/.sn.$a 2>&1
    got=$(tr -d '\r\n' </tmp/.sn.$a)
    if [ "$got" = "1" ]; then echo "  $a: configured"; else echo "  $a: FAILED ($got)"; fi
    rm -f /tmp/.sn.$a
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
