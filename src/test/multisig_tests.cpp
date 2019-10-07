// Copyright (c) 2011-2013 The Bitcoin Core developers
// Copyright (c) 2015-2019 The Bitcoin Unlimited developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h" // Freeze CBitcoinAddress
#include "chain.h" // Freeze CBlockIndex
#include "dstencode.h"
#include "key.h"
#include "keystore.h"
#include "policy/policy.h"
#include "script/interpreter.h"
#include "script/ismine.h"
#include "script/script.h"
#include "script/script_error.h"
#include "script/sign.h"
#include "test/test_bitcoin.h"
#include "uint256.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h" // Freeze wallet test
#endif

#include <boost/test/unit_test.hpp>

using namespace std;

typedef vector<unsigned char> valtype;

// Append given push onto a script, using specific opcode (not necessarily
// the minimal one, but must be able to contain the given data.)
void AppendPush(CScript &script, opcodetype opcode, const std::vector<uint8_t> &b)
{
    assert(opcode <= OP_PUSHDATA4);
    script.push_back(opcode);
    if (opcode < OP_PUSHDATA1)
    {
        assert(b.size() == opcode);
    }
    else if (opcode == OP_PUSHDATA1)
    {
        assert(b.size() <= 0xff);
        script.push_back(uint8_t(b.size()));
    }
    else if (opcode == OP_PUSHDATA2)
    {
        assert(b.size() <= 0xffff);
        uint8_t _data[2];
        WriteLE16(_data, b.size());
        script.insert(script.end(), _data, _data + sizeof(_data));
    }
    else if (opcode == OP_PUSHDATA4)
    {
        uint8_t _data[4];
        WriteLE32(_data, b.size());
        script.insert(script.end(), _data, _data + sizeof(_data));
    }
    script.insert(script.end(), b.begin(), b.end());
}

BOOST_FIXTURE_TEST_SUITE(multisig_tests, BasicTestingSetup)

CScript sign_multisig(CScript scriptPubKey, vector<CKey> keys, CTransaction transaction, int whichIn)
{
    uint256 hash = SignatureHash(scriptPubKey, transaction, whichIn, SIGHASH_ALL | SIGHASH_FORKID, 0);
    BOOST_CHECK(hash != SIGNATURE_HASH_ERROR);

    CScript result;
    result << OP_0; // CHECKMULTISIG bug workaround
    for (const CKey &key : keys)
    {
        vector<uint8_t> vchSig;
        BOOST_CHECK(key.SignECDSA(hash, vchSig));
        vchSig.push_back((uint8_t)SIGHASH_ALL | SIGHASH_FORKID);
        result << vchSig;
    }
    return result;
}

