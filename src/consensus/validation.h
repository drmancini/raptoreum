// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_VALIDATION_H
#define BITCOIN_CONSENSUS_VALIDATION_H

#include <string>

/** "reject" message codes */
static const unsigned char REJECT_MALFORMED = 0x01;
static const unsigned char REJECT_INVALID = 0x10;
static const unsigned char REJECT_OBSOLETE = 0x11;
static const unsigned char REJECT_DUPLICATE = 0x12;
static const unsigned char REJECT_NONSTANDARD = 0x40;
// static const unsigned char REJECT_DUST = 0x41; // part of BIP 61
static const unsigned char REJECT_INSUFFICIENTFEE = 0x42;
static const unsigned char REJECT_CHECKPOINT = 0x43;

/** Capture information about block/transaction validation */
class CValidationState {
private:
    enum mode_state {
        MODE_VALID,   //!< everything ok
        MODE_INVALID, //!< network rule violation (DoS value may be set)
        MODE_ERROR,   //!< run-time error
    } mode;
    int nDoS;
    std::string strRejectReason;
    unsigned int chRejectCode;
    bool corruptionPossible;
    bool bodiesMissing;
    std::string strDebugMessage;
public:
    CValidationState() : mode(MODE_VALID), nDoS(0), chRejectCode(0), corruptionPossible(false),
                         bodiesMissing(false) {}

    bool DoS(int level, bool ret = false,
             unsigned int chRejectCodeIn = 0, const std::string &strRejectReasonIn = "",
             bool corruptionIn = false,
             const std::string &strDebugMessageIn = "") {
        chRejectCode = chRejectCodeIn;
        strRejectReason = strRejectReasonIn;
        corruptionPossible = corruptionIn;
        strDebugMessage = strDebugMessageIn;
        if (mode == MODE_ERROR)
            return ret;
        nDoS += level;
        mode = MODE_INVALID;
        return ret;
    }

    bool Invalid(bool ret = false,
                 unsigned int _chRejectCode = 0, const std::string &_strRejectReason = "",
                 const std::string &_strDebugMessage = "") {
        return DoS(0, ret, _chRejectCode, _strRejectReason, false, _strDebugMessage);
    }

    bool Error(const std::string &strRejectReasonIn) {
        if (mode == MODE_VALID)
            strRejectReason = strRejectReasonIn;
        mode = MODE_ERROR;
        return false;
    }

    bool IsValid() const {
        return mode == MODE_VALID;
    }

    bool IsInvalid() const {
        return mode == MODE_INVALID;
    }

    bool IsError() const {
        return mode == MODE_ERROR;
    }

    bool IsInvalid(int &nDoSOut) const {
        if (IsInvalid()) {
            nDoSOut = nDoS;
            return true;
        }
        return false;
    }

    bool CorruptionPossible() const {
        return corruptionPossible;
    }

    /** Set when a block could not be connected because its transaction bodies
     *  are not held -- the decoupling design's third outcome, beside "valid"
     *  and "invalid".
     *
     *  It exists because the two existing outcomes are both wrong for it.
     *  Marking the block invalid would permanently reject a chain other nodes
     *  accept. Treating it as a run-time error makes it fatal: ThreadImport
     *  shuts the node down on any failed ActivateBestChain, which is measured
     *  behaviour -- the probe's node logged "holding it incomplete" and then
     *  "Failed to connect best block ( (code 0))" and exited, because an empty
     *  state is indistinguishable from a disk failure.
     *
     *  So the signal is carried rather than inferred, on the same pattern
     *  corruptionPossible already uses: a non-consensus reason travelling with
     *  the state so callers can tell one kind of "no" from another. */
    void SetBodiesMissing() {
        bodiesMissing = true;
    }

    bool BodiesMissing() const {
        return bodiesMissing;
    }

    unsigned int GetRejectCode() const { return chRejectCode; }

    std::string GetRejectReason() const { return strRejectReason; }

    std::string GetDebugMessage() const { return strDebugMessage; }
};

#endif // BITCOIN_CONSENSUS_VALIDATION_H
