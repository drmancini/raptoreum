# Stdlib SNTP client: report this host's clock offset vs a common reference.
import socket, struct, sys, time, statistics
SERVER = sys.argv[1] if len(sys.argv) > 1 else "time.cloudflare.com"
NTP_EPOCH = 2208988800
def sample(srv):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
    p = b'\x1b' + 47*b'\0'
    t1 = time.time(); s.sendto(p, (srv, 123))
    d, _ = s.recvfrom(256); t4 = time.time(); s.close()
    t2 = struct.unpack("!I", d[32:36])[0] + struct.unpack("!I", d[36:40])[0]/2**32 - NTP_EPOCH
    t3 = struct.unpack("!I", d[40:44])[0] + struct.unpack("!I", d[44:48])[0]/2**32 - NTP_EPOCH
    return ((t2-t1)+(t3-t4))/2, (t4-t1)-(t3-t2)
offs=[]; dels=[]
for _ in range(5):
    try:
        o,d = sample(SERVER); offs.append(o); dels.append(d); time.sleep(0.2)
    except Exception as e:
        pass
if not offs:
    print("FAILED"); sys.exit(1)
best = offs[dels.index(min(dels))]   # lowest-delay sample is the least biased
print("%+8.2f ms offset   (median %+.2f, spread %.2f, rtt %.0f ms)" %
      (best*1000, statistics.median(offs)*1000, (max(offs)-min(offs))*1000, min(dels)*1000))