BOOST_AUTO_TEST_CASE(multisig_verify)
{
    unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC | SCRIPT_ENABLE_SIGHASH_FORKID;

    ScriptError err;
    CKey key[4];
    CAmount amount = 0;
    for (int i = 0; i < 4; i++)
        key[i].MakeNewKey(true);

    CScript a_and_b;
    a_and_b << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;

    CScript a_or_b;
    a_or_b << OP_1 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;

    CScript escrow;
    escrow << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey())
           << ToByteVector(key[2].GetPubKey()) << OP_3 << OP_CHECKMULTISIG;

    CMutableTransaction txFrom; // Funding transaction
    txFrom.vout.resize(3);
    txFrom.vout[0].scriptPubKey = a_and_b;
    txFrom.vout[1].scriptPubKey = a_or_b;
    txFrom.vout[2].scriptPubKey = escrow;

    CMutableTransaction txTo[3]; // Spending transaction
    for (int i = 0; i < 3; i++)
    {
        txTo[i].vin.resize(1);
        txTo[i].vout.resize(1);
        txTo[i].vin[0].prevout.n = i;
        txTo[i].vin[0].prevout.hash = txFrom.GetHash();
        txTo[i].vout[0].nValue = 1;
    }

    vector<CKey> keys;
    CScript s;

    // Test a AND b:
    keys.assign(1, key[0]);
    keys.push_back(key[1]);
    s = sign_multisig(a_and_b, keys, txTo[0], 0);
    BOOST_CHECK(VerifyScript(
        s, a_and_b, flags, MAX_OPS_PER_SCRIPT, MutableTransactionSignatureChecker(&txTo[0], 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_OK, ScriptErrorString(err));

    for (int i = 0; i < 4; i++)
    {
        keys.assign(1, key[i]);
        s = sign_multisig(a_and_b, keys, txTo[0], 0);
        BOOST_CHECK_MESSAGE(!VerifyScript(s, a_and_b, flags, MAX_OPS_PER_SCRIPT,
                                MutableTransactionSignatureChecker(&txTo[0], 0, amount), &err),
            strprintf("a&b 1: %d", i));
        BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_INVALID_STACK_OPERATION, ScriptErrorString(err));

        keys.assign(1, key[1]);
        keys.push_back(key[i]);
        s = sign_multisig(a_and_b, keys, txTo[0], 0);
        BOOST_CHECK_MESSAGE(!VerifyScript(s, a_and_b, flags, MAX_OPS_PER_SCRIPT,
                                MutableTransactionSignatureChecker(&txTo[0], 0, amount), &err),
            strprintf("a&b 2: %d", i));
        BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_EVAL_FALSE, ScriptErrorString(err));
    }

    // Test a OR b:
    for (int i = 0; i < 4; i++)
    {
        keys.assign(1, key[i]);
        s = sign_multisig(a_or_b, keys, txTo[1], 0);
        if (i == 0 || i == 1)
        {
            BOOST_CHECK_MESSAGE(VerifyScript(s, a_or_b, flags, MAX_OPS_PER_SCRIPT,
                                    MutableTransactionSignatureChecker(&txTo[1], 0, amount), &err),
                strprintf("a|b: %d", i));
            BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_OK, ScriptErrorString(err));
        }
        else
        {
            BOOST_CHECK_MESSAGE(!VerifyScript(s, a_or_b, flags, MAX_OPS_PER_SCRIPT,
                                    MutableTransactionSignatureChecker(&txTo[1], 0, amount), &err),
                strprintf("a|b: %d", i));
            BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_EVAL_FALSE, ScriptErrorString(err));
        }
    }
    s.clear();
    s << OP_0 << OP_1;
    BOOST_CHECK(!VerifyScript(
        s, a_or_b, flags, MAX_OPS_PER_SCRIPT, MutableTransactionSignatureChecker(&txTo[1], 0, amount), &err));
    BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_SIG_DER, ScriptErrorString(err));


    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            keys.assign(1, key[i]);
            keys.push_back(key[j]);
            s = sign_multisig(escrow, keys, txTo[2], 0);
            if (i < j && i < 3 && j < 3)
            {
                BOOST_CHECK_MESSAGE(VerifyScript(s, escrow, flags, MAX_OPS_PER_SCRIPT,
                                        MutableTransactionSignatureChecker(&txTo[2], 0, amount), &err),
                    strprintf("escrow 1: %d %d", i, j));
                BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_OK, ScriptErrorString(err));
            }
            else
            {
                BOOST_CHECK_MESSAGE(!VerifyScript(s, escrow, flags, MAX_OPS_PER_SCRIPT,
                                        MutableTransactionSignatureChecker(&txTo[2], 0, amount), &err),
                    strprintf("escrow 2: %d %d", i, j));
                BOOST_CHECK_MESSAGE(err == SCRIPT_ERR_EVAL_FALSE, ScriptErrorString(err));
            }
        }
}

BOOST_AUTO_TEST_CASE(multisig_IsStandard)
{
    CKey key[4];
    for (int i = 0; i < 4; i++)
        key[i].MakeNewKey(true);

    txnouttype whichType;

    CScript a_and_b;
    a_and_b << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;
    BOOST_CHECK(::IsStandard(a_and_b, whichType));

    CScript a_or_b;
    a_or_b << OP_1 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;
    BOOST_CHECK(::IsStandard(a_or_b, whichType));

    CScript escrow;
    escrow << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey())
           << ToByteVector(key[2].GetPubKey()) << OP_3 << OP_CHECKMULTISIG;
    BOOST_CHECK(::IsStandard(escrow, whichType));

    CScript one_of_four;
    one_of_four << OP_1 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey())
                << ToByteVector(key[2].GetPubKey()) << ToByteVector(key[3].GetPubKey()) << OP_4 << OP_CHECKMULTISIG;
    BOOST_CHECK(!::IsStandard(one_of_four, whichType));

    CScript malformed[6];
    malformed[0] << OP_3 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2
                 << OP_CHECKMULTISIG;
    malformed[1] << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_3
                 << OP_CHECKMULTISIG;
    malformed[2] << OP_0 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2
                 << OP_CHECKMULTISIG;
    malformed[3] << OP_1 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_0
                 << OP_CHECKMULTISIG;
    malformed[4] << OP_1 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_CHECKMULTISIG;
    malformed[5] << OP_1 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey());

    for (int i = 0; i < 6; i++)
        BOOST_CHECK(!::IsStandard(malformed[i], whichType));
}

