#!/bin/bash
# Fix apt on this host and install chrony.
#
# Problem: /etc/apt/sources.list.d/noble-main.sources is missing its
# "Components:" line, so apt cannot build an index URL from that stanza and
# silently learns nothing from the Ubuntu archive. Everything else on the box
# is fine -- archive reachable, keyring present, no other broken sources.
#
# Run: sudo bash fix-apt.sh
set -e
F=/etc/apt/sources.list.d/noble-main.sources

echo "== before =="
cat "$F"

if grep -q '^Components:' "$F"; then
    echo "== Components already present, nothing to add =="
else
    cp "$F" "$F.bak"
    sed -i '/^Suites:/a Components: main restricted universe multiverse' "$F"
    echo "== after (backup at $F.bak) =="
    cat "$F"
fi

echo "== apt-get update =="
apt-get update

echo "== installing chrony =="
apt-get install -y chrony

echo "== result =="
apt-cache policy chrony | head -3
systemctl is-active chrony || systemctl is-active chronyd || true
chronyc tracking 2>/dev/null | head -4 || echo "(chrony still settling)"
