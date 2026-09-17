#!/usr/bin/env python3
"""Per-thread CPU from /proc/<tid>/stat deltas -- the only honest way to get an
instantaneous figure; top -b -n1 computes its first sample from process start."""
import os, sys, time, glob
base, secs = sys.argv[1], float(sys.argv[2])
pid = None
for p in glob.glob("/proc/[0-9]*/cmdline"):
    try:
        c = open(p, "rb").read().replace(b"\0", b" ").decode(errors="replace")
    except Exception:
        continue
    if "raptoreumd" in c and ("datadir=%s/data" % base) in c:
        pid = p.split("/")[2]; break
if not pid:
    print("  no swarm raptoreumd"); sys.exit(1)
HZ = os.sysconf("SC_CLK_TCK")
def snap():
    out = {}
    for t in os.listdir("/proc/%s/task" % pid):
        try:
            f = open("/proc/%s/task/%s/stat" % (pid, t)).read()
            name = f[f.index("(")+1:f.rindex(")")]
            fields = f[f.rindex(")")+2:].split()
            out[t] = (name, int(fields[11]) + int(fields[12]))
        except Exception:
            pass
    return out
a = snap(); time.sleep(secs); b = snap()
rows = []
for t, (name, t1) in b.items():
    if t in a:
        rows.append((100.0 * (t1 - a[t][1]) / HZ / secs, name))
rows.sort(reverse=True)
tot = sum(r[0] for r in rows)
for pct, name in rows[:6]:
    if pct > 0.5: print("  %-16s %6.1f%%" % (name, pct))
print("  %-16s %6.1f%% of one core" % ("TOTAL", tot))
