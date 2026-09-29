// Copyright (c) 2026 The Raptoreum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// F-221: a wire command string longer than CMessageHeader::COMMAND_SIZE makes
// EVERY attempt to construct that message's header abort -- CMessageHeader's
// own constructor (protocol.cpp:170-181) copies at most COMMAND_SIZE bytes
// and then asserts the source string was already null-terminated by that
// point (`assert(pszCommand[i] == 0)`, protocol.cpp:177). NetMsgType::
// SENDCOMMITMENTS = "sendcommitments" is 15 bytes against a 12-byte
// COMMAND_SIZE and shipped past this undetected because no prior test in
// this tree ever exercised two NODE_COMMITMENTS-capable peers connecting to
// each other: ShouldNegotiateCommitments only ever sends this message when
// BOTH sides claim the bit (net_processing.cpp:3585), so every single-sided
// pairing in the existing F-155-220 corpus never actually calls
// CNetMsgMaker::Make(NetMsgType::SENDCOMMITMENTS) at all -- reproduced
// directly with two real raptoreumd processes, both -commitmentblocks=1,
// connected via addnode: both abort immediately and identically on
// protocol.cpp:177 (F-221).
//
// This test checks the same length invariant the real constructor enforces,
// without calling that constructor -- calling it with an oversized command
// would abort() the whole test binary, not just fail a BOOST_CHECK. Every
// current and future entry in getAllNetMessageTypes() is covered, so this
// is a general regression guard, not a SENDCOMMITMENTS-specific one.

#include <protocol.h>
#include <tinyformat.h>
#include <test/test_raptoreum.h>

#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(protocol_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(every_net_message_type_fits_command_size) {
    for (const std::string &cmd : getAllNetMessageTypes()) {
        BOOST_CHECK_MESSAGE(cmd.size() <= CMessageHeader::COMMAND_SIZE,
                             strprintf("NetMsgType \"%s\" is %d bytes, exceeds COMMAND_SIZE (%d) -- "
                                       "CMessageHeader's constructor will abort() the instant this "
                                       "message is ever sent (protocol.cpp:177, F-221)",
                                       cmd, cmd.size(), CMessageHeader::COMMAND_SIZE));
    }
}

BOOST_AUTO_TEST_SUITE_END()