BOOST_AUTO_TEST_CASE(multisig_Solver1)
{
    // Tests Solver() that returns lists of keys that are
    // required to satisfy a ScriptPubKey
    //
    // Also tests IsMine() and ExtractDestination()
    //
    // Note: ExtractDestination for the multisignature transactions
    // always returns false for this release, even if you have
    // one key that would satisfy an (a|b) or 2-of-3 keys needed
    // to spend an escrow transaction.
    //
    CBasicKeyStore keystore, emptykeystore, partialkeystore;
    CKey key[3];
    CTxDestination keyaddr[3];
    for (int i = 0; i < 3; i++)
    {
        key[i].MakeNewKey(true);
        keystore.AddKey(key[i]);
        keyaddr[i] = key[i].GetPubKey().GetID();
    }
    partialkeystore.AddKey(key[0]);

    {
        vector<valtype> solutions;
        txnouttype whichType;
        CScript s;
        s << ToByteVector(key[0].GetPubKey()) << OP_CHECKSIG;
        BOOST_CHECK(Solver(s, whichType, solutions));
        BOOST_CHECK(solutions.size() == 1);
        CTxDestination addr;
        BOOST_CHECK(ExtractDestination(s, addr));
        BOOST_CHECK(addr == keyaddr[0]);
#ifdef ENABLE_WALLET
        CBlockIndex *nullBestBlock = nullptr;
        BOOST_CHECK(IsMine(keystore, s, nullBestBlock));
        BOOST_CHECK(!IsMine(emptykeystore, s, nullBestBlock));
#endif
    }
    {
        vector<valtype> solutions;
        txnouttype whichType;
        CScript s;
        s << OP_DUP << OP_HASH160 << ToByteVector(key[0].GetPubKey().GetID()) << OP_EQUALVERIFY << OP_CHECKSIG;
        BOOST_CHECK(Solver(s, whichType, solutions));
        BOOST_CHECK(solutions.size() == 1);
        CTxDestination addr;
        BOOST_CHECK(ExtractDestination(s, addr));
        BOOST_CHECK(addr == keyaddr[0]);
#ifdef ENABLE_WALLET
        CBlockIndex *nullBestBlock = nullptr;
        BOOST_CHECK(IsMine(keystore, s, nullBestBlock));
        BOOST_CHECK(!IsMine(emptykeystore, s, nullBestBlock));
#endif
    }
    {
        vector<valtype> solutions;
        txnouttype whichType;
        CScript s;
        s << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;
        BOOST_CHECK(Solver(s, whichType, solutions));
        BOOST_CHECK_EQUAL(solutions.size(), 4U);
        CTxDestination addr;
        BOOST_CHECK(!ExtractDestination(s, addr));
#ifdef ENABLE_WALLET
        CBlockIndex *nullBestBlock = nullptr;
        BOOST_CHECK(IsMine(keystore, s, nullBestBlock));
        BOOST_CHECK(!IsMine(emptykeystore, s, nullBestBlock));
        BOOST_CHECK(!IsMine(partialkeystore, s, nullBestBlock));
#endif
    }
    {
        vector<valtype> solutions;
        txnouttype whichType;
        CScript s;
        s << OP_1 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;
        BOOST_CHECK(Solver(s, whichType, solutions));
        BOOST_CHECK_EQUAL(solutions.size(), 4U);
        vector<CTxDestination> addrs;
        int nRequired;
        BOOST_CHECK(ExtractDestinations(s, whichType, addrs, nRequired));
        BOOST_CHECK(addrs[0] == keyaddr[0]);
        BOOST_CHECK(addrs[1] == keyaddr[1]);
        BOOST_CHECK(nRequired == 1);
#ifdef ENABLE_WALLET
        CBlockIndex *nullBestBlock = nullptr;
        BOOST_CHECK(IsMine(keystore, s, nullBestBlock));
        BOOST_CHECK(!IsMine(emptykeystore, s, nullBestBlock));
        BOOST_CHECK(!IsMine(partialkeystore, s, nullBestBlock));
#endif
    }
    {
        vector<valtype> solutions;
        txnouttype whichType;
        CScript s;
        s << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey())
          << ToByteVector(key[2].GetPubKey()) << OP_3 << OP_CHECKMULTISIG;
        BOOST_CHECK(Solver(s, whichType, solutions));
        BOOST_CHECK(solutions.size() == 5);
    }
    {
        vector<valtype> solutions;
        txnouttype whichType;
        CScript s;
        // Try some non-minimal PUSHDATA pushes in various standard scripts
        for (auto pushdataop : {OP_PUSHDATA1, OP_PUSHDATA2, OP_PUSHDATA4})
        {
            // mutated TX_PUBKEY
            s.clear();
            AppendPush(s, pushdataop, ToByteVector(keyaddr[0]));
            s << OP_CHECKSIG;
            BOOST_CHECK(!Solver(s, whichType, solutions));
            BOOST_CHECK_EQUAL(whichType, TX_NONSTANDARD);
            BOOST_CHECK_EQUAL(solutions.size(), 0);

            // mutated TX_PUBKEYHASH
            s.clear();
            s << OP_DUP << OP_HASH160;
            AppendPush(s, pushdataop, ToByteVector(keyaddr[0].GetID()));
            s << OP_EQUALVERIFY << OP_CHECKSIG;
            BOOST_CHECK(!Solver(s, whichType, solutions));
            BOOST_CHECK_EQUAL(whichType, TX_NONSTANDARD);
            BOOST_CHECK_EQUAL(solutions.size(), 0);

            // mutated TX_SCRIPTHASH
            s.clear();
            s << OP_HASH160;
            AppendPush(s, pushdataop, ToByteVector(CScriptID(redeemScript)));
            s << OP_EQUAL;
            BOOST_CHECK(!Solver(s, whichType, solutions));
            BOOST_CHECK_EQUAL(whichType, TX_NONSTANDARD);
            BOOST_CHECK_EQUAL(solutions.size(), 0);

            // mutated TX_MULTISIG -- pubkey
            s.clear();
            s << OP_1;
            AppendPush(s, pushdataop, ToByteVector(keyaddr[0]));
            s << ToByteVector(keyaddr[1]) << OP_2 << OP_CHECKMULTISIG;
            BOOST_CHECK(!Solver(s, whichType, solutions));
            BOOST_CHECK_EQUAL(whichType, TX_NONSTANDARD);
            BOOST_CHECK_EQUAL(solutions.size(), 0);

            // mutated TX_MULTISIG -- num_signatures
            s.clear();
            AppendPush(s, pushdataop, {1});
            s << ToByteVector(keyaddr[0]) << ToByteVector(keyaddr[1]) << OP_2 << OP_CHECKMULTISIG;
            BOOST_CHECK(!Solver(s, whichType, solutions));
            BOOST_CHECK_EQUAL(whichType, TX_NONSTANDARD);
            BOOST_CHECK_EQUAL(solutions.size(), 0);

            // mutated TX_MULTISIG -- num_keyaddr
            s.clear();
            s << OP_1 << ToByteVector(keyaddr[0]) << ToByteVector(keyaddr[1]);
            AppendPush(s, pushdataop, {2});
            s << OP_CHECKMULTISIG;
            BOOST_CHECK(!Solver(s, whichType, solutions));
            BOOST_CHECK_EQUAL(whichType, TX_NONSTANDARD);
            BOOST_CHECK_EQUAL(solutions.size(), 0);
        }

        // also try pushing the num_signatures and num_keyaddr using PUSH_N opcode
        // instead of OP_N opcode:
        s.clear();
        s << std::vector<uint8_t>{1} << ToByteVector(keyaddr[0]) << ToByteVector(keyaddr[1]) << OP_2
          << OP_CHECKMULTISIG;
        BOOST_CHECK(!Solver(s, whichType, solutions));
        BOOST_CHECK_EQUAL(whichType, TX_NONSTANDARD);
        BOOST_CHECK_EQUAL(solutions.size(), 0);
        s.clear();
        s << OP_1 << ToByteVector(keyaddr[0]) << ToByteVector(keyaddr[1]) << std::vector<uint8_t>{2}
          << OP_CHECKMULTISIG;
        BOOST_CHECK(!Solver(s, whichType, solutions));
        BOOST_CHECK_EQUAL(whichType, TX_NONSTANDARD);
        BOOST_CHECK_EQUAL(solutions.size(), 0);

        // Non-minimal pushes in OP_RETURN scripts are standard (some OP_RETURN
        // protocols like SLP rely on this). Also it turns out OP_RESERVED gets past
        // IsPushOnly and thus is standard here.
        std::vector<uint8_t> op_return_nonminimal{
            OP_RETURN, OP_RESERVED, OP_PUSHDATA1, 0x00, 0x01, 0x01, OP_PUSHDATA4, 0x01, 0x00, 0x00, 0x00, 0xaa};
        s.assign(op_return_nonminimal.begin(), op_return_nonminimal.end());
        BOOST_CHECK(Solver(s, whichType, solutions));
        BOOST_CHECK_EQUAL(whichType, TX_NULL_DATA);
        BOOST_CHECK_EQUAL(solutions.size(), 0);
    }
}

