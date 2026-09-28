#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Integration-shaped tests for check_body_range_coverage.py's orchestration
(F-215): default end-height resolution, per-node clamping to that node's own
tip, pagination across chunks, and end-to-end pass/fail/error exit codes --
all against a fake RpcClient.call, no real node required.
"""

import json
import os
import tempfile
import unittest
from unittest import mock

import check_body_range_coverage as cbrc
from rpc_client import NodeConfig, RpcError, RpcTransportError


def _height_entries(heights_to_status):
    return [{"height": h, "status": s, "firstobserved": 0, "lastobserved": 0, "lastchange": 0} for h, s in heights_to_status.items()]


class FakeRpc:
    """Stands in for RpcClient: maps node name -> {method: canned behavior}.
    A behavior is either a plain return value, or a callable(params) ->
    value, or an exception instance/class to raise.
    """

    def __init__(self, script):
        self.script = script  # {node_name: {method: behavior}}
        self.calls = []

    def __call__(self, node):
        return _FakeClientForNode(self, node)


class _FakeClientForNode:
    def __init__(self, fake_rpc, node):
        self.fake_rpc = fake_rpc
        self.node = node

    def call(self, method, params=None):
        self.fake_rpc.calls.append((self.node.name, method, params))
        behavior = self.fake_rpc.script[self.node.name][method]
        if isinstance(behavior, Exception):
            raise behavior
        if isinstance(behavior, type) and issubclass(behavior, Exception):
            raise behavior("boom")
        if callable(behavior) and not isinstance(behavior, list):
            return behavior(params)
        return behavior


def _node(name):
    return NodeConfig(name=name, host="127.0.0.1", port=10225, rpcuser="u", rpcpassword="p")


class RunOrchestrationTest(unittest.TestCase):
    def _patch_rpc(self, script):
        fake = FakeRpc(script)
        return mock.patch.object(cbrc, "RpcClient", side_effect=fake), fake

    def test_default_end_height_is_min_tip_across_nodes(self):
        script = {
            "a": {"getblockcount": 100, "getbodyrangecoverageheights": lambda p: _height_entries({h: "full" for h in range(p[0], p[1] + 1)})},
            "b": {"getblockcount": 50, "getbodyrangecoverageheights": lambda p: _height_entries({h: "full" for h in range(p[0], p[1] + 1)})},
        }
        patcher, fake = self._patch_rpc(script)
        with patcher:
            result = cbrc.run([_node("a"), _node("b")], start_height=None, end_height=None, chunk_size=1000)
        self.assertEqual(result.end_height, 50)
        self.assertEqual(result.start_height, 0)
        self.assertTrue(result.passed)

    def test_height_beyond_a_nodes_tip_is_not_reported_as_that_nodes_gap(self):
        # node a's tip is 5; an explicit end_height of 10 should not make
        # heights 6-10 look like a's retention gap.
        script = {
            "a": {"getblockcount": 5, "getbodyrangecoverageheights": lambda p: _height_entries({h: "full" for h in range(p[0], p[1] + 1)})},
            "b": {"getblockcount": 10, "getbodyrangecoverageheights": lambda p: _height_entries({h: "full" for h in range(p[0], p[1] + 1)})},
        }
        patcher, fake = self._patch_rpc(script)
        with patcher:
            result = cbrc.run([_node("a"), _node("b")], start_height=0, end_height=10, chunk_size=1000)
        self.assertTrue(result.passed, result.to_dict())
        # a should never have been asked for heights past its own tip
        a_calls = [c for c in fake.calls if c[0] == "a" and c[1] == "getbodyrangecoverageheights"]
        for _, _, params in a_calls:
            self.assertLessEqual(params[1], 5)

    def test_pagination_across_multiple_chunks_finds_a_gap_in_a_later_chunk(self):
        # Range 0..9, chunk_size 3 => chunks (0,2)(3,5)(6,8)(9,9). Put the
        # one miss in the last chunk to prove chunking doesn't drop data.
        def responder(params):
            statuses = {h: "full" for h in range(params[0], params[1] + 1)}
            if 9 in statuses:
                statuses[9] = "miss"
            return _height_entries(statuses)

        script = {"a": {"getblockcount": 9, "getbodyrangecoverageheights": responder}}
        patcher, fake = self._patch_rpc(script)
        with patcher:
            result = cbrc.run([_node("a")], start_height=0, end_height=9, chunk_size=3)
        self.assertFalse(result.passed)
        self.assertEqual(len(result.gaps), 1)
        self.assertEqual(result.gaps[0].height, 9)
        chunk_calls = [p for (_, m, p) in fake.calls if m == "getbodyrangecoverageheights"]
        self.assertEqual(sorted(chunk_calls), [[0, 2], [3, 5], [6, 8], [9, 9]])

    def test_rpc_error_on_one_node_marks_error_not_pass(self):
        script = {
            "a": {"getblockcount": 10, "getbodyrangecoverageheights": lambda p: _height_entries({h: "full" for h in range(p[0], p[1] + 1)})},
            "b": {"getblockcount": RpcTransportError("connection refused")},
        }
        patcher, fake = self._patch_rpc(script)
        with patcher:
            result = cbrc.run([_node("a"), _node("b")], start_height=0, end_height=None, chunk_size=1000)
        # b contributed no tip, so end_height falls back to a's tip (10);
        # b's own range query should then also fail cleanly.
        self.assertFalse(result.passed)
        self.assertIn("b", result.node_errors)

    def test_method_not_found_on_all_nodes_is_error_not_silent_pass(self):
        # Matches what F-215's own manual check found against a real local
        # mainnet node still running a pre-F-212 build.
        script = {
            "a": {"getblockcount": 10, "getbodyrangecoverageheights": RpcError(-32601, "Method not found")},
        }
        patcher, fake = self._patch_rpc(script)
        with patcher:
            result = cbrc.run([_node("a")], start_height=0, end_height=10, chunk_size=1000)
        self.assertFalse(result.passed)
        self.assertIn("a", result.node_errors)
        self.assertIn("Method not found", result.node_errors["a"])


class MainExitCodeTest(unittest.TestCase):
    def _write_config(self, nodes):
        f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump(nodes, f)
        f.close()
        return f.name

    def test_main_exits_zero_on_pass(self):
        script = {"a": {"getblockcount": 3, "getbodyrangecoverageheights": lambda p: _height_entries({h: "full" for h in range(p[0], p[1] + 1)})}}
        fake = FakeRpc(script)
        config_path = self._write_config([{"name": "a", "host": "127.0.0.1", "port": 10225, "rpcuser": "u", "rpcpassword": "p"}])
        try:
            with mock.patch.object(cbrc, "RpcClient", side_effect=fake):
                rc = cbrc.main(["--config", config_path, "--output", "json"])
        finally:
            os.unlink(config_path)
        self.assertEqual(rc, 0)

    def test_main_exits_nonzero_on_gap(self):
        script = {"a": {"getblockcount": 3, "getbodyrangecoverageheights": lambda p: _height_entries({h: "miss" for h in range(p[0], p[1] + 1)})}}
        fake = FakeRpc(script)
        config_path = self._write_config([{"name": "a", "host": "127.0.0.1", "port": 10225, "rpcuser": "u", "rpcpassword": "p"}])
        try:
            with mock.patch.object(cbrc, "RpcClient", side_effect=fake):
                rc = cbrc.main(["--config", config_path])
        finally:
            os.unlink(config_path)
        self.assertEqual(rc, 1)

    def test_main_exits_2_on_bad_config(self):
        rc = cbrc.main(["--config", "/nonexistent/path.json"])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
