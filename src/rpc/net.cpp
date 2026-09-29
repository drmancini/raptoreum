// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2019 The Dash Core developers
// Copyright (c) 2020-2023 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>

#include <banman.h>
#include <chainparams.h>
#include <clientversion.h>
#include <core_io.h>
#include <net.h>
#include <net_processing.h>
#include <netbase.h>
#include <policy/policy.h>
#include <node/context.h>
#include <policy/settings.h>
#include <rpc/blockchain.h>
#include <rpc/protocol.h>
#include <rpc/util.h>
#include <sync.h>
#include <timedata.h>
#include <ui_interface.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <validation.h>
#include <version.h>
#include <warnings.h>

#include <univalue.h>

UniValue getconnectioncount(const JSONRPCRequest &request) {
    RPCHelpMan{"getconnectioncount",
               "\nReturns the number of connections to other nodes.\n",
               {},
               RPCResult{
                       RPCResult::Type::NUM, "", "The connection count"},
               RPCExamples{
                       HelpExampleCli("getconnectioncount", "")
                       + HelpExampleRpc("getconnectioncount", "")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    return (int) node.connman->GetNodeCount(CConnman::CONNECTIONS_ALL);
}

UniValue ping(const JSONRPCRequest &request) {
    RPCHelpMan{"ping",
               "\nRequests that a ping be sent to all other nodes, to measure ping time.\n"
               "Results provided in getpeerinfo, pingtime and pingwait fields are decimal seconds.\n"
               "Ping command is handled in queue with all other commands, so it measures processing backlog, not just network ping.\n",
               {},
               RPCResult{RPCResult::Type::NONE, "", ""},
               RPCExamples{
                       HelpExampleCli("ping", "")
                       + HelpExampleRpc("ping", "")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    // Request that each node send a ping during next message processing pass
    node.connman->ForEachNode([](CNode *pnode) {
        pnode->fPingQueued = true;
    });
    return NullUniValue;
}

UniValue getpeerinfo(const JSONRPCRequest &request) {
    RPCHelpMan{"getpeerinfo",
               "\nReturns data about each connected network node as a json array of objects.\n",
               {},
               RPCResult{
                       RPCResult::Type::ARR, "", "",
                       {
                               {RPCResult::Type::OBJ, "", "",
                                {
                                        {
                                                {RPCResult::Type::NUM, "id", "Peer index"},
                                                {RPCResult::Type::STR, "addr",
                                                 "(host:port) The IP address and port of the peer"},
                                                {RPCResult::Type::STR, "addrlocal",
                                                 "(ip:port) Local address as reported by the peer"},
                                                {RPCResult::Type::STR, "addrbind",
                                                 "(ip:port) Bind address of the connection to the peer"},
                                                {RPCResult::Type::STR, "mapped_as",
                                                 "The AS in the BGP route to the peer used for diversifying peer selection"},
                                                {RPCResult::Type::STR_HEX, "services", "The services offered"},
                                                {RPCResult::Type::STR_HEX, "verified_proregtx_hash", true /*optional*/,
                                                 "Only present when the peer is a smartnode and successfully "
                                                 "authenticated via MNAUTH. In this case, this field contains the "
                                                 "protx hash of the smartnode"},
                                                {RPCResult::Type::STR_HEX, "verified_pubkey_hash", true /*optional*/,
                                                 "Only present when the peer is a smartnode and successfully "
                                                 "authenticated via MNAUTH. In this case, this field contains the "
                                                 "hash of the smartnode's operator public key"},
                                                {RPCResult::Type::ARR, "servicesnames",
                                                 "the services offered, in human-readable form",
                                                 {
                                                         {RPCResult::Type::STR, "SERVICE_NAME",
                                                          "the service name if it is recognised"}
                                                 }},
                                                {RPCResult::Type::BOOL, "relaytxes",
                                                 "Whether peer has asked us to relay transactions to it"},
                                                {RPCResult::Type::NUM_TIME, "lastsend",
                                                 "The " + UNIX_EPOCH_TIME + " of the last send"},
                                                {RPCResult::Type::NUM_TIME, "lastrecv",
                                                 "The " + UNIX_EPOCH_TIME + " of the last receive"},
                                                {RPCResult::Type::NUM, "bytessent", "The total bytes sent"},
                                                {RPCResult::Type::NUM, "bytesrecv", "The total bytes received"},
                                                {RPCResult::Type::NUM_TIME, "conntime",
                                                 "The " + UNIX_EPOCH_TIME + " of the connection"},
                                                {RPCResult::Type::NUM, "timeoffset", "The time offset in seconds"},
                                                {RPCResult::Type::NUM, "pingtime", "ping time (if available)"},
                                                {RPCResult::Type::NUM, "minping",
                                                 "minimum observed ping time (if any at all)"},
                                                {RPCResult::Type::NUM, "pingwait", "ping wait (if non-zero)"},
                                                {RPCResult::Type::NUM, "version", "The peer version, such as 70001"},
                                                {RPCResult::Type::STR, "subver", "The string version"},
                                                {RPCResult::Type::BOOL, "inbound",
                                                 "Inbound (true) or Outbound (false)"},
                                                {RPCResult::Type::BOOL, "addnode",
                                                 "Whether connection was due to addnode/-connect or if it was an automatic/inbound connection"},
                                                {RPCResult::Type::BOOL, "smartnode",
                                                 "Whether connection was due to smartnode connection attempt"},
                                                {RPCResult::Type::NUM, "startingheight",
                                                 "The starting height (block) of the peer"},
                                                {RPCResult::Type::NUM, "banscore", "The ban score"},
                                                {RPCResult::Type::NUM, "synced_headers",
                                                 "The last header we have in common with this peer"},
                                                {RPCResult::Type::NUM, "synced_blocks",
                                                 "The last block we have in common with this peer"},
                                                {RPCResult::Type::ARR, "inflight", "",
                                                 {
                                                         {RPCResult::Type::NUM, "n",
                                                          "The heights of blocks we're currently asking from this peer"},
                                                 }},
                                                {RPCResult::Type::NUM, "bodyrange_hits", true /*optional*/,
                                                 "4.2 (F-206): validated GETBODYRANGE responses from this peer that "
                                                 "were not empty, since the connection was established. Only present "
                                                 "once at least one response has been observed."},
                                                {RPCResult::Type::NUM, "bodyrange_misses", true /*optional*/,
                                                 "4.2 (F-206): validated GETBODYRANGE responses from this peer that "
                                                 "were empty (an honest miss -- withheld or never had it), since the "
                                                 "connection was established. Only present once at least one response "
                                                 "has been observed."},
                                                {RPCResult::Type::NUM, "bodyrange_missrate", true /*optional*/,
                                                 "bodyrange_misses / (bodyrange_hits + bodyrange_misses). Only present "
                                                 "once at least one response has been observed -- a 0/0 rate would be "
                                                 "indistinguishable from '0% miss rate' rather than 'never asked'."},
                                                {RPCResult::Type::BOOL, "whitelisted",
                                                 "Whether the peer is whitelisted"},
                                                {RPCResult::Type::OBJ_DYN, "bytessent_per_msg", "",
                                                 {
                                                         {RPCResult::Type::NUM, "msg",
                                                          "The total bytes sent aggregated by message type\n"
                                                          "When a message type is not listed in this json object, the bytes sent are 0.\n"
                                                          "Only known message types can appear as keys in the object."}
                                                 }},
                                                {RPCResult::Type::OBJ, "bytesrecv_per_msg", "",
                                                 {
                                                         {RPCResult::Type::NUM, "msg",
                                                          "The total bytes received aggregated by message type\n"
                                                          "When a message type is not listed in this json object, the bytes received are 0.\n"
                                                          "Only known message types can appear as keys in the object and all bytes received of unknown message types are listed under '" +
                                                          NET_MESSAGE_COMMAND_OTHER + "'."}
                                                 }},
                                        }},
                               }}},
               RPCExamples{
                       HelpExampleCli("getpeerinfo", "")
                       + HelpExampleRpc("getpeerinfo", "")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    std::vector <CNodeStats> vstats;
    node.connman->GetNodeStats(vstats);

    UniValue ret(UniValue::VARR);

    for (const CNodeStats &stats: vstats) {
        UniValue obj(UniValue::VOBJ);
        CNodeStateStats statestats;
        bool fStateStats = GetNodeStateStats(stats.nodeid, statestats);
        obj.pushKV("id", stats.nodeid);
        obj.pushKV("addr", stats.addrName);
        if (!(stats.addrLocal.empty()))
            obj.pushKV("addrlocal", stats.addrLocal);
        if (stats.addrBind.IsValid())
            obj.pushKV("addrbind", stats.addrBind.ToString());
        if (stats.m_mapped_as != 0) {
            obj.pushKV("mapped_as", uint64_t(stats.m_mapped_as));
        }
        obj.pushKV("services", strprintf("%016x", stats.nServices));
        if (!stats.verifiedProRegTxHash.IsNull()) {
            obj.pushKV("verified_proregtx_hash", stats.verifiedProRegTxHash.ToString());
        }
        if (!stats.verifiedPubKeyHash.IsNull()) {
            obj.pushKV("verified_pubkey_hash", stats.verifiedPubKeyHash.ToString());
        }
        obj.pushKV("servicesnames", GetServicesNames(stats.nServices));
        obj.pushKV("relaytxes", stats.fRelayTxes);
        obj.pushKV("lastsend", stats.nLastSend);
        obj.pushKV("lastrecv", stats.nLastRecv);
        obj.pushKV("bytessent", stats.nSendBytes);
        obj.pushKV("bytesrecv", stats.nRecvBytes);
        obj.pushKV("conntime", stats.nTimeConnected);
        obj.pushKV("timeoffset", stats.nTimeOffset);
        if (stats.m_ping_usec > 0) {
            obj.pushKV("pingtime", ((double) stats.m_ping_usec) / 1e6);
        }
        if (stats.m_min_ping_usec < std::numeric_limits<int64_t>::max()) {
            obj.pushKV("minping", ((double) stats.m_min_ping_usec) / 1e6);
        }
        if (stats.m_ping_wait_usec > 0) {
            obj.pushKV("pingwait", ((double) stats.m_ping_wait_usec) / 1e6);
        }
        obj.pushKV("version", stats.nVersion);
        // Use the sanitized form of subver here, to avoid tricksy remote peers from
        // corrupting or modifying the JSON output by putting special characters in
        // their ver message.
        obj.pushKV("subver", stats.cleanSubVer);
        obj.pushKV("inbound", stats.fInbound);
        obj.pushKV("addnode", stats.m_manual_connection);
        obj.pushKV("smartnode", stats.m_smartnode_connection);
        obj.pushKV("startingheight", stats.nStartingHeight);
        if (fStateStats) {
            obj.pushKV("banscore", statestats.nMisbehavior);
            obj.pushKV("synced_headers", statestats.nSyncHeight);
            obj.pushKV("synced_blocks", statestats.nCommonHeight);
            UniValue heights(UniValue::VARR);
            for (const int height: statestats.vHeightInFlight) {
                heights.push_back(height);
            }
            obj.pushKV("inflight", heights);
            // 4.2 (F-206): per-peer body-range miss-rate telemetry -- omit
            // entirely rather than emit a misleading 0/0, matching
            // pingtime's own "if available" convention above.
            uint64_t nBodyRangeTotal = statestats.nBodyRangeHits + statestats.nBodyRangeMisses;
            if (nBodyRangeTotal > 0) {
                obj.pushKV("bodyrange_hits", statestats.nBodyRangeHits);
                obj.pushKV("bodyrange_misses", statestats.nBodyRangeMisses);
                obj.pushKV("bodyrange_missrate",
                           (double) statestats.nBodyRangeMisses / (double) nBodyRangeTotal);
            }
        }
        obj.pushKV("whitelisted", stats.fWhitelisted);
        obj.pushKV("addr_processed", stats.nAddrProcessed);
        obj.pushKV("addr_rate_limited", stats.nAddrRateLimited);

        UniValue sendPerMsgCmd(UniValue::VOBJ);
        for (const mapMsgCmdSize::value_type &i: stats.mapSendBytesPerMsgCmd) {
            if (i.second > 0)
                sendPerMsgCmd.pushKV(i.first, i.second);
        }
        obj.pushKV("bytessent_per_msg", sendPerMsgCmd);

        UniValue recvPerMsgCmd(UniValue::VOBJ);
        for (const mapMsgCmdSize::value_type &i: stats.mapRecvBytesPerMsgCmd) {
            if (i.second > 0)
                recvPerMsgCmd.pushKV(i.first, i.second);
        }
        obj.pushKV("bytesrecv_per_msg", recvPerMsgCmd);

        ret.push_back(obj);
    }

    return ret;
}

UniValue addnode(const JSONRPCRequest &request) {
    std::string strCommand;
    if (!request.params[1].isNull())
        strCommand = request.params[1].get_str();
    RPCHelpMan{"addnode",
               "\nAttempts to add or remove a node from the addnode list.\n"
               "Or try a connection to a node once.\n"
               "Nodes added using addnode (or -connect) are protected from DoS disconnection and are not required to be\n"
               "full nodes as other outbound peers are (though such peers will not be synced from).\n",
               {
                       {"node", RPCArg::Type::STR, RPCArg::Optional::NO, "The node (see getpeerinfo for nodes)"},
                       {"command", RPCArg::Type::STR, RPCArg::Optional::NO,
                        "'add' to add a node to the list, 'remove' to remove a node from the list, 'onetry' to try a connection to the node once"},
               },
               RPCResult{RPCResult::Type::NONE, "", ""},
               RPCExamples{
                       HelpExampleCli("addnode", "\"192.168.0.6:9999\" \"onetry\"")
                       + HelpExampleRpc("addnode", "\"192.168.0.6:9999\", \"onetry\"")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    std::string strNode = request.params[0].get_str();

    if (strCommand == "onetry") {
        CAddress addr;
        node.connman->OpenNetworkConnection(addr, false, nullptr, strNode.c_str(), false, false, true);
        return NullUniValue;
    }

    if (strCommand == "add") {
        if (!node.connman->AddNode(strNode))
            throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: Node already added");
    } else if (strCommand == "remove") {
        if (!node.connman->RemoveAddedNode(strNode))
            throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node has not been added.");
    }

    return NullUniValue;
}

UniValue disconnectnode(const JSONRPCRequest &request) {
    RPCHelpMan{"disconnectnode",
               "\nImmediately disconnects from the specified peer node.\n"
               "\nStrictly one out of 'address' and 'nodeid' can be provided to identify the node.\n"
               "\nTo disconnect by nodeid, either set 'address' to the empty string, or call using the named 'nodeid' argument only.\n",
               {
                       {"address", RPCArg::Type::STR, /* default */ "fallback to nodeid",
                        "The IP address/port of the node"},
                       {"nodeid", RPCArg::Type::NUM, /* default */ "fallback to address",
                        "The node ID (see getpeerinfo for node IDs)"},
               },
               RPCResult{RPCResult::Type::NONE, "", ""},
               RPCExamples{
                       HelpExampleCli("disconnectnode", "\"192.168.0.6:9999\"")
                       + HelpExampleCli("disconnectnode", "\"\" 1")
                       + HelpExampleRpc("disconnectnode", "\"192.168.0.6:9999\"")
                       + HelpExampleRpc("disconnectnode", "\"\", 1")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    bool success;
    const UniValue &address_arg = request.params[0];
    const UniValue &id_arg = request.params[1];

    if (!address_arg.isNull() && id_arg.isNull()) {
        /* handle disconnect-by-address */
        success = node.connman->DisconnectNode(address_arg.get_str());
    } else if (!id_arg.isNull() && (address_arg.isNull() || (address_arg.isStr() && address_arg.get_str().empty()))) {
        /* handle disconnect-by-id */
        NodeId nodeid = (NodeId) id_arg.get_int64();
        success = node.connman->DisconnectNode(nodeid);
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Only one of address and nodeid should be provided.");
    }

    if (!success) {
        throw JSONRPCError(RPC_CLIENT_NODE_NOT_CONNECTED, "Node not found in connected nodes");
    }

    return NullUniValue;
}

UniValue getaddednodeinfo(const JSONRPCRequest &request) {
    RPCHelpMan{"getaddednodeinfo",
               "\nReturns information about the given added node, or all added nodes\n"
               "(note that onetry addnodes are not listed here)\n",
               {
                       {"node", RPCArg::Type::STR, /* default */ "all nodes",
                        "If provided, return information about this specific node, otherwise all nodes are returned."},
               },
               RPCResult{
                       RPCResult::Type::ARR, "", "",
                       {
                               {RPCResult::Type::OBJ, "", "",
                                {
                                        {RPCResult::Type::STR, "addednode",
                                         "The node IP address or name (as provided to addnode)"},
                                        {RPCResult::Type::BOOL, "connected", "If connected"},
                                        {RPCResult::Type::ARR, "addresses", "Only when connected = true",
                                         {
                                                 {RPCResult::Type::OBJ, "", "",
                                                  {
                                                          {RPCResult::Type::STR, "address",
                                                           "The Raptoreum server IP and port we're connected to"},
                                                          {RPCResult::Type::STR, "connected",
                                                           "connection, inbound or outbound"},
                                                  }},
                                         }},
                                }},
                       }
               },
               RPCExamples{
                       HelpExampleCli("getaddednodeinfo", "\"192.168.0.201\"")
                       + HelpExampleRpc("getaddednodeinfo", "\"192.168.0.201\"")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    std::vector <AddedNodeInfo> vInfo = node.connman->GetAddedNodeInfo();

    if (!request.params[0].isNull()) {
        bool found = false;
        for (const AddedNodeInfo &info: vInfo) {
            if (info.strAddedNode == request.params[0].get_str()) {
                vInfo.assign(1, info);
                found = true;
                break;
            }
        }
        if (!found) {
            throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node has not been added.");
        }
    }

    UniValue ret(UniValue::VARR);

    for (const AddedNodeInfo &info: vInfo) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("addednode", info.strAddedNode);
        obj.pushKV("connected", info.fConnected);
        UniValue addresses(UniValue::VARR);
        if (info.fConnected) {
            UniValue address(UniValue::VOBJ);
            address.pushKV("address", info.resolvedAddress.ToString());
            address.pushKV("connected", info.fInbound ? "inbound" : "outbound");
            addresses.push_back(address);
        }
        obj.pushKV("addresses", addresses);
        ret.push_back(obj);
    }

    return ret;
}

UniValue getnettotals(const JSONRPCRequest &request) {
    RPCHelpMan{"getnettotals",
               "\nReturns information about network traffic, including bytes in, bytes out,\n"
               "and current time.\n",
               {},
               RPCResult{
                       RPCResult::Type::OBJ, "", "",
                       {
                               {RPCResult::Type::NUM, "totalbytesrecv", "Total bytes received"},
                               {RPCResult::Type::NUM, "totalbytessent", "Total bytes sent"},
                               {RPCResult::Type::NUM_TIME, "timemillis", "Current UNIX time in milliseconds"},
                               {RPCResult::Type::OBJ, "uploadtarget", "",
                                {
                                        {RPCResult::Type::NUM, "timeframe",
                                         "Length of the measuring timeframe in seconds"},
                                        {RPCResult::Type::NUM, "target", "Target in bytes"},
                                        {RPCResult::Type::BOOL, "target_reached", "True if target is reached"},
                                        {RPCResult::Type::BOOL, "serve_historical_blocks",
                                         "True if serving historical blocks"},
                                        {RPCResult::Type::NUM, "bytes_left_in_cycle",
                                         "Bytes left in current time cycle"},
                                        {RPCResult::Type::NUM, "time_left_in_cycle",
                                         "Seconds left in current time cycle"},
                                }},
                       }
               },
               RPCExamples{
                       HelpExampleCli("getnettotals", "")
                       + HelpExampleRpc("getnettotals", "")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("totalbytesrecv", node.connman->GetTotalBytesRecv());
    obj.pushKV("totalbytessent", node.connman->GetTotalBytesSent());
    obj.pushKV("timemillis", GetTimeMillis());

    UniValue outboundLimit(UniValue::VOBJ);
    outboundLimit.pushKV("timeframe", node.connman->GetMaxOutboundTimeframe());
    outboundLimit.pushKV("target", node.connman->GetMaxOutboundTarget());
    outboundLimit.pushKV("target_reached", node.connman->OutboundTargetReached(false));
    outboundLimit.pushKV("serve_historical_blocks", !node.connman->OutboundTargetReached(true));
    outboundLimit.pushKV("bytes_left_in_cycle", node.connman->GetOutboundTargetBytesLeft());
    outboundLimit.pushKV("time_left_in_cycle", node.connman->GetMaxOutboundTimeLeftInCycle());
    obj.pushKV("uploadtarget", outboundLimit);
    return obj;
}

// 4.2 (F-206, build-plan.md's 4.2 row; docs/transaction-decoupling.md
// SS14.5/SS14.9); F-212 (this rework): the RPC surface for per-range coverage
// telemetry -- the query SS14.5's activation criterion (4.6) or an operator
// needs, matching this file's own established getpeerinfo/getnettotals shape
// (a plain data dump, no gating logic, no I/O beyond the in-memory map).
// Read-only: consensus-adjacency caution (this phase's own scope limit) means
// this must never be able to influence validation, and it does not -- it
// only reads mapBodyRangeCoverage back through GetBodyRangeCoverageStats.
//
// F-212's own output rework: "hits"/"misses"/"lasthit"/"lastmiss" are gone,
// replaced by "full"/"partial"/"misses" (distinct HEIGHT counts, deduped --
// see coveragetelemetry.h's HeightCoverageStatus) and a single "lastchange"
// (the most recent status UPGRADE in this bucket, coveragetelemetry.h's own
// CoverageHeightRecord::nLastChangeTime doc). This is a deliberate breaking
// change to a pre-production RPC (no external consumer exists yet) rather
// than a field bolted on beside numbers the rework proved were miscounted.
UniValue getbodyrangecoverage(const JSONRPCRequest &request) {
    RPCHelpMan{"getbodyrangecoverage",
               "\n4.2/F-212: passive per-range body coverage telemetry (docs/transaction-decoupling.md\n"
               "SS14.9). For every block-height bucket this node has at least one OBSERVED\n"
               "height in: how many distinct heights in that bucket reached each coverage\n"
               "level (misses-only, first-chunk-only/\"partial\", full-body-reconstructed/\"full\"),\n"
               "deduped per height -- a height retried many times before succeeding counts once,\n"
               "at its best-ever status, never once per attempt. A query and a counter (SS14.5),\n"
               "not a classifier -- telling honest loss (oldest-first, file-aligned, scattered, or\n"
               "a shrinking gap) apart from deliberate erasure (contiguous, mid-history, aligned to\n"
               "nothing, stable) is left to a caller polling this repeatedly and reading the shape;\n"
               "this RPC does not judge it, and a bucket rollup alone cannot show WHERE within the\n"
               "bucket the gaps are -- see getbodyrangecoverageheights for that detail.\n"
               "Buckets with zero observed heights are omitted.\n",
               {},
               RPCResult{
                       RPCResult::Type::OBJ, "", "",
                       {
                               {RPCResult::Type::NUM, "rangesize",
                                "The width, in blocks, of one coverage bucket"},
                               {RPCResult::Type::ARR, "ranges", "",
                                {
                                        {RPCResult::Type::OBJ, "", "",
                                         {
                                                 {RPCResult::Type::NUM, "startheight",
                                                  "First height in this bucket"},
                                                 {RPCResult::Type::NUM, "endheight",
                                                  "Last height in this bucket (inclusive)"},
                                                 {RPCResult::Type::NUM, "full",
                                                  "Distinct heights in this bucket whose full body was confirmed reconstructed at least once"},
                                                 {RPCResult::Type::NUM, "partial",
                                                  "Distinct heights in this bucket seen (first chunk validated) but never confirmed fully reconstructed"},
                                                 {RPCResult::Type::NUM, "misses",
                                                  "Distinct heights in this bucket with only miss responses, never any positive evidence"},
                                                 {RPCResult::Type::NUM_TIME, "firstobserved",
                                                  "The " + UNIX_EPOCH_TIME + " the earliest height in this bucket was first observed"},
                                                 {RPCResult::Type::NUM_TIME, "lastchange",
                                                  "The " + UNIX_EPOCH_TIME + " of the most recent status upgrade among heights in this bucket, 0 if none"},
                                         }},
                                }},
                       }
               },
               RPCExamples{
                       HelpExampleCli("getbodyrangecoverage", "")
                       + HelpExampleRpc("getbodyrangecoverage", "")
               },
    }.Check(request);

    std::vector <BodyRangeCoverageEntry> vStats;
    GetBodyRangeCoverageStats(vStats);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("rangesize", COVERAGE_RANGE_SIZE);

    UniValue ranges(UniValue::VARR);
    for (const BodyRangeCoverageEntry &entry: vStats) {
        UniValue r(UniValue::VOBJ);
        r.pushKV("startheight", entry.nRangeStartHeight);
        r.pushKV("endheight", entry.nRangeEndHeightInclusive);
        r.pushKV("full", entry.stats.nFull);
        r.pushKV("partial", entry.stats.nPartial);
        r.pushKV("misses", entry.stats.nMissOnly);
        // Recorded internally in microseconds (GetTimeMicros(), matching
        // this codebase's own body-range staleness/backoff convention,
        // net_processing.cpp) -- reported here in whole seconds, matching
        // every other NUM_TIME field in this file (e.g. pingtime's own
        // "/1e6" conversion above).
        r.pushKV("firstobserved", entry.stats.nFirstObservedTime / 1000000);
        r.pushKV("lastchange", entry.stats.nLastChangeTime / 1000000);
        ranges.push_back(r);
    }
    obj.pushKV("ranges", ranges);
    return obj;
}

// F-212 (gap 1's own shape-detail escape hatch): the per-height counterpart
// to getbodyrangecoverage above -- a bucket rollup cannot tell a contiguous
// erasure apart from scattered corruption (SS14.9's own example table), so
// this returns the raw, deduped per-height records a caller needs to see
// that shape for itself. Bounded by required start/end params, matching
// this file's own general convention of never letting a plain data-read RPC
// do unbounded work.
UniValue getbodyrangecoverageheights(const JSONRPCRequest &request) {
    RPCHelpMan{"getbodyrangecoverageheights",
               "\nF-212: per-height coverage detail for a bounded height range -- the raw,\n"
               "deduped record behind getbodyrangecoverage's own bucket rollup, for a caller\n"
               "that needs to see WHICH heights in a range are covered and which are not (e.g.\n"
               "to tell a 300-block contiguous erasure apart from 300 scattered corrupt blocks,\n"
               "docs/transaction-decoupling.md SS14.9's own example -- both roll up to the same\n"
               "bucket-level miss count). Only heights with at least one observation are\n"
               "returned; an absent height in the requested range has never been observed at\n"
               "all (SS14.9's own \"never asked\" case, distinct from a genuine miss).\n",
               {
                       {"start_height", RPCArg::Type::NUM, RPCArg::Optional::NO,
                        "First height to query (inclusive)"},
                       {"end_height", RPCArg::Type::NUM, RPCArg::Optional::NO,
                        "Last height to query (inclusive)"},
               },
               RPCResult{
                       RPCResult::Type::ARR, "", "",
                       {
                               {RPCResult::Type::OBJ, "", "",
                                {
                                        {RPCResult::Type::NUM, "height", "The block height"},
                                        {RPCResult::Type::STR, "status",
                                         "One of \"miss\", \"partial\" (first chunk seen, never confirmed fully reconstructed), or \"full\" (fully reconstructed at least once)"},
                                        {RPCResult::Type::NUM_TIME, "firstobserved",
                                         "The " + UNIX_EPOCH_TIME + " this height was first observed"},
                                        {RPCResult::Type::NUM_TIME, "lastobserved",
                                         "The " + UNIX_EPOCH_TIME + " of the most recent observation of any kind for this height"},
                                        {RPCResult::Type::NUM_TIME, "lastchange",
                                         "The " + UNIX_EPOCH_TIME + " of the most recent status upgrade for this height"},
                                }},
                       }
               },
               RPCExamples{
                       HelpExampleCli("getbodyrangecoverageheights", "100000 100999")
                       + HelpExampleRpc("getbodyrangecoverageheights", "100000, 100999")
               },
    }.Check(request);

    const int nStartHeight = request.params[0].get_int();
    const int nEndHeightInclusive = request.params[1].get_int();
    if (nEndHeightInclusive < nStartHeight) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "end_height must be >= start_height");
    }

    std::vector <BodyRangeCoverageHeightEntry> vHeights;
    GetBodyRangeCoverageHeights(nStartHeight, nEndHeightInclusive, vHeights);

    UniValue ranges(UniValue::VARR);
    for (const BodyRangeCoverageHeightEntry &entry: vHeights) {
        UniValue r(UniValue::VOBJ);
        r.pushKV("height", entry.nHeight);
        switch (entry.status) {
            case HeightCoverageStatus::FULL:
                r.pushKV("status", "full");
                break;
            case HeightCoverageStatus::PARTIAL:
                r.pushKV("status", "partial");
                break;
            case HeightCoverageStatus::MISS:
            case HeightCoverageStatus::NOT_OBSERVED:
            default:
                r.pushKV("status", "miss");
                break;
        }
        r.pushKV("firstobserved", entry.nFirstObservedTime / 1000000);
        r.pushKV("lastobserved", entry.nLastObservedTime / 1000000);
        r.pushKV("lastchange", entry.nLastChangeTime / 1000000);
        ranges.push_back(r);
    }
    return ranges;
}

// 2.4b (build-plan.md's 2.4 row, F-219): an operator-invokable, read-only
// consistency check for the body store's own bdy*.dat bytes against the
// block's own committed identifiers -- matching this file's own established
// getbodyrangecoverageheights precedent (F-212) for exposing an internal
// consistency mechanism via RPC. Deliberately read-only: it never mutates
// pindex/BLOCK_HAVE_BODIES or the body-store index -- see
// VerifyBodyRecordAtRest's own doc (validation.h) for why repair on a
// running node is judged unsafe and left to CVerifyDB::VerifyDB's own
// startup-time, fail-closed wiring (-checkblocks/-checklevel, no new flag)
// instead. Bounded by required start/end params, matching
// getbodyrangecoverageheights's own convention of never letting a plain
// data-read RPC do unbounded work -- a full-history scan re-hashes every
// stored body, genuinely expensive at this project's own target scale, so an
// operator must explicitly opt into the range they want checked.
UniValue verifybodystore(const JSONRPCRequest &request) {
    RPCHelpMan{"verifybodystore",
               "\n2.4b (F-219): verify the body store's own on-disk bytes (bodystore.h's\n"
               "bdy*.dat series) still hash-match what each block's own commitment block\n"
               "commits to, for every height in the given range that this node currently\n"
               "holds bodies for. Read-only -- detects, does not repair; a mismatch means\n"
               "this node's local copy of that block's body is corrupt or was never written\n"
               "correctly, and should not be trusted or served until the datadir is\n"
               "rebuilt (e.g. -reindex, which re-derives the body store from blk*.dat).\n"
               "A height this node has no bodies for at all (never synced, pruned, or a\n"
               "block still commitment-only) is reported as \"not-held\", not an error.\n",
               {
                       {"start_height", RPCArg::Type::NUM, RPCArg::Optional::NO,
                        "First height to check (inclusive)"},
                       {"end_height", RPCArg::Type::NUM, RPCArg::Optional::NO,
                        "Last height to check (inclusive)"},
               },
               RPCResult{
                       RPCResult::Type::OBJ, "", "",
                       {
                               {RPCResult::Type::NUM, "checked", "Heights checked whose bodies were actually held"},
                               {RPCResult::Type::NUM, "ok", "Heights that verified clean"},
                               {RPCResult::Type::NUM, "not_held", "Heights this node holds no bodies for (skipped, not an error)"},
                               {RPCResult::Type::ARR, "mismatches", "Every height that failed verification",
                                {
                                        {RPCResult::Type::OBJ, "", "",
                                         {
                                                 {RPCResult::Type::NUM, "height", "The block height"},
                                                 {RPCResult::Type::STR_HEX, "hash", "The block hash"},
                                                 {RPCResult::Type::STR, "reason",
                                                  "Why verification failed -- commitments-unreadable, body-unreadable, count-mismatch, or hash-mismatch"},
                                         }},
                                }},
                       }
               },
               RPCExamples{
                       HelpExampleCli("verifybodystore", "100000 100999")
                       + HelpExampleRpc("verifybodystore", "100000, 100999")
               },
    }.Check(request);

    const int nStartHeight = request.params[0].get_int();
    const int nEndHeightInclusive = request.params[1].get_int();
    if (nEndHeightInclusive < nStartHeight) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "end_height must be >= start_height");
    }

    int nChecked = 0, nOk = 0, nNotHeld = 0;
    UniValue mismatches(UniValue::VARR);
    for (int height = nStartHeight; height <= nEndHeightInclusive; height++) {
        CBlockIndex *pindex;
        {
            LOCK(cs_main);
            pindex = ::ChainActive()[height];
        }
        if (pindex == nullptr) {
            continue;
        }
        BodyRecordVerification result = VerifyBodyRecordAtRest(pindex, Params().GetConsensus());
        if (result == BodyRecordVerification::NOT_HELD) {
            nNotHeld++;
            continue;
        }
        nChecked++;
        if (result == BodyRecordVerification::OK) {
            nOk++;
            continue;
        }
        // At minimum: log it clearly (2.4's own stated requirement) -- an
        // operator running this RPC unattended (e.g. a periodic health
        // check) must not depend on reading the RPC's own return value to
        // learn their local history has a real hole.
        LogPrintf("verifybodystore: *** body-store record at rest failed verification at height %d, hash=%s (%s)\n",
                  height, pindex->GetBlockHash().ToString(), BodyRecordVerificationToString(result));
        UniValue m(UniValue::VOBJ);
        m.pushKV("height", height);
        m.pushKV("hash", pindex->GetBlockHash().GetHex());
        m.pushKV("reason", BodyRecordVerificationToString(result));
        mismatches.push_back(m);
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("checked", nChecked);
    obj.pushKV("ok", nOk);
    obj.pushKV("not_held", nNotHeld);
    obj.pushKV("mismatches", mismatches);
    return obj;
}

static UniValue GetNetworksInfo() {
    UniValue networks(UniValue::VARR);
    for (int n = 0; n < NET_MAX; ++n) {
        enum Network network = static_cast<enum Network>(n);
        if (network == NET_UNROUTABLE || network == NET_INTERNAL)
            continue;
        proxyType proxy;
        UniValue obj(UniValue::VOBJ);
        GetProxy(network, proxy);
        obj.pushKV("name", GetNetworkName(network));
        obj.pushKV("limited", IsLimited(network));
        obj.pushKV("reachable", IsReachable(network));
        obj.pushKV("proxy", proxy.IsValid() ? proxy.proxy.ToStringIPPort() : std::string());
        obj.pushKV("proxy_randomize_credentials", proxy.randomize_credentials);
        networks.push_back(obj);
    }
    return networks;
}

UniValue getnetworkinfo(const JSONRPCRequest &request) {
    RPCHelpMan{"getnetworkinfo",
               "Returns an object containing various state info regarding P2P networking.\n",
               {},
               RPCResult{
                       RPCResult::Type::OBJ, "", "",
                       {
                               {RPCResult::Type::NUM, "version", "the server version"},
                               {RPCResult::Type::STR, "buildversion",
                                "the server build version including RC info or commit as relevant"},
                               {RPCResult::Type::STR, "subversion", "the server subversion string"},
                               {RPCResult::Type::NUM, "protocolversion", "the protocol version"},
                               {RPCResult::Type::STR_HEX, "localservices", "the services we offer to the network"},
                               {RPCResult::Type::ARR, "localservicesnames",
                                "the services we offer to the network, in human-readable form",
                                {
                                        {RPCResult::Type::STR, "SERVICE_NAME", "the service name"},
                                }},
                               {RPCResult::Type::BOOL, "localrelay",
                                "true if transaction relay is requested from peers"},
                               {RPCResult::Type::NUM, "timeoffset", "the time offset"},
                               {RPCResult::Type::NUM, "connections", "the number of connections"},
                               {RPCResult::Type::BOOL, "networkactive", "whether p2p networking is enabled"},
                               {RPCResult::Type::STR, "socketevents",
                                "the socket events mode, either kqueue, epoll, poll or select"},
                               {RPCResult::Type::ARR, "networks", "information per network",
                                {
                                        {RPCResult::Type::OBJ, "", "",
                                         {
                                                 {RPCResult::Type::STR, "name", "network (ipv4, ipv6 or onion)"},
                                                 {RPCResult::Type::BOOL, "limited",
                                                  "is the network limited using -onlynet?"},
                                                 {RPCResult::Type::BOOL, "reachable", "is the network reachable?"},
                                                 {RPCResult::Type::STR, "proxy",
                                                  "(\"host:port\") the proxy that is used for this network, or empty if none"},
                                                 {RPCResult::Type::BOOL, "proxy_randomize_credentials",
                                                  "Whether randomized credentials are used"},
                                         }},
                                }},
                               {RPCResult::Type::NUM, "relayfee",
                                "minimum relay fee for transactions in " + CURRENCY_UNIT + "/kB"},
                               {RPCResult::Type::NUM, "incrementalfee",
                                "minimum fee increment for mempool limiting in " + CURRENCY_UNIT + "/kB"},
                               {RPCResult::Type::ARR, "localaddresses", "list of local addresses",
                                {
                                        {RPCResult::Type::OBJ, "", "",
                                         {
                                                 {RPCResult::Type::STR, "address", "network address"},
                                                 {RPCResult::Type::NUM, "port", "network port"},
                                                 {RPCResult::Type::NUM, "score", "relative score"},
                                         }},
                                }},
                               {RPCResult::Type::STR, "warnings", "any network and blockchain warnings"},
                       }
               },
               RPCExamples{
                       HelpExampleCli("getnetworkinfo", "")
                       + HelpExampleRpc("getnetworkinfo", "")
               },
    }.Check(request);

    LOCK(cs_main);
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("version", CLIENT_VERSION);
    obj.pushKV("buildversion", FormatFullVersion());
    obj.pushKV("subversion", strSubVersion);
    obj.pushKV("protocolversion", PROTOCOL_VERSION);
    NodeContext &node = EnsureNodeContext(request.context);
    if (node.connman) {
        ServiceFlags services = node.connman->GetLocalServices();
        obj.pushKV("localservices", strprintf("%016x", services));
        obj.pushKV("localservicesnames", GetServicesNames(services));
    }
    obj.pushKV("localrelay", fRelayTxes);
    obj.pushKV("timeoffset", GetTimeOffset());
    if (node.connman) {
        obj.pushKV("networkactive", node.connman->GetNetworkActive());
        obj.pushKV("connections", (int) node.connman->GetNodeCount(CConnman::CONNECTIONS_ALL));
        std::string strSocketEvents;
        switch (node.connman->GetSocketEventsMode()) {
            case CConnman::SOCKETEVENTS_SELECT:
                strSocketEvents = "select";
                break;
            case CConnman::SOCKETEVENTS_POLL:
                strSocketEvents = "poll";
                break;
            case CConnman::SOCKETEVENTS_EPOLL:
                strSocketEvents = "epoll";
                break;
            case CConnman::SOCKETEVENTS_KQUEUE:
                strSocketEvents = "kqueue";
                break;
            default:
                assert(false);
        }
        obj.pushKV("socketevents", strSocketEvents);
    }
    obj.pushKV("networks", GetNetworksInfo());
    obj.pushKV("relayfee", ValueFromAmount(::minRelayTxFee.GetFeePerK()));
    obj.pushKV("incrementalfee", ValueFromAmount(::incrementalRelayFee.GetFeePerK()));
    UniValue localAddresses(UniValue::VARR);
    {
        LOCK(cs_mapLocalHost);
        for (const std::pair<const CNetAddr, LocalServiceInfo> &item: mapLocalHost) {
            UniValue rec(UniValue::VOBJ);
            rec.pushKV("address", item.first.ToString());
            rec.pushKV("port", item.second.nPort);
            rec.pushKV("score", item.second.nScore);
            localAddresses.push_back(rec);
        }
    }
    obj.pushKV("localaddresses", localAddresses);
    obj.pushKV("warnings", GetWarnings(false));
    return obj;
}

UniValue setban(const JSONRPCRequest &request) {
    const RPCHelpMan help{"setban",
                          "\nAttempts to add or remove an IP/Subnet from the banned list.\n",
                          {
                                  {"subnet", RPCArg::Type::STR, RPCArg::Optional::NO,
                                   "The IP/Subnet (see getpeerinfo for nodes IP) with an optional netmask (default is /32 = single IP)"},
                                  {"command", RPCArg::Type::STR, RPCArg::Optional::NO,
                                   "'add' to add an IP/Subnet to the list, 'remove' to remove an IP/Subnet from the list"},
                                  {"bantime", RPCArg::Type::NUM, /* default */ "0",
                                   "time in seconds how long (or until when if [absolute] is set) the IP is banned (0 or empty means using the default time of 24h which can also be overwritten by the -bantime startup argument)"},
                                  {"absolute", RPCArg::Type::BOOL, /* default */ "false",
                                   "If set, the bantime must be an absolute timestamp expressed in " + UNIX_EPOCH_TIME},
                          },
                          RPCResult{RPCResult::Type::NONE, "", ""},
                          RPCExamples{
                                  HelpExampleCli("setban", "\"192.168.0.6\" \"add\" 86400")
                                  + HelpExampleCli("setban", "\"192.168.0.0/24\" \"add\"")
                                  + HelpExampleRpc("setban", "\"192.168.0.6\", \"add\", 86400")
                          },
    };

    std::string strCommand;
    if (!request.params[1].isNull())
        strCommand = request.params[1].get_str();
    if (request.fHelp || !help.IsValidNumArgs(request.params.size()) ||
        (strCommand != "add" && strCommand != "remove")) {
        throw std::runtime_error(help.ToString());
    }
    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.banman) {
        throw JSONRPCError(RPC_DATABASE_ERROR, "Error: Ban database not loaded");
    }

    CSubNet subNet;
    CNetAddr netAddr;
    bool isSubnet = false;

    if (request.params[0].get_str().find('/') != std::string::npos)
        isSubnet = true;

    if (!isSubnet) {
        CNetAddr resolved;
        LookupHost(request.params[0].get_str().c_str(), resolved, false);
        netAddr = resolved;
    } else
        LookupSubNet(request.params[0].get_str().c_str(), subNet);

    if (!(isSubnet ? subNet.IsValid() : netAddr.IsValid()))
        throw JSONRPCError(RPC_CLIENT_INVALID_IP_OR_SUBNET, "Error: Invalid IP/Subnet");

    if (strCommand == "add") {
        if (isSubnet ? node.banman->IsBanned(subNet) : node.banman->IsBanned(netAddr)) {
            throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: IP/Subnet already banned");
        }

        int64_t banTime = 0; //use standard bantime if not specified
        if (!request.params[2].isNull())
            banTime = request.params[2].get_int64();

        bool absolute = false;
        if (request.params[3].isTrue())
            absolute = true;

        if (isSubnet) {
            node.banman->Ban(subNet, BanReasonManuallyAdded, banTime, absolute);
            if (node.connman) {
                node.connman->DisconnectNode(subNet);
            }
        } else {
            node.banman->Ban(netAddr, BanReasonManuallyAdded, banTime, absolute);
            if (node.connman) {
                node.connman->DisconnectNode(netAddr);
            }
        }
    } else if (strCommand == "remove") {
        if (!(isSubnet ? node.banman->Unban(subNet) : node.banman->Unban(netAddr))) {
            throw JSONRPCError(RPC_CLIENT_INVALID_IP_OR_SUBNET,
                               "Error: Unban failed. Requested address/subnet was not previously banned.");
        }
    }
    return NullUniValue;
}

UniValue listbanned(const JSONRPCRequest &request) {
    RPCHelpMan{"listbanned",
               "\nList all manually banned IPs/Subnets.\n",
               {},
               RPCResult{RPCResult::Type::ARR, "", "",
                         {
                                 {RPCResult::Type::OBJ, "", "",
                                  {
                                          {RPCResult::Type::STR, "address", ""},
                                          {RPCResult::Type::NUM_TIME, "banned_until", ""},
                                          {RPCResult::Type::NUM_TIME, "ban_created", ""},
                                  }},
                         }},
               RPCExamples{
                       HelpExampleCli("listbanned", "")
                       + HelpExampleRpc("listbanned", "")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.banman) {
        throw JSONRPCError(RPC_DATABASE_ERROR, "Error: Ban database not loaded");
    }

    banmap_t banMap;
    node.banman->GetBanned(banMap);

    UniValue bannedAddresses(UniValue::VARR);
    for (const auto &entry: banMap) {
        const CBanEntry &banEntry = entry.second;
        UniValue rec(UniValue::VOBJ);
        rec.pushKV("address", entry.first.ToString());
        rec.pushKV("banned_until", banEntry.nBanUntil);
        rec.pushKV("ban_created", banEntry.nCreateTime);

        bannedAddresses.push_back(rec);
    }

    return bannedAddresses;
}

UniValue clearbanned(const JSONRPCRequest &request) {
    RPCHelpMan{"clearbanned",
               "\nClear all banned IPs.\n",
               {},
               RPCResult{RPCResult::Type::NONE, "", ""},
               RPCExamples{
                       HelpExampleCli("clearbanned", "")
                       + HelpExampleRpc("clearbanned", "")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.banman) {
        throw JSONRPCError(RPC_DATABASE_ERROR, "Error: Ban database not loaded");
    }

    node.banman->ClearBanned();

    return NullUniValue;
}

UniValue setnetworkactive(const JSONRPCRequest &request) {
    RPCHelpMan{"setnetworkactive",
               "\nDisable/enable all p2p network activity.\n",
               {
                       {"state", RPCArg::Type::BOOL, RPCArg::Optional::NO,
                        "true to enable networking, false to disable"},
               },
               RPCResult{RPCResult::Type::BOOL, "", "The value that was passed in"},
               RPCExamples{""},
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.banman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    node.connman->SetNetworkActive(request.params[0].get_bool());

    return node.connman->GetNetworkActive();
}

static UniValue getnodeaddresses(const JSONRPCRequest &request) {
    RPCHelpMan{"getnodeaddresses",
               "\nReturn known addresses which can potentially be used to find new nodes in the network\n",
               {
                       {"count", RPCArg::Type::NUM, /* default */ "1",
                        "How many addresses to return. Limited to the smaller of " +
                        std::to_string(ADDRMAN_GETADDR_MAX) + " or " + std::to_string(ADDRMAN_GETADDR_MAX_PCT) +
                        "% of all known addresses."},
               },
               RPCResult{
                       RPCResult::Type::ARR, "", "",
                       {
                               {RPCResult::Type::OBJ, "", "",
                                {
                                        {RPCResult::Type::NUM_TIME, "time",
                                         "The " + UNIX_EPOCH_TIME + " of when the node was last seen"},
                                        {RPCResult::Type::NUM, "services", "The services offered"},
                                        {RPCResult::Type::STR, "address", "The address of the node"},
                                        {RPCResult::Type::NUM, "port", "The port of the node"},
                                }},
                       }
               },
               RPCExamples{
                       HelpExampleCli("getnodeaddresses", "8")
                       + HelpExampleRpc("getnodeaddresses", "8")
               },
    }.Check(request);

    NodeContext &node = EnsureNodeContext(request.context);
    if (!node.banman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    int count = 1;
    if (!request.params[0].isNull()) {
        count = request.params[0].get_int();
        if (count <= 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Address count out of range");
        }
    }
    // returns a shuffled list of CAddress
    std::vector <CAddress> vAddr = node.connman->GetAddresses();
    UniValue ret(UniValue::VARR);

    int address_return_count = std::min<int>(count, vAddr.size());
    for (int i = 0; i < address_return_count; ++i) {
        UniValue obj(UniValue::VOBJ);
        const CAddress &addr = vAddr[i];
        obj.pushKV("time", (int) addr.nTime);
        obj.pushKV("services", (uint64_t) addr.nServices);
        obj.pushKV("address", addr.ToStringIP());
        obj.pushKV("port", addr.GetPort());
        ret.push_back(obj);
    }
    return ret;
}

// clang-format off
static const CRPCCommand commands[] =
        { //  category              name                      actor (function)         argNames
                //  --------------------- ------------------------  -----------------------  ----------
                {"network", "getconnectioncount", &getconnectioncount, {}},
                {"network", "ping",               &ping,               {}},
                {"network", "getpeerinfo",        &getpeerinfo,        {}},
                {"network", "addnode",            &addnode,            {"node",    "command"}},
                {"network", "disconnectnode",     &disconnectnode,     {"address", "nodeid"}},
                {"network", "getaddednodeinfo",   &getaddednodeinfo,   {"node"}},
                {"network", "getnettotals",       &getnettotals,       {}},
                {"network", "getbodyrangecoverage", &getbodyrangecoverage, {}},
                {"network", "getbodyrangecoverageheights", &getbodyrangecoverageheights, {"start_height", "end_height"}},
                {"network", "verifybodystore",     &verifybodystore,   {"start_height", "end_height"}},
                {"network", "getnetworkinfo",     &getnetworkinfo,     {}},
                {"network", "setban",             &setban,             {"subnet",  "command", "bantime", "absolute"}},
                {"network", "listbanned",         &listbanned,         {}},
                {"network", "clearbanned",        &clearbanned,        {}},
                {"network", "setnetworkactive",   &setnetworkactive,   {"state"}},
                {"network", "getnodeaddresses",   &getnodeaddresses,   {"count"}},
        };

void RegisterNetRPCCommands(CRPCTable &t) {
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