BOOST_AUTO_TEST_CASE(multisig_Sign)
{
    // Test SignSignature() (and therefore the version of Solver() that signs transactions)
    CBasicKeyStore keystore;
    CKey key[4];
    for (int i = 0; i < 4; i++)
    {
        key[i].MakeNewKey(true);
        keystore.AddKey(key[i]);
    }

    CScript a_and_b;
    a_and_b << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;

    CScript a_or_b;
    a_or_b << OP_1 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey()) << OP_2 << OP_CHECKMULTISIG;

    CScript escrow;
    escrow << OP_2 << ToByteVector(key[0].GetPubKey()) << ToByteVector(key[1].GetPubKey())
           << ToByteVector(key[2].GetPubKey()) << OP_3 << OP_CHECKMULTISIG;

    CMutableTransaction txFrom; // Funding transaction
    txFrom.vout.resize(3);
    txFrom.vout[0].scriptPubKey = a_and_b;
    txFrom.vout[1].scriptPubKey = a_or_b;
    txFrom.vout[2].scriptPubKey = escrow;

    CMutableTransaction txTo[3]; // Spending transaction
    for (int i = 0; i < 3; i++)
    {
        txTo[i].vin.resize(1);
        txTo[i].vout.resize(1);
        txTo[i].vin[0].prevout.n = i;
        txTo[i].vin[0].prevout.hash = txFrom.GetHash();
        txTo[i].vout[0].nValue = 1;
    }

    for (int i = 0; i < 3; i++)
    {
        BOOST_CHECK_MESSAGE(SignSignature(keystore, txFrom, txTo[i], 0), strprintf("SignSignature %d", i));
    }
}

