#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Pure logic for check_body_range_coverage.py -- no network I/O.

F-215 (docs/findings.md), following on F-213 point 4/7(ii): the manual
precondition the owner must check before docs/build-plan.md's 4.6 activation
criterion can ever be given a real ``startHeight`` -- every height in the
range a Smartnode commits to retaining forever (docs/transaction-decoupling.md
:162, "Everything, forever") must read ``status: "full"`` from
``getbodyrangecoverageheights`` (src/rpc/net.cpp:597-668, added by F-212) on
every independently-operated node queried. This module holds the parts of
that check that do not touch the network, so they can be unit tested against
fixed inputs: chunking a height range into RPC-sized requests, turning one
node's raw JSON response into a height->status map, and cross-referencing
several nodes' maps to find the actionable gaps.

Status vocabulary matches src/rpc/net.cpp:619-620's own RPCResult doc exactly:
"full", "partial", "miss". A fourth value, NOT_OBSERVED, is synthesized here
(never sent by the RPC) for a height inside the queried range that is simply
absent from the response -- src/rpc/net.cpp:604-606's own doc: "Only heights
with at least one observation are returned; an absent height in the
requested range has never been observed at all." F-213 point 4's threshold
("every single queried height reads status FULL") makes no exception for
"never observed" -- an unobserved height is not a confirmed FULL height
either, so it is treated as a gap here, not silently skipped.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Tuple

FULL = "full"
PARTIAL = "partial"
MISS = "miss"
NOT_OBSERVED = "not_observed"

# The set of statuses that satisfy F-213 point 4's threshold. Deliberately a
# set of one: "not a percentage or average... one non-FULL height anywhere
# in range is a real, nameable finding" (docs/findings.md F-213 point 4).
PASSING_STATUSES = frozenset({FULL})

# A height that falls after a node's OWN reported chain tip is not a gap in
# that node's retained history -- it simply does not exist there yet. See
# this module's own doc and evaluate_range's below.
BEYOND_TIP = "beyond_tip"

# A node this script could not query at all for some part of the range (RPC
# error, connection failure, timeout). Distinct from a real gap: the script
# must not report PASS when it does not actually know a height's status
# everywhere it needs to.
NODE_ERROR = "error"


def chunk_ranges(start_height: int, end_height_inclusive: int, chunk_size: int) -> List[Tuple[int, int]]:
    """Split [start_height, end_height_inclusive] into <= chunk_size-wide,
    inclusive sub-ranges matching getbodyrangecoverageheights's own
    (start_height, end_height) inclusive argument pair (src/rpc/net.cpp:608-611).

    net.cpp enforces only end_height >= start_height (:638-640) -- no
    server-side cap on span was found by reading the RPC handler, so this
    client-side chunk size is what keeps a single HTTP response bounded;
    see check_body_range_coverage.py's --chunk-size default and help text.
    """
    if chunk_size <= 0:
        raise ValueError("chunk_size must be > 0")
    if end_height_inclusive < start_height:
        raise ValueError("end_height_inclusive must be >= start_height")

    chunks = []
    cur = start_height
    while cur <= end_height_inclusive:
        chunk_end = min(cur + chunk_size - 1, end_height_inclusive)
        chunks.append((cur, chunk_end))
        cur = chunk_end + 1
    return chunks


def parse_heights_response(raw_result) -> Dict[int, str]:
    """Turn one getbodyrangecoverageheights JSON-RPC result (a list of
    {"height", "status", ...}, src/rpc/net.cpp:645-667) into a height->status
    dict. Accepts either an already-decoded list (what AuthServiceProxy /
    RpcClient.call returns) or a raw JSON string, for testability.

    Only "full", "partial", "miss" are accepted status strings -- anything
    else is an RPC contract violation and raises rather than silently being
    treated as a gap or a pass.
    """
    if isinstance(raw_result, (str, bytes)):
        raw_result = json.loads(raw_result)

    out: Dict[int, str] = {}
    for entry in raw_result:
        height = entry["height"]
        status = entry["status"]
        if status not in (FULL, PARTIAL, MISS):
            raise ValueError(
                "unrecognised status %r for height %r -- getbodyrangecoverageheights "
                "only ever returns full/partial/miss (src/rpc/net.cpp:619-620)" % (status, height)
            )
        out[height] = status
    return out


@dataclass
class NodeRangeResult:
    """One node's outcome for the whole requested range.

    `heights`: height -> status for every height this node reported an
    observation for (a subset of the requested range; absence means
    NOT_OBSERVED, not a zero-cost default -- see this module's own doc).
    `tip_height`: this node's own chain tip at query time (getblockcount),
    used so a height beyond THIS node's tip is not misreported as a gap in
    ITS retained history (evaluate_range below).
    `error`: set (to a human-readable message) if any chunk request to this
    node failed outright; a partial `heights` map alongside a set `error`
    must still be treated as incomplete, never as a clean pass for the
    heights it DID get.
    """

    node_name: str
    heights: Dict[int, str] = field(default_factory=dict)
    tip_height: Optional[int] = None
    error: Optional[str] = None


