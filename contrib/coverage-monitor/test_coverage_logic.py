#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Unit tests for coverage_logic.py (F-215).

Matches this repo's own contrib/devtools/test-security-check.py precedent:
stdlib unittest, no external test framework dependency (pytest is not
installed in this checkout -- confirmed before choosing). Runnable directly
(`python3 test_coverage_logic.py`) or via `python3 -m pytest` if pytest
happens to be available, since pytest collects unittest.TestCase classes
too.

No network, no mocked HTTP -- these tests exercise the pure logic
(chunking, response parsing, cross-node gap detection) against fixed,
hand-built inputs, matching this task's own ask ("given these three nodes'
responses, does it correctly flag this one gap").
"""

import unittest

from coverage_logic import (
    BEYOND_TIP,
    FULL,
    MISS,
    NODE_ERROR,
    NOT_OBSERVED,
    PARTIAL,
    NodeRangeResult,
    chunk_ranges,
    evaluate_range,
    format_table,
    parse_heights_response,
)


class ChunkRangesTest(unittest.TestCase):
    def test_exact_multiple(self):
        self.assertEqual(chunk_ranges(0, 9, 5), [(0, 4), (5, 9)])

    def test_remainder(self):
        self.assertEqual(chunk_ranges(0, 10, 5), [(0, 4), (5, 9), (10, 10)])

    def test_single_height(self):
        self.assertEqual(chunk_ranges(100, 100, 5), [(100, 100)])

    def test_chunk_larger_than_range(self):
        self.assertEqual(chunk_ranges(0, 3, 1000), [(0, 3)])

    def test_covers_every_height_no_overlap_no_gap(self):
        chunks = chunk_ranges(0, 999, 7)
        covered = []
        for start, end in chunks:
            self.assertLessEqual(start, end)
            covered.extend(range(start, end + 1))
        self.assertEqual(covered, list(range(0, 1000)))

    def test_rejects_bad_chunk_size(self):
        with self.assertRaises(ValueError):
            chunk_ranges(0, 10, 0)

    def test_rejects_inverted_range(self):
        with self.assertRaises(ValueError):
            chunk_ranges(10, 5, 100)


class ParseHeightsResponseTest(unittest.TestCase):
    def test_decoded_list(self):
        raw = [
            {"height": 100, "status": "full", "firstobserved": 1, "lastobserved": 2, "lastchange": 1},
            {"height": 101, "status": "partial", "firstobserved": 1, "lastobserved": 2, "lastchange": 1},
            {"height": 103, "status": "miss", "firstobserved": 1, "lastobserved": 2, "lastchange": 1},
        ]
        self.assertEqual(parse_heights_response(raw), {100: "full", 101: "partial", 103: "miss"})

    def test_json_string(self):
        raw = '[{"height": 5, "status": "full", "firstobserved": 0, "lastobserved": 0, "lastchange": 0}]'
        self.assertEqual(parse_heights_response(raw), {5: "full"})

    def test_absent_heights_are_simply_missing_from_the_map(self):
        # src/rpc/net.cpp:604-606: only observed heights are returned.
        raw = [{"height": 5, "status": "full", "firstobserved": 0, "lastobserved": 0, "lastchange": 0}]
        result = parse_heights_response(raw)
        self.assertNotIn(6, result)

    def test_rejects_unknown_status(self):
        raw = [{"height": 5, "status": "bogus", "firstobserved": 0, "lastobserved": 0, "lastchange": 0}]
        with self.assertRaises(ValueError):
            parse_heights_response(raw)

    def test_empty_result(self):
        self.assertEqual(parse_heights_response([]), {})


class EvaluateRangeTest(unittest.TestCase):
    def test_all_full_on_all_nodes_passes(self):
        nodes = [
            NodeRangeResult("a", heights={1: FULL, 2: FULL, 3: FULL}, tip_height=10),
            NodeRangeResult("b", heights={1: FULL, 2: FULL, 3: FULL}, tip_height=10),
            NodeRangeResult("c", heights={1: FULL, 2: FULL, 3: FULL}, tip_height=10),
        ]
        result = evaluate_range(nodes, 1, 3)
        self.assertTrue(result.passed)
        self.assertEqual(result.gaps, [])
        self.assertEqual(result.heights_checked, 3)

    def test_one_node_miss_at_one_height_is_flagged(self):
        nodes = [
            NodeRangeResult("a", heights={1: FULL, 2: FULL}, tip_height=10),
            NodeRangeResult("b", heights={1: FULL, 2: MISS}, tip_height=10),
        ]
        result = evaluate_range(nodes, 1, 2)
        self.assertFalse(result.passed)
        self.assertEqual(len(result.gaps), 1)
        gap = result.gaps[0]
        self.assertEqual(gap.height, 2)
        self.assertEqual(gap.statuses, {"a": FULL, "b": MISS})

    def test_partial_status_is_a_gap_not_a_pass(self):
        nodes = [NodeRangeResult("a", heights={5: PARTIAL}, tip_height=10)]
        result = evaluate_range(nodes, 5, 5)
        self.assertFalse(result.passed)
        self.assertEqual(result.gaps[0].statuses, {"a": PARTIAL})

    def test_unobserved_height_is_a_gap_not_silently_skipped(self):
        # F-213 point 4's own threshold makes no exception for "never asked".
        nodes = [NodeRangeResult("a", heights={5: FULL}, tip_height=10)]  # height 6 absent
        result = evaluate_range(nodes, 5, 6)
        self.assertFalse(result.passed)
        self.assertEqual(len(result.gaps), 1)
        self.assertEqual(result.gaps[0].height, 6)
        self.assertEqual(result.gaps[0].statuses, {"a": NOT_OBSERVED})

    def test_height_beyond_one_nodes_tip_is_not_a_gap(self):
        nodes = [
            NodeRangeResult("a", heights={100: FULL}, tip_height=100),
            NodeRangeResult("b", heights={100: FULL, 101: FULL}, tip_height=101),
        ]
        # node a's tip is 100, so height 101 is simply not there yet for a --
        # not a hole in its retained history.
        result = evaluate_range(nodes, 100, 101)
        self.assertTrue(result.passed)

    def test_beyond_tip_does_not_mask_a_real_gap_on_another_node(self):
        nodes = [
            NodeRangeResult("a", heights={100: FULL}, tip_height=100),
            NodeRangeResult("b", heights={100: FULL, 101: MISS}, tip_height=101),
        ]
        result = evaluate_range(nodes, 100, 101)
        self.assertFalse(result.passed)
        self.assertEqual(result.gaps[0].height, 101)
        self.assertEqual(result.gaps[0].statuses, {"a": BEYOND_TIP, "b": MISS})

    def test_node_error_fails_the_whole_run_even_with_zero_gaps_in_returned_data(self):
        nodes = [
            NodeRangeResult("a", heights={1: FULL, 2: FULL}, tip_height=10),
            NodeRangeResult("b", heights={1: FULL, 2: FULL}, tip_height=10, error="connection refused"),
        ]
        result = evaluate_range(nodes, 1, 2)
        self.assertFalse(result.passed)
        self.assertIn("b", result.node_errors)
        # still per-height informative, not silently dropped
        self.assertEqual(result.gaps[0].statuses["b"], NODE_ERROR)

    def test_multiple_gaps_all_reported(self):
        nodes = [
            NodeRangeResult("a", heights={1: FULL, 2: MISS, 3: FULL, 4: PARTIAL}, tip_height=10),
        ]
        result = evaluate_range(nodes, 1, 4)
        self.assertEqual([g.height for g in result.gaps], [2, 4])

    def test_to_dict_shape(self):
        nodes = [NodeRangeResult("a", heights={1: FULL}, tip_height=10)]
        result = evaluate_range(nodes, 1, 1)
        d = result.to_dict()
        self.assertEqual(d["pass"], True)
        self.assertEqual(d["gap_count"], 0)
        self.assertEqual(d["gaps"], [])
        self.assertEqual(d["node_errors"], {})
        self.assertEqual(d["heights_checked"], 1)


class FormatTableTest(unittest.TestCase):
    def test_pass_banner_says_pass(self):
        nodes = [NodeRangeResult("a", heights={1: FULL}, tip_height=10)]
        text = format_table(evaluate_range(nodes, 1, 1))
        self.assertIn("RESULT: PASS", text)
        self.assertNotIn("FAIL", text)
        self.assertNotIn("ERROR", text)

    def test_fail_banner_says_fail_and_lists_gap(self):
        nodes = [NodeRangeResult("a", heights={1: MISS}, tip_height=10)]
        text = format_table(evaluate_range(nodes, 1, 1))
        self.assertIn("RESULT: FAIL", text)
        self.assertIn("1: a=miss", text)

    def test_error_banner_says_error(self):
        nodes = [NodeRangeResult("a", heights={}, tip_height=10, error="timeout")]
        text = format_table(evaluate_range(nodes, 1, 1))
        self.assertIn("RESULT: ERROR", text)
        self.assertIn("timeout", text)


if __name__ == "__main__":
    unittest.main()
