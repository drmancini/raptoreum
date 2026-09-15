#!/usr/bin/env bash
# Run islock_rate.py across quorum sizes.
#
# The BLS benchmark shows threshold recovery is linear in the threshold at about
# 0.3 ms per share, so a sweep separates the two terms: what the pipeline costs
# per session regardless of quorum size, and what the cryptography adds.
set -euo pipefail

TREE=${TREE:-/data/forge/projects/raptoreum}
OUTDIR=${OUTDIR:-/data/rtm-perf/runs}
PY=${PY:-$HOME/.pyenv/versions/3.11.9/bin/python3}
COUNT=${COUNT:-200}

# smartnodes:size:threshold — MAX_NODES is 15 in the framework, so 14 is the ceiling
for spec in 5:5:3 9:9:6 13:13:8; do
    IFS=: read -r mns size threshold <<<"$spec"
    tag="${size}-${threshold}"
    echo "=== quorum ${size}/${threshold} with ${mns} smartnodes ==="
    rm -rf "/data/rtm-perf/tmp-islock-${tag}"
    ISLOCK_MNS=$mns ISLOCK_SIZE=$size ISLOCK_THRESHOLD=$threshold \
    ISLOCK_COUNT=$COUNT ISLOCK_OUT="${OUTDIR}/islock-${tag}.json" \
    PYTHONPATH="${TREE}/test/functional" \
        "$PY" "${TREE}/test/perf/islock_rate.py" \
        --tmpdir="/data/rtm-perf/tmp-islock-${tag}" 2>&1 | tail -5
done

echo
echo "=== results ==="
for f in "${OUTDIR}"/islock-*.json; do
    echo "--- $f"
    cat "$f"
done