def status_for_height(node_result: NodeRangeResult, height: int) -> str:
    """This node's status for one height, folding in the beyond-tip and
    node-error cases NODE_ERROR/BEYOND_TIP handle (see this module's own
    module-level doc)."""
    if node_result.error is not None:
        return NODE_ERROR
    if node_result.tip_height is not None and height > node_result.tip_height:
        return BEYOND_TIP
    return node_result.heights.get(height, NOT_OBSERVED)


@dataclass
class GapEntry:
    height: int
    statuses: Dict[str, str]  # node_name -> status (only non-passing/non-beyond-tip nodes need to be looked at, but ALL queried nodes' statuses are recorded here for full context, per F-213 point 4's cross-node ask)


@dataclass
class EvaluationResult:
    start_height: int
    end_height: int
    node_names: List[str]
    heights_checked: int
    gaps: List[GapEntry]
    node_errors: Dict[str, str]  # node_name -> error message, only for nodes with a hard failure

    @property
    def passed(self) -> bool:
        """PASS iff every configured node was queried successfully for the
        whole range AND every height (within each node's own tip) reads
        FULL everywhere. An incomplete query (node_errors non-empty) is
        never a pass, even if zero gaps were found in the data that DID
        come back -- F-213 point 4's threshold is "every single queried
        height", which this script cannot claim to have honoured with
        missing data (see docs/findings.md's own feedback on retry drivers/
        pipeline exit codes never letting an unknown stand in for a
        verified good)."""
        return not self.node_errors and not self.gaps

    def to_dict(self) -> dict:
        return {
            "start_height": self.start_height,
            "end_height": self.end_height,
            "nodes": self.node_names,
            "heights_checked": self.heights_checked,
            "pass": self.passed,
            "gap_count": len(self.gaps),
            "gaps": [{"height": g.height, "statuses": g.statuses} for g in self.gaps],
            "node_errors": self.node_errors,
        }


def evaluate_range(
    node_results: Iterable[NodeRangeResult],
    start_height: int,
    end_height_inclusive: int,
) -> EvaluationResult:
    """Cross-reference every configured node's per-height result and report
    every height where at least one node does NOT report FULL (and is not
    simply beyond that node's own tip) -- F-213 point 4's own ask, "report
    any height where at least one queried node does NOT report FULL...
    per-height, with which node(s) disagreed and what status they actually
    reported" (this task's own brief, point 3).

    A height where every node's status is either FULL or BEYOND_TIP (that
    node simply has not synced that far yet) is not a gap. A height where
    any node reports NODE_ERROR is recorded in `node_errors` at the node
    level (the whole node's data for this run is suspect, not just one
    height) and forces `passed` to False -- see EvaluationResult.passed's
    own doc for why an error must never read as a pass.
    """
    node_results = list(node_results)
    node_names = [nr.node_name for nr in node_results]
    node_errors = {nr.node_name: nr.error for nr in node_results if nr.error is not None}

    gaps: List[GapEntry] = []
    for height in range(start_height, end_height_inclusive + 1):
        statuses = {nr.node_name: status_for_height(nr, height) for nr in node_results}
        # A height is fine only if every node's status is FULL or BEYOND_TIP
        # (that node just hasn't reached it yet -- not evidence of a gap in
        # ITS retained history). NODE_ERROR always fails the height, but is
        # already surfaced at the node level via node_errors above; still
        # listed per-height here so a gap report is self-contained.
        ok = all(s in PASSING_STATUSES or s == BEYOND_TIP for s in statuses.values())
        if not ok:
            gaps.append(GapEntry(height=height, statuses=statuses))

    return EvaluationResult(
        start_height=start_height,
        end_height=end_height_inclusive,
        node_names=node_names,
        heights_checked=end_height_inclusive - start_height + 1,
        gaps=gaps,
        node_errors=node_errors,
    )


def format_table(result: EvaluationResult) -> str:
    """Human-readable summary: unambiguous PASS/FAIL/ERROR banner, matching
    this task's own ask (point 4: "the 'pass' case... trivially
    distinguishable from any failure"), followed by the gap list when there
    is one to show."""
    lines = []
    if result.node_errors:
        lines.append("RESULT: ERROR -- %d node(s) could not be fully queried; no pass/fail verdict possible" % len(result.node_errors))
        for name, err in sorted(result.node_errors.items()):
            lines.append("  node %s: %s" % (name, err))
    elif result.passed:
        lines.append(
            "RESULT: PASS -- every height %d..%d read FULL on every queried node (%s)"
            % (result.start_height, result.end_height, ", ".join(result.node_names))
        )
    else:
        lines.append(
            "RESULT: FAIL -- %d of %d heights are not FULL on every node"
            % (len(result.gaps), result.heights_checked)
        )

    if result.gaps:
        lines.append("")
        lines.append("gaps (height: node=status, ...):")
        for gap in result.gaps:
            status_str = ", ".join("%s=%s" % (n, s) for n, s in sorted(gap.statuses.items()))
            lines.append("  %d: %s" % (gap.height, status_str))

    return "\n".join(lines)
