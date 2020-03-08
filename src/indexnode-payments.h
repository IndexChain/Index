// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INDEXNODE_PAYMENTS_H
#define INDEXNODE_PAYMENTS_H

#include "util.h"
#include "core_io.h"
#include "key.h"
#include "main.h"
#include "indexnode.h"
#include "utilstrencodings.h"

class CIndexnodePayments;
class CIndexnodePaymentVote;
class CIndexnodeBlockPayees;

static const int MNPAYMENTS_SIGNATURES_REQUIRED         = 6;
static const int MNPAYMENTS_SIGNATURES_TOTAL            = 10;

//! minimum peer version that can receive and send indexnode payment messages,
//  vote for indexnode and be elected as a payment winner
// V1 - Last protocol version before update
// V2 - Newest protocol version
static const int MIN_INDEXNODE_PAYMENT_PROTO_VERSION_1 = MIN_PEER_PROTO_VERSION;
static const int MIN_INDEXNODE_PAYMENT_PROTO_VERSION_2 = PROTOCOL_VERSION;

extern CCriticalSection cs_vecPayees;
extern CCriticalSection cs_mapIndexnodeBlocks;
extern CCriticalSection cs_mapIndexnodePayeeVotes;

extern CIndexnodePayments mnpayments;

/// TODO: all 4 functions do not belong here really, they should be refactored/moved somewhere (main.cpp ?)
bool IsBlockValueValid(const CBlock& block, int nBlockHeight, CAmount blockReward, std::string &strErrorRet);
bool IsBlockPayeeValid(const CTransaction& txNew, int nBlockHeight, CAmount blockReward);
void FillBlockPayments(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutIndexnodeRet, std::vector<CTxOut>& voutSuperblockRet);
std::string GetRequiredPaymentsString(int nBlockHeight);

class CIndexnodePayee
{
private:
    CScript scriptPubKey;
    std::vector<uint256> vecVoteHashes;

public:
    CIndexnodePayee() :
        scriptPubKey(),
        vecVoteHashes()
        {}

    CIndexnodePayee(CScript payee, uint256 hashIn) :
        scriptPubKey(payee),
        vecVoteHashes()
    {
        vecVoteHashes.push_back(hashIn);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(*(CScriptBase*)(&scriptPubKey));
        READWRITE(vecVoteHashes);
    }

    CScript GetPayee() { return scriptPubKey; }

    void AddVoteHash(uint256 hashIn) { vecVoteHashes.push_back(hashIn); }
    std::vector<uint256> GetVoteHashes() { return vecVoteHashes; }
    int GetVoteCount() { return vecVoteHashes.size(); }
    std::string ToString() const;
};

// Keep track of votes for payees from indexnodes
class CIndexnodeBlockPayees
{
public:
    int nBlockHeight;
    std::vector<CIndexnodePayee> vecPayees;

    CIndexnodeBlockPayees() :
        nBlockHeight(0),
        vecPayees()
        {}
    CIndexnodeBlockPayees(int nBlockHeightIn) :
        nBlockHeight(nBlockHeightIn),
        vecPayees()
        {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(nBlockHeight);
        READWRITE(vecPayees);
    }

    void AddPayee(const CIndexnodePaymentVote& vote);
    bool GetBestPayee(CScript& payeeRet);
    bool HasPayeeWithVotes(CScript payeeIn, int nVotesReq);

    bool IsTransactionValid(const CTransaction& txNew, bool fMTP, int nHeight);

    std::string GetRequiredPaymentsString();
};

// vote for the winning payment
class CIndexnodePaymentVote
{
public:
    CTxIn vinIndexnode;

    int nBlockHeight;
    CScript payee;
    std::vector<unsigned char> vchSig;

    CIndexnodePaymentVote() :
        vinIndexnode(),
        nBlockHeight(0),
        payee(),
        vchSig()
        {}

    CIndexnodePaymentVote(CTxIn vinIndexnode, int nBlockHeight, CScript payee) :
        vinIndexnode(vinIndexnode),
        nBlockHeight(nBlockHeight),
        payee(payee),
        vchSig()
        {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(vinIndexnode);
        READWRITE(nBlockHeight);
        READWRITE(*(CScriptBase*)(&payee));
        READWRITE(vchSig);
    }

    uint256 GetHash() const {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << *(CScriptBase*)(&payee);
        ss << nBlockHeight;
        ss << vinIndexnode.prevout;
        return ss.GetHash();
    }

    bool Sign();
    bool CheckSignature(const CPubKey& pubKeyIndexnode, int nValidationHeight, int &nDos);

    bool IsValid(CNode* pnode, int nValidationHeight, std::string& strError);
    void Relay();

    bool IsVerified() { return !vchSig.empty(); }
    void MarkAsNotVerified() { vchSig.clear(); }

    std::string ToString() const;
};

//
// Indexnode Payments Class
// Keeps track of who should get paid for which blocks
//

class CIndexnodePayments
{
private:
    // indexnode count times nStorageCoeff payments blocks should be stored ...
    const float nStorageCoeff;
    // ... but at least nMinBlocksToStore (payments blocks)
    const int nMinBlocksToStore;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

public:
    std::map<uint256, CIndexnodePaymentVote> mapIndexnodePaymentVotes;
    std::map<int, CIndexnodeBlockPayees> mapIndexnodeBlocks;
    std::map<COutPoint, int> mapIndexnodesLastVote;

    CIndexnodePayments() : nStorageCoeff(1.25), nMinBlocksToStore(5000) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(mapIndexnodePaymentVotes);
        READWRITE(mapIndexnodeBlocks);
    }

    void Clear();

    bool AddPaymentVote(const CIndexnodePaymentVote& vote);
    bool HasVerifiedPaymentVote(uint256 hashIn);
    bool ProcessBlock(int nBlockHeight);

    void Sync(CNode* node);
    void RequestLowDataPaymentBlocks(CNode* pnode);
    void CheckAndRemove();

    bool GetBlockPayee(int nBlockHeight, CScript& payee);
    bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight, bool fMTP);
    bool IsScheduled(CIndexnode& mn, int nNotBlockHeight);

    bool CanVote(COutPoint outIndexnode, int nBlockHeight);

    int GetMinIndexnodePaymentsProto();
    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    std::string GetRequiredPaymentsString(int nBlockHeight);
    void FillBlockPayee(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutIndexnodeRet);
    std::string ToString() const;

    int GetBlockCount() { return mapIndexnodeBlocks.size(); }
    int GetVoteCount() { return mapIndexnodePaymentVotes.size(); }

    bool IsEnoughData();
    int GetStorageLimit();

    void UpdatedBlockTip(const CBlockIndex *pindex);
};

#endif
