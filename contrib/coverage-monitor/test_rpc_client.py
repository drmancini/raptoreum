#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Unit tests for rpc_client.py (F-215): node config parsing/auth selection
and JSON-RPC response handling, with the actual HTTP connection mocked out
(unittest.mock) -- no real node required. A real local mainnet node was also
used for a manual, read-only sanity check (see F-215's docs/findings.md
entry); that is deliberately not re-run here since automated tests must not
depend on a specific machine's local daemon being up."""

import json
import os
import tempfile
import unittest
from unittest import mock

from rpc_client import (
    NodeConfig,
    RpcClient,
    RpcError,
    RpcTransportError,
    load_node_configs,
)


class NodeConfigAuthTest(unittest.TestCase):
    def test_userpass_auth(self):
        n = NodeConfig(name="a", host="1.2.3.4", port=10225, rpcuser="u", rpcpassword="p")
        self.assertEqual(n.auth_pair(), ("u", "p"))

    def test_cookie_auth(self):
        with tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write("__cookie__:deadbeef")
            path = f.name
        try:
            n = NodeConfig(name="a", host="1.2.3.4", port=10225, rpccookiefile=path)
            self.assertEqual(n.auth_pair(), ("__cookie__", "deadbeef"))
        finally:
            os.unlink(path)

    def test_no_auth_configured_raises(self):
        n = NodeConfig(name="a", host="1.2.3.4", port=10225)
        with self.assertRaises(ValueError):
            n.auth_pair()

    def test_malformed_cookie_file_raises(self):
        with tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write("not-a-cookie-file")
            path = f.name
        try:
            n = NodeConfig(name="a", host="1.2.3.4", port=10225, rpccookiefile=path)
            with self.assertRaises(ValueError):
                n.auth_pair()
        finally:
            os.unlink(path)


class LoadNodeConfigsTest(unittest.TestCase):
    def _write(self, obj):
        f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump(obj, f)
        f.close()
        return f.name

    def test_loads_multiple_nodes(self):
        path = self._write([
            {"name": "a", "host": "1.1.1.1", "port": 10225, "rpcuser": "u", "rpcpassword": "p"},
            {"name": "b", "host": "2.2.2.2", "port": 10225, "rpccookiefile": "/x/.cookie"},
        ])
        try:
            configs = load_node_configs(path)
            self.assertEqual([c.name for c in configs], ["a", "b"])
            self.assertEqual(configs[0].rpcuser, "u")
            self.assertEqual(configs[1].rpccookiefile, "/x/.cookie")
        finally:
            os.unlink(path)

    def test_rejects_empty_list(self):
        path = self._write([])
        try:
            with self.assertRaises(ValueError):
                load_node_configs(path)
        finally:
            os.unlink(path)

    def test_rejects_duplicate_names(self):
        path = self._write([
            {"name": "a", "host": "1.1.1.1", "port": 10225, "rpcuser": "u", "rpcpassword": "p"},
            {"name": "a", "host": "2.2.2.2", "port": 10225, "rpcuser": "u", "rpcpassword": "p"},
        ])
        try:
            with self.assertRaises(ValueError):
                load_node_configs(path)
        finally:
            os.unlink(path)

    def test_rejects_missing_required_field(self):
        path = self._write([{"name": "a", "host": "1.1.1.1"}])
        try:
            with self.assertRaises(ValueError):
                load_node_configs(path)
        finally:
            os.unlink(path)


def _mock_http_response(status, body_dict_or_bytes):
    if isinstance(body_dict_or_bytes, (dict, list)):
        body = json.dumps(body_dict_or_bytes).encode("utf-8")
    else:
        body = body_dict_or_bytes
    resp = mock.Mock()
    resp.status = status
    resp.read.return_value = body
    return resp


class RpcClientCallTest(unittest.TestCase):
    def _client(self):
        node = NodeConfig(name="a", host="127.0.0.1", port=10225, rpcuser="u", rpcpassword="p")
        return RpcClient(node)

    @mock.patch("rpc_client.http.client.HTTPConnection")
    def test_successful_call_returns_result(self, mock_conn_cls):
        mock_conn = mock_conn_cls.return_value
        mock_conn.getresponse.return_value = _mock_http_response(
            200, {"result": [{"height": 1, "status": "full"}], "error": None, "id": "coverage-monitor"}
        )
        client = self._client()
        result = client.call("getbodyrangecoverageheights", [1, 1])
        self.assertEqual(result, [{"height": 1, "status": "full"}])

    @mock.patch("rpc_client.http.client.HTTPConnection")
    def test_sends_basic_auth_header(self, mock_conn_cls):
        mock_conn = mock_conn_cls.return_value
        mock_conn.getresponse.return_value = _mock_http_response(200, {"result": [], "error": None})
        self._client().call("getbodyrangecoverageheights", [1, 1])
        args, kwargs = mock_conn.request.call_args
        headers = args[3] if len(args) > 3 else kwargs.get("headers") or args[-1]
        self.assertIn("Authorization", headers)
        self.assertTrue(headers["Authorization"].startswith("Basic "))

    @mock.patch("rpc_client.http.client.HTTPConnection")
    def test_json_rpc_error_raises_rpc_error(self, mock_conn_cls):
        mock_conn = mock_conn_cls.return_value
        mock_conn.getresponse.return_value = _mock_http_response(
            200, {"result": None, "error": {"code": -32601, "message": "Method not found"}}
        )
        with self.assertRaises(RpcError) as ctx:
            self._client().call("getbodyrangecoverageheights", [1, 1])
        self.assertEqual(ctx.exception.code, -32601)

    @mock.patch("rpc_client.http.client.HTTPConnection")
    def test_http_401_raises_transport_error(self, mock_conn_cls):
        mock_conn = mock_conn_cls.return_value
        mock_conn.getresponse.return_value = _mock_http_response(401, b"Unauthorized")
        with self.assertRaises(RpcTransportError):
            self._client().call("getbodyrangecoverageheights", [1, 1])

    @mock.patch("rpc_client.http.client.HTTPConnection")
    def test_non_json_body_raises_transport_error(self, mock_conn_cls):
        mock_conn = mock_conn_cls.return_value
        mock_conn.getresponse.return_value = _mock_http_response(200, b"<html>not json</html>")
        with self.assertRaises(RpcTransportError):
            self._client().call("getbodyrangecoverageheights", [1, 1])

    @mock.patch("rpc_client.http.client.HTTPConnection")
    def test_connection_error_raises_transport_error(self, mock_conn_cls):
        mock_conn = mock_conn_cls.return_value
        mock_conn.getresponse.side_effect = ConnectionRefusedError("refused")
        with self.assertRaises(RpcTransportError):
            self._client().call("getbodyrangecoverageheights", [1, 1])

    @mock.patch("rpc_client.http.client.HTTPConnection")
    def test_connection_closed_after_call(self, mock_conn_cls):
        mock_conn = mock_conn_cls.return_value
        mock_conn.getresponse.return_value = _mock_http_response(200, {"result": [], "error": None})
        self._client().call("getbodyrangecoverageheights", [1, 1])
        mock_conn.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
