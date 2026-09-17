#!/bin/bash
# RTM swarm node setup. Run as root. Idempotent -- safe to re-run.
# Does exactly three things: opens the regtest P2P port to the other swarm
# nodes, installs chrony, and reports what it found.
set -u
SWARM_IPS="31.220.102.19 154.53.56.169 46.250.249.159 46.250.228.165 45.85.249.86 \
65.109.84.93 159.69.148.79 154.26.136.8 38.242.232.225 207.244.239.208 \
38.242.128.214 85.207.214.141"
PORT=19899

echo "== $(hostname) =="

# --- 1. firewall: allow the swarm on 19899 only ---------------------------
if command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q "Status: active"; then
    echo "  firewall: ufw (active)"
    for ip in $SWARM_IPS; do ufw allow from "$ip" to any port $PORT proto tcp >/dev/null 2>&1; done
    echo "  -> allowed $PORT/tcp from 12 swarm IPs"
else
    echo "  firewall: ufw not active; using iptables"
    for ip in $SWARM_IPS; do
        iptables -C INPUT -p tcp -s "$ip" --dport $PORT -j ACCEPT 2>/dev/null \
          || iptables -I INPUT -p tcp -s "$ip" --dport $PORT -j ACCEPT
    done
    echo "  -> inserted 12 ACCEPT rules on $PORT/tcp"
    if command -v netfilter-persistent >/dev/null; then netfilter-persistent save >/dev/null 2>&1 && echo "  -> persisted"
    elif [ -d /etc/iptables ]; then iptables-save > /etc/iptables/rules.v4 && echo "  -> persisted"
    else echo "  !! NOT PERSISTED across reboot (no iptables-persistent) -- fine for a test run"; fi
fi

# --- 2. chrony: tight clocks, because we measure propagation --------------
if systemctl is-active --quiet chrony 2>/dev/null || systemctl is-active --quiet chronyd 2>/dev/null; then
    echo "  chrony: already running"
else
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq chrony >/dev/null 2>&1 \
      && echo "  chrony: installed" || echo "  chrony: INSTALL FAILED"
fi
systemctl enable --now chrony >/dev/null 2>&1 || systemctl enable --now chronyd >/dev/null 2>&1

# --- 3. report ------------------------------------------------------------
echo "  listening check: $(ss -ltn 2>/dev/null | grep -c ":$PORT ") socket(s) on $PORT"
echo "  time: $(chronyc tracking 2>/dev/null | awk '/System time/{printf "%.2f ms off", $4*1000}' || echo 'chrony not reporting yet')"