#ifdef ENABLE_WALLET
BOOST_AUTO_TEST_CASE(cltv_freeze)
{
    CKey key[4];
    for (int i = 0; i < 2; i++)
        key[i].MakeNewKey(true);

    // Create and unpack a CLTV script
    vector<valtype> solutions;
    txnouttype whichType;
    vector<CTxDestination> addresses;
    int nRequiredReturn;
    txnouttype type = TX_CLTV;

    // check cltv solve for block
    CPubKey newKey1 = ToByteVector(key[0].GetPubKey());
    CTxDestination newAddr1 = CTxDestination(newKey1.GetID());
    CScriptNum nFreezeLockTime(50000);
    CScript s1 = GetScriptForFreeze(nFreezeLockTime, newKey1);

    BOOST_CHECK(Solver(s1, whichType, solutions));
    BOOST_CHECK(whichType == TX_CLTV);
    BOOST_CHECK(solutions.size() == 2);
    BOOST_CHECK(CScriptNum(solutions[0], false) == nFreezeLockTime);

    nRequiredReturn = 0;
    ExtractDestinations(s1, type, addresses, nRequiredReturn);

    for (const CTxDestination &addr : addresses)
        BOOST_CHECK(EncodeDestination(newAddr1) == EncodeDestination(addr));
    BOOST_CHECK(nRequiredReturn == 1);


    // check cltv solve for datetime
    CPubKey newKey2 = ToByteVector(key[0].GetPubKey());
    CTxDestination newAddr2 = CTxDestination(newKey2.GetID());
    nFreezeLockTime = CScriptNum(1482255731);
    CScript s2 = GetScriptForFreeze(nFreezeLockTime, newKey2);

    BOOST_CHECK(Solver(s2, whichType, solutions));
    BOOST_CHECK(whichType == TX_CLTV);
    BOOST_CHECK(solutions.size() == 2);
    BOOST_CHECK(CScriptNum(solutions[0], false) == nFreezeLockTime);

    nRequiredReturn = 0;
    ExtractDestinations(s2, type, addresses, nRequiredReturn);

    for (const CTxDestination &addr : addresses)
        BOOST_CHECK(newAddr2 == addr);

    BOOST_CHECK(nRequiredReturn == 1);
}

BOOST_AUTO_TEST_CASE(opreturn_send)
{
    CKey key[4];
    for (int i = 0; i < 2; i++)
        key[i].MakeNewKey(true);

    CBasicKeyStore keystore;

    // Create and unpack a CLTV script
    vector<valtype> solutions;
    txnouttype whichType;
    vector<CTxDestination> addresses;

    string inMsg = "hello world", outMsg = "";
    CScript s = GetScriptLabelPublic(inMsg);

    outMsg = getLabelPublic(s);
    BOOST_CHECK(inMsg == outMsg);
    BOOST_CHECK(Solver(s, whichType, solutions));
    BOOST_CHECK(whichType == TX_LABELPUBLIC);
}
#endif
BOOST_AUTO_TEST_SUITE_END()
