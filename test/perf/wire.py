"""Wire-format helpers shared by the corpus builder and the load generator.

Serialisation and signing come from the functional test framework so the bytes
are produced by the same code the node's own tests use.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "functional"))

from test_framework.messages import (  # noqa: E402
    COIN, COutPoint, CTransaction, CTxIn, CTxOut, hash256,
)
from test_framework.script import (  # noqa: E402
    CScript, OP_CHECKSIG, OP_DUP, OP_EQUALVERIFY, OP_HASH160, SIGHASH_ALL,
    SignatureHash, hash160,
)

MAGIC = {
    "regtest": b"\xfc\xc1\xb7\xdc",
    "testnet3": b"\xcc\xe2\xca\xff",
    "mainnet": b"\xb4\x6e\xce\xd7",
}

# Raptoreum packs nVersion and nType into one 32-bit word, low half then high
# half. A normal transaction is type 0, so the word is the version alone and the
# bytes match an ordinary v2 transaction.
TX_VERSION = 2


def frame(command, payload, net="regtest"):
    """A complete p2p message: magic, command, length, checksum, payload."""
    return (MAGIC[net]
            + command.ljust(12, b"\x00")
            + struct.pack("<I", len(payload))
            + hash256(payload)[:4]
            + payload)


def p2pkh_script(pubkey):
    return CScript([OP_DUP, OP_HASH160, hash160(pubkey), OP_EQUALVERIFY, OP_CHECKSIG])


def build_tx(inputs, outputs):
    """inputs: (txid_int, vout, amount, pubkey) — outputs: (pubkey, amount)."""
    tx = CTransaction()
    tx.nVersion = TX_VERSION
    for txid, vout, _amount, _pubkey in inputs:
        tx.vin.append(CTxIn(COutPoint(txid, vout), b"", 0xffffffff))
    for pubkey, amount in outputs:
        tx.vout.append(CTxOut(amount, p2pkh_script(pubkey)))
    return tx


def sign_tx(tx, inputs, signer):
    """Sign every input with SIGHASH_ALL. signer(privkey, msg32) -> DER bytes."""
    for i, (_txid, _vout, _amount, pubkey) in enumerate(inputs):
        spk = p2pkh_script(pubkey)
        sighash, err = SignatureHash(spk, tx, i, SIGHASH_ALL)
        assert err is None, err
        sig = signer(i, sighash) + bytes([SIGHASH_ALL])
        tx.vin[i].scriptSig = CScript([sig, pubkey])
    tx.rehash()
    return tx


def tx_message(tx, net="regtest"):
    """The framed p2p `tx` message for a signed transaction."""
    return frame(b"tx", tx.serialize(), net)
