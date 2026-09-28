#!/usr/bin/env python3
# Copyright (c) 2026 The Raptoreum developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Minimal JSON-RPC client for talking to a raptoreumd node over HTTP.

F-215's own transport: deliberately small and stdlib-only (http.client,
json, base64) rather than importing test/functional/test_framework's
AuthServiceProxy, for two reasons. First, that framework is built around a
locally-launched regtest/testnet node addressed by --datadir (see
test/perf/fanout.py's own `rpc()` helper), while this script's whole job
(F-213 point 4) is querying several REMOTE, independently-operated
Smartnode-tier nodes by host:port -- a different shape of "how do I find
this node" entirely. Second, AuthServiceProxy keeps one persistent
http.client connection per instance and is not documented as safe for
concurrent calls from multiple threads on that one instance; this script
constructs a short-lived RpcClient per call (see check_body_range_coverage.py),
which is simplest to get right when a ThreadPoolExecutor is firing many
requests at once.

Authentication matches how raptoreum-cli itself authenticates (src/init.cpp's
-rpcuser/-rpcpassword/-rpccookiefile args): either a plain user/password pair
sent as HTTP Basic auth, or a cookie file (".cookie", format
"__cookie__:<hex>", written by the node itself at startup) whose contents
are used as the user:password pair the same way.
"""

from __future__ import annotations

import base64
import http.client
import json
import os
from dataclasses import dataclass
from typing import Any, List, Optional


class RpcError(Exception):
    """A JSON-RPC-level error response (node is reachable, method call itself
    failed) -- e.g. "Method not found" (-32601, the exact response a node
    that predates F-212's getbodyrangecoverageheights gives; confirmed
    against a real local mainnet node during F-215's own testing)."""

    def __init__(self, code: int, message: str):
        self.code = code
        self.message = message
        super().__init__("RPC error %d: %s" % (code, message))


class RpcTransportError(Exception):
    """The request never got a JSON-RPC response at all: connection refused,
    DNS failure, timeout, auth rejected at the HTTP layer, non-JSON body."""


@dataclass
class NodeConfig:
    """One configured node endpoint. Exactly one of (rpcuser and
    rpcpassword) or rpccookiefile must be set -- matches raptoreumd's own
    "cookie auth is used unless an explicit rpcuser is configured"
    precedent (src/init.cpp's -rpccookiefile/-rpcuser docs)."""

    name: str
    host: str
    port: int
    rpcuser: Optional[str] = None
    rpcpassword: Optional[str] = None
    rpccookiefile: Optional[str] = None
    use_https: bool = False
    timeout: float = 30.0

    def auth_pair(self) -> "tuple[str, str]":
        if self.rpccookiefile:
            with open(self.rpccookiefile, "r", encoding="utf-8") as f:
                cookie = f.read().strip()
            user, _, password = cookie.partition(":")
            if not _:
                raise ValueError("malformed cookie file %r (expected user:password)" % self.rpccookiefile)
            return user, password
        if self.rpcuser is not None and self.rpcpassword is not None:
            return self.rpcuser, self.rpcpassword
        raise ValueError(
            "node %r has neither rpccookiefile nor rpcuser/rpcpassword configured" % self.name
        )

    @classmethod
    def from_dict(cls, d: dict) -> "NodeConfig":
        required = {"name", "host", "port"}
        missing = required - set(d)
        if missing:
            raise ValueError("node config missing required field(s): %s" % ", ".join(sorted(missing)))
        return cls(
            name=d["name"],
            host=d["host"],
            port=int(d["port"]),
            rpcuser=d.get("rpcuser"),
            rpcpassword=d.get("rpcpassword"),
            rpccookiefile=d.get("rpccookiefile"),
            use_https=bool(d.get("use_https", False)),
            timeout=float(d.get("timeout", 30.0)),
        )


def load_node_configs(path: str) -> List[NodeConfig]:
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    if not isinstance(raw, list) or not raw:
        raise ValueError("%s must contain a non-empty JSON list of node configs" % path)
    configs = [NodeConfig.from_dict(entry) for entry in raw]
    names = [c.name for c in configs]
    if len(set(names)) != len(names):
        raise ValueError("duplicate node name(s) in %s: names must be unique" % path)
    return configs


class RpcClient:
    """One JSON-RPC call, one HTTP connection -- see this module's own
    top-of-file doc for why a persistent connection is deliberately not
    reused across concurrent calls here."""

    def __init__(self, node: NodeConfig):
        self.node = node

    def call(self, method: str, params: Optional[List[Any]] = None) -> Any:
        user, password = self.node.auth_pair()
        auth = base64.b64encode(("%s:%s" % (user, password)).encode("utf-8")).decode("ascii")
        payload = json.dumps({"jsonrpc": "1.0", "id": "coverage-monitor", "method": method, "params": params or []})

        conn_cls = http.client.HTTPSConnection if self.node.use_https else http.client.HTTPConnection
        conn = conn_cls(self.node.host, self.node.port, timeout=self.node.timeout)
        try:
            conn.request(
                "POST",
                "/",
                payload.encode("utf-8"),
                {
                    "Content-Type": "application/json",
                    "Authorization": "Basic %s" % auth,
                },
            )
            resp = conn.getresponse()
            body = resp.read()
        except (OSError, http.client.HTTPException) as e:
            raise RpcTransportError("%s:%d %s: %s" % (self.node.host, self.node.port, method, e)) from e
        finally:
            conn.close()

        if resp.status == 401:
            raise RpcTransportError("%s:%d %s: HTTP 401 Unauthorized (bad rpcuser/rpcpassword or cookie)" % (self.node.host, self.node.port, method))

        try:
            decoded = json.loads(body.decode("utf-8"))
        except ValueError as e:
            raise RpcTransportError(
                "%s:%d %s: non-JSON HTTP %d response: %r" % (self.node.host, self.node.port, method, resp.status, body[:200])
            ) from e

        if decoded.get("error"):
            err = decoded["error"]
            raise RpcError(err.get("code", -1), err.get("message", str(err)))
        if "result" not in decoded:
            raise RpcTransportError("%s:%d %s: response had neither result nor error: %r" % (self.node.host, self.node.port, method, decoded))
        return decoded["result"]
