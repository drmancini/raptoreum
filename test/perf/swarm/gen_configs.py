#!/usr/bin/env python3
"""Generate a raptoreum.conf per swarm node (full mesh) from hosts.tsv + swarm-ips.txt."""
import argparse, os, secrets

ap = argparse.ArgumentParser()
ap.add_argument("--hosts", default="hosts.tsv")
ap.add_argument("--ips", default="swarm-ips.txt")
ap.add_argument("--out", default="conf")
ap.add_argument("--password", default="")
ap.add_argument("--invmax", type=int, default=50000)
a = ap.parse_args()

rows = []
for line in open(a.hosts):
    if line.startswith("#") or not line.strip():
        continue
    f = line.split()
    rows.append({"alias": f[0], "ssh": f[1], "thr": int(f[2]), "mem": int(f[3]),
                 "maxmempool": int(f[4]), "base": f[5], "region": f[6]})
ips = [l.strip() for l in open(a.ips) if l.strip()]
assert len(ips) == len(rows), "ip count %d != host count %d" % (len(ips), len(rows))
for r, ip in zip(rows, ips):
    r["ip"] = ip

pw = a.password or secrets.token_urlsafe(24)
os.makedirs(a.out, exist_ok=True)

for r in rows:
    # Full mesh: every node dials every other. bowser is behind NAT, so it only
    # ever completes outbound connections -- which is enough to be a full peer.
    peers = [x["ip"] for x in rows if x["alias"] != r["alias"]]
    conf = [
        "# rtm swarm node %s (%s, %s) -- regtest" % (r["alias"], r["ssh"], r["region"]),
        "# Network-specific options MUST live under [regtest]; at top level the node",
        "# warns and ignores them, which silently leaves defaults in place.",
        "rpcuser=swarm",
        "rpcpassword=%s" % pw,
        "rpcthreads=8",
        "rpcworkqueue=512",
        "dnsseed=0",
        "",
        "# regtest re-validates the whole mempool on every accept unless this is off",
        "# (fDefaultConsistencyChecks=true) -- a ~60x understatement of throughput.",
        "checkmempool=0",
        "# regtest sets fRequireStandard=false; force mainnet standardness back on.",
        "acceptnonstdtxn=0",
        "",
        "maxmempool=%d" % r["maxmempool"],
        "dbcache=450",
        "maxconnections=125",
        "",
        "# shipped relay cap is block-indexed (280/trickle = 56 tx/s) and cannot",
        "# carry the v1 operating point; dev-build flag only, never upstream.",
        "perfinvmax=%d" % a.invmax,
        "",
        "debug=bench",
        "debug=cmpctblock",
        # Per-transaction arrival times. AcceptToMemoryPool logs one line per
        # accepted tx under this category; with logtimemicros that gives each
        # node's arrival time for every txid, which is the only way to measure
        # true propagation latency -- getrawmempool's "time" is whole seconds.
        "debug=mempool",
        "logtimemicros=1",
        "",
        "[regtest]",
        "listen=1",
        "bind=0.0.0.0:19899",
        "rpcbind=127.0.0.1",
        "rpcport=19898",
        "rpcallowip=127.0.0.1",
    ] + ["addnode=%s:19899" % p for p in peers]
    open(os.path.join(a.out, "%s.conf" % r["alias"]), "w").write("\n".join(conf) + "\n")

open(os.path.join(a.out, ".rpcpassword"), "w").write(pw + "\n")
print("wrote %d configs to %s/ (full mesh, %d peers each)" % (len(rows), a.out, len(rows) - 1))
print("rpc password stored in %s/.rpcpassword" % a.out)
