#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""F-215 (docs/findings.md): the operational script F-213 point 7(ii) flagged
as follow-up work -- wraps getbodyrangecoverageheights (src/rpc/net.cpp:597,
added by F-212) across several configured peer nodes so the owner's own
manual precondition check (F-213 point 4, before docs/build-plan.md's 4.6
activation criterion can ever be given a real startHeight) is not a
by-hand RPC loop.

The check: poll getbodyrangecoverageheights across several independently-
operated, well-connected Smartnode-tier nodes, over the FULL height range a
Smartnode commits to retaining forever (docs/transaction-decoupling.md:162,
"Everything, forever" -- not the windowed tier's 720-block floor), and
confirm every single queried height reads status FULL
(coveragetelemetry.h's HeightCoverageStatus) on every one of them. One
non-FULL height anywhere in range is a real, nameable finding -- not a
percentage, not an average.

This is non-consensus operational tooling. It does not read, write, or
import anything from src/*.cpp/.h at build time -- it is a standalone
Python client of the JSON-RPC surface those files expose.

Usage
-----
    python3 check_body_range_coverage.py --config nodes.json
    python3 check_body_range_coverage.py --config nodes.json --start-height 0 --end-height 1500000
    python3 check_body_range_coverage.py --config nodes.json --output json > report.json

nodes.json is a JSON list, one object per node:
    [
      {"name": "smartnode-a", "host": "203.0.113.10", "port": 10225,
       "rpcuser": "rtm", "rpcpassword": "..."},
      {"name": "smartnode-b", "host": "203.0.113.20", "port": 10225,
       "rpccookiefile": "/path/to/.cookie"}
    ]

See rpc_client.py's NodeConfig for the full field list (host/port required;
exactly one of rpcuser+rpcpassword or rpccookiefile required per node,
matching how raptoreum-cli itself authenticates).

Exit code is 0 only on a genuine PASS (every configured node reachable,
every queried height FULL everywhere within that node's own tip). Any gap
or any node error exits 1 -- safe to wire into a pre-registration checklist
or CI gate without parsing the human-readable output.
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, List, Optional

from coverage_logic import EvaluationResult, NodeRangeResult, chunk_ranges, evaluate_range, format_table, parse_heights_response
from rpc_client import NodeConfig, RpcClient, RpcError, RpcTransportError, load_node_configs

DEFAULT_CHUNK_SIZE = 2000
DEFAULT_CONCURRENCY = 16
DEFAULT_PER_NODE_CONCURRENCY = 4


def fetch_node_tip(node: NodeConfig) -> int:
    """This node's current chain height (getblockcount) -- used both to
    default --end-height (docs' own "Everything, forever" resolved down to
    "everything reachable right now") and to tell "beyond this node's tip"
    apart from a real retention gap (coverage_logic.status_for_height)."""
    return int(RpcClient(node).call("getblockcount"))


def fetch_node_range(
    node: NodeConfig,
    start_height: int,
    end_height_inclusive: int,
    chunk_size: int,
    per_node_semaphore: threading.Semaphore,
) -> NodeRangeResult:
    """Query one node for the whole range, chunked (chunk_ranges), throttled
    to at most per_node_semaphore's own count of concurrent in-flight
    requests to THIS node -- politeness toward a real remote operator, not
    just our own pool's global cap (see this module's --per-node-concurrency
    help text). Stops at the first RPC/transport failure for this node and
    records it (NodeRangeResult.error) rather than silently returning a
    partial "clean" result -- see coverage_logic.EvaluationResult.passed's
    own doc for why an incomplete node must never read as a pass.
    """
    heights: Dict[int, str] = {}
    tip: Optional[int] = None
    try:
        tip = fetch_node_tip(node)
    except (RpcError, RpcTransportError) as e:
        return NodeRangeResult(node_name=node.name, error="getblockcount failed: %s" % e)

    # Nothing to query on this node past its own tip -- clamp so we don't
    # send a range that can only ever come back empty. status_for_height
    # would treat those heights as BEYOND_TIP anyway, but there is no point
    # spending an RPC round trip finding that out chunk by chunk.
    node_end = min(end_height_inclusive, tip)
    if node_end < start_height:
        return NodeRangeResult(node_name=node.name, heights={}, tip_height=tip)

    chunks = chunk_ranges(start_height, node_end, chunk_size)
    for chunk_start, chunk_end in chunks:
        with per_node_semaphore:
            try:
                raw = RpcClient(node).call("getbodyrangecoverageheights", [chunk_start, chunk_end])
            except (RpcError, RpcTransportError) as e:
                return NodeRangeResult(
                    node_name=node.name,
                    heights=heights,
                    tip_height=tip,
                    error="getbodyrangecoverageheights(%d, %d) failed: %s" % (chunk_start, chunk_end, e),
                )
        try:
            heights.update(parse_heights_response(raw))
        except ValueError as e:
            return NodeRangeResult(node_name=node.name, heights=heights, tip_height=tip, error=str(e))

    return NodeRangeResult(node_name=node.name, heights=heights, tip_height=tip)


def run(
    nodes: List[NodeConfig],
    start_height: Optional[int],
    end_height: Optional[int],
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    concurrency: int = DEFAULT_CONCURRENCY,
    per_node_concurrency: int = DEFAULT_PER_NODE_CONCURRENCY,
) -> EvaluationResult:
    """Query every node concurrently (ThreadPoolExecutor -- this is I/O-bound
    RPC traffic, per the parallel-data-jobs skill's own table: HTTP/API
    calls are a threads-or-asyncio case, not ProcessPoolExecutor) and cross-
    reference the results.

    start_height/end_height default to the full range "Everything, forever"
    (docs/transaction-decoupling.md:162) resolves to right now: genesis (0)
    through the LOWEST tip reported by any configured node -- the greatest
    height every node can meaningfully be compared at without one node's
    query for heights it hasn't synced yet reading as a false gap. If the
    caller passes an explicit --end-height beyond some node's own tip, that
    is handled per-node (fetch_node_range's own clamp), not by silently
    lowering everyone else's range.
    """
    if start_height is None:
        start_height = 0

    per_node_semaphore = threading.Semaphore(per_node_concurrency)

    tips: Dict[str, int] = {}
    if end_height is None:
        with ThreadPoolExecutor(max_workers=min(concurrency, len(nodes))) as pool:
            futs = {pool.submit(fetch_node_tip, node): node for node in nodes}
            for fut in as_completed(futs):
                node = futs[fut]
                try:
                    tips[node.name] = fut.result()
                except (RpcError, RpcTransportError) as e:
                    # Surfaced properly once the real range query runs this
                    # node through fetch_node_range too; here we just cannot
                    # contribute a tip toward the auto-computed end_height.
                    print("warning: could not fetch tip from %s: %s" % (node.name, e), file=sys.stderr)
        if not tips:
            raise RuntimeError("could not determine --end-height automatically: no node returned getblockcount")
        end_height = min(tips.values())

    node_results: List[NodeRangeResult] = []
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futs = {
            pool.submit(fetch_node_range, node, start_height, end_height, chunk_size, per_node_semaphore): node
            for node in nodes
        }
        for fut in as_completed(futs):
            node_results.append(fut.result())

    # as_completed does not preserve submission order; sort so output is
    # stable/reproducible across runs.
    node_results.sort(key=lambda nr: nr.node_name)

    return evaluate_range(node_results, start_height, end_height)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="F-215: cross-node getbodyrangecoverageheights precondition check "
        "for docs/build-plan.md 4.6's activation criterion (docs/findings.md F-213 point 4).",
    )
    p.add_argument("--config", required=True, help="Path to a JSON node list (see this file's module docstring).")
    p.add_argument("--start-height", type=int, default=None, help="First height to check (default: 0, genesis).")
    p.add_argument(
        "--end-height",
        type=int,
        default=None,
        help="Last height to check, inclusive (default: the lowest current tip -- getblockcount -- "
        "among all configured nodes, so no node is queried past what it has synced).",
    )
    p.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK_SIZE,
        help="Heights per getbodyrangecoverageheights call (default %d). src/rpc/net.cpp's handler enforces "
        "no server-side maximum span (read directly before choosing this default) -- this bounds each "
        "HTTP response client-side." % DEFAULT_CHUNK_SIZE,
    )
    p.add_argument(
        "--concurrency",
        type=int,
        default=DEFAULT_CONCURRENCY,
        help="Max concurrent in-flight RPC requests across all nodes combined (default %d). This is I/O-bound "
        "work (thread pool, not process pool -- see this file's module docstring)." % DEFAULT_CONCURRENCY,
    )
    p.add_argument(
        "--per-node-concurrency",
        type=int,
        default=DEFAULT_PER_NODE_CONCURRENCY,
        help="Max concurrent in-flight requests to any ONE node (default %d) -- keeps this script polite to a "
        "single remote operator even when --concurrency is raised for a large node set." % DEFAULT_PER_NODE_CONCURRENCY,
    )
    p.add_argument("--output", choices=["table", "json"], default="table", help="Report format (default table).")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)

    try:
        nodes = load_node_configs(args.config)
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 2

    try:
        result = run(
            nodes,
            start_height=args.start_height,
            end_height=args.end_height,
            chunk_size=args.chunk_size,
            concurrency=args.concurrency,
            per_node_concurrency=args.per_node_concurrency,
        )
    except RuntimeError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2

    if args.output == "json":
        print(json.dumps(result.to_dict(), indent=2, sort_keys=True))
    else:
        print(format_table(result))

    return 0 if result.passed else 1


if __name__ == "__main__":
    sys.exit(main())
