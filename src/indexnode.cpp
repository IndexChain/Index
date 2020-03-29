// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeindexnode.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "darksend.h"
#include "init.h"
//#include "governance.h"
#include "indexnode.h"
#include "indexnode-payments.h"
#include "indexnodeconfig.h"
#include "indexnode-sync.h"
#include "indexnodeman.h"
#include "util.h"
#include "validationinterface.h"

#include <boost/lexical_cast.hpp>


CIndexnode::CIndexnode() :
        vin(),
        addr(),
        pubKeyCollateralAddress(),
        pubKeyIndexnode(),
        lastPing(),
        vchSig(),
        sigTime(GetAdjustedTime()),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(0),
        nActiveState(INDEXNODE_ENABLED),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(PROTOCOL_VERSION),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

CIndexnode::CIndexnode(CService addrNew, CTxIn vinNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyIndexnodeNew, int nProtocolVersionIn) :
        vin(vinNew),
        addr(addrNew),
        pubKeyCollateralAddress(pubKeyCollateralAddressNew),
        pubKeyIndexnode(pubKeyIndexnodeNew),
        lastPing(),
        vchSig(),
        sigTime(GetAdjustedTime()),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(0),
        nActiveState(INDEXNODE_ENABLED),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(nProtocolVersionIn),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

CIndexnode::CIndexnode(const CIndexnode &other) :
        vin(other.vin),
        addr(other.addr),
        pubKeyCollateralAddress(other.pubKeyCollateralAddress),
        pubKeyIndexnode(other.pubKeyIndexnode),
        lastPing(other.lastPing),
        vchSig(other.vchSig),
        sigTime(other.sigTime),
        nLastDsq(other.nLastDsq),
        nTimeLastChecked(other.nTimeLastChecked),
        nTimeLastPaid(other.nTimeLastPaid),
        nTimeLastWatchdogVote(other.nTimeLastWatchdogVote),
        nActiveState(other.nActiveState),
        nCacheCollateralBlock(other.nCacheCollateralBlock),
        nBlockLastPaid(other.nBlockLastPaid),
        nProtocolVersion(other.nProtocolVersion),
        nPoSeBanScore(other.nPoSeBanScore),
        nPoSeBanHeight(other.nPoSeBanHeight),
        fAllowMixingTx(other.fAllowMixingTx),
        fUnitTest(other.fUnitTest) {}

CIndexnode::CIndexnode(const CIndexnodeBroadcast &mnb) :
        vin(mnb.vin),
        addr(mnb.addr),
        pubKeyCollateralAddress(mnb.pubKeyCollateralAddress),
        pubKeyIndexnode(mnb.pubKeyIndexnode),
        lastPing(mnb.lastPing),
        vchSig(mnb.vchSig),
        sigTime(mnb.sigTime),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(mnb.sigTime),
        nActiveState(mnb.nActiveState),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(mnb.nProtocolVersion),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

//CSporkManager sporkManager;
//
// When a new indexnode broadcast is sent, update our information
//
bool CIndexnode::UpdateFromNewBroadcast(CIndexnodeBroadcast &mnb) {
    if (mnb.sigTime <= sigTime && !mnb.fRecovery) return false;

    pubKeyIndexnode = mnb.pubKeyIndexnode;
    sigTime = mnb.sigTime;
    vchSig = mnb.vchSig;
    nProtocolVersion = mnb.nProtocolVersion;
    addr = mnb.addr;
    nPoSeBanScore = 0;
    nPoSeBanHeight = 0;
    nTimeLastChecked = 0;
    int nDos = 0;
    if (mnb.lastPing == CIndexnodePing() || (mnb.lastPing != CIndexnodePing() && mnb.lastPing.CheckAndUpdate(this, true, nDos))) {
        SetLastPing(mnb.lastPing);
        mnodeman.mapSeenIndexnodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
    }
    // if it matches our Indexnode privkey...
    if (fIndexNode && pubKeyIndexnode == activeIndexnode.pubKeyIndexnode) {
        nPoSeBanScore = -INDEXNODE_POSE_BAN_MAX_SCORE;
        if (nProtocolVersion == PROTOCOL_VERSION) {
            // ... and PROTOCOL_VERSION, then we've been remotely activated ...
            activeIndexnode.ManageState();
        } else {
            // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
            // but also do not ban the node we get this message from
            LogPrintf("CIndexnode::UpdateFromNewBroadcast -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", nProtocolVersion, PROTOCOL_VERSION);
            return false;
        }
    }
    return true;
}

//
// Deterministically calculate a given "score" for a Indexnode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CIndexnode::CalculateScore(const uint256 &blockHash) {
    uint256 aux = ArithToUint256(UintToArith256(vin.prevout.hash) + vin.prevout.n);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << blockHash;
    arith_uint256 hash2 = UintToArith256(ss.GetHash());

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << blockHash;
    ss2 << aux;
    arith_uint256 hash3 = UintToArith256(ss2.GetHash());

    return (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
}

void CIndexnode::Check(bool fForce) {
    LOCK(cs);

    if (ShutdownRequested()) return;

    if (!fForce && (GetTime() - nTimeLastChecked < INDEXNODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    LogPrint("indexnode", "CIndexnode::Check -- Indexnode %s is in %s state\n", vin.prevout.ToStringShort(), GetStateString());

    //once spent, stop doing the checks
    if (IsOutpointSpent()) return;

    int nHeight = 0;
    if (!fUnitTest) {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) return;

        CCoins coins;
        if (!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
            (unsigned int) vin.prevout.n >= coins.vout.size() ||
            coins.vout[vin.prevout.n].IsNull()) {
            SetStatus(INDEXNODE_OUTPOINT_SPENT);
            LogPrint("indexnode", "CIndexnode::Check -- Failed to find Indexnode UTXO, indexnode=%s\n", vin.prevout.ToStringShort());
            return;
        }

        nHeight = chainActive.Height();
    }

    if (IsPoSeBanned()) {
        if (nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Indexnode still will be on the edge and can be banned back easily if it keeps ignoring mnverify
        // or connect attempts. Will require few mnverify messages to strengthen its position in mn list.
        LogPrintf("CIndexnode::Check -- Indexnode %s is unbanned and back in list now\n", vin.prevout.ToStringShort());
        DecreasePoSeBanScore();
    } else if (nPoSeBanScore >= INDEXNODE_POSE_BAN_MAX_SCORE) {
        SetStatus(INDEXNODE_POSE_BAN);
        // ban for the whole payment cycle
        nPoSeBanHeight = nHeight + mnodeman.size();
        LogPrintf("CIndexnode::Check -- Indexnode %s is banned till block %d now\n", vin.prevout.ToStringShort(), nPoSeBanHeight);
        return;
    }

    int nActiveStatePrev = nActiveState;
    bool fOurIndexnode = fIndexNode && activeIndexnode.pubKeyIndexnode == pubKeyIndexnode;

    // indexnode doesn't meet payment protocol requirements ...
/*    bool fRequireUpdate = nProtocolVersion < mnpayments.GetMinIndexnodePaymentsProto() ||
                          // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                          (fOurIndexnode && nProtocolVersion < PROTOCOL_VERSION); */

    // indexnode doesn't meet payment protocol requirements ...
    bool fRequireUpdate = nProtocolVersion < mnpayments.GetMinIndexnodePaymentsProto() ||
                          // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                          (fOurIndexnode && (nProtocolVersion < MIN_INDEXNODE_PAYMENT_PROTO_VERSION_1 || nProtocolVersion > MIN_INDEXNODE_PAYMENT_PROTO_VERSION_2));

    if (fRequireUpdate) {
        SetStatus(INDEXNODE_UPDATE_REQUIRED);
        if (nActiveStatePrev != nActiveState) {
            LogPrint("indexnode", "CIndexnode::Check -- Indexnode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    // keep old indexnodes on start, give them a chance to receive updates...
    bool fWaitForPing = !indexnodeSync.IsIndexnodeListSynced() && !IsPingedWithin(INDEXNODE_MIN_MNP_SECONDS);

    if (fWaitForPing && !fOurIndexnode) {
        // ...but if it was already expired before the initial check - return right away
        if (IsExpired() || IsWatchdogExpired() || IsNewStartRequired()) {
            LogPrint("indexnode", "CIndexnode::Check -- Indexnode %s is in %s state, waiting for ping\n", vin.prevout.ToStringShort(), GetStateString());
            return;
        }
    }

    // don't expire if we are still in "waiting for ping" mode unless it's our own indexnode
    if (!fWaitForPing || fOurIndexnode) {

        if (!IsPingedWithin(INDEXNODE_NEW_START_REQUIRED_SECONDS)) {
            SetStatus(INDEXNODE_NEW_START_REQUIRED);
            if (nActiveStatePrev != nActiveState) {
                LogPrint("indexnode", "CIndexnode::Check -- Indexnode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        bool fWatchdogActive = indexnodeSync.IsSynced() && mnodeman.IsWatchdogActive();
        bool fWatchdogExpired = (fWatchdogActive && ((GetTime() - nTimeLastWatchdogVote) > INDEXNODE_WATCHDOG_MAX_SECONDS));

//        LogPrint("indexnode", "CIndexnode::Check -- outpoint=%s, nTimeLastWatchdogVote=%d, GetTime()=%d, fWatchdogExpired=%d\n",
//                vin.prevout.ToStringShort(), nTimeLastWatchdogVote, GetTime(), fWatchdogExpired);

        if (fWatchdogExpired) {
            SetStatus(INDEXNODE_WATCHDOG_EXPIRED);
            if (nActiveStatePrev != nActiveState) {
                LogPrint("indexnode", "CIndexnode::Check -- Indexnode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        if (!IsPingedWithin(INDEXNODE_EXPIRATION_SECONDS)) {
            SetStatus(INDEXNODE_EXPIRED);
            if (nActiveStatePrev != nActiveState) {
                LogPrint("indexnode", "CIndexnode::Check -- Indexnode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }
    }

    if (lastPing.sigTime - sigTime < INDEXNODE_MIN_MNP_SECONDS) {
        SetStatus(INDEXNODE_PRE_ENABLED);
        if (nActiveStatePrev != nActiveState) {
            LogPrint("indexnode", "CIndexnode::Check -- Indexnode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    SetStatus(INDEXNODE_ENABLED); // OK
    if (nActiveStatePrev != nActiveState) {
        LogPrint("indexnode", "CIndexnode::Check -- Indexnode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
    }
}

bool CIndexnode::IsValidNetAddr() {
    return IsValidNetAddr(addr);
}

bool CIndexnode::IsValidForPayment() {
    if (nActiveState == INDEXNODE_ENABLED) {
        return true;
    }
//    if(!sporkManager.IsSporkActive(SPORK_14_REQUIRE_SENTINEL_FLAG) &&
//       (nActiveState == INDEXNODE_WATCHDOG_EXPIRED)) {
//        return true;
//    }

    return false;
}

bool CIndexnode::IsValidNetAddr(CService addrIn) {
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
           (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

bool CIndexnode::IsMyIndexnode(){
    BOOST_FOREACH(CIndexnodeConfig::CIndexnodeEntry mne, indexnodeConfig.getEntries()) {
        const std::string& txHash = mne.getTxHash();
        const std::string& outputIndex = mne.getOutputIndex();

        if(txHash==vin.prevout.hash.ToString().substr(0,64) &&
           outputIndex==to_string(vin.prevout.n))
            return true;
    }
    return false;
}

indexnode_info_t CIndexnode::GetInfo() {
    indexnode_info_t info;
    info.vin = vin;
    info.addr = addr;
    info.pubKeyCollateralAddress = pubKeyCollateralAddress;
    info.pubKeyIndexnode = pubKeyIndexnode;
    info.sigTime = sigTime;
    info.nLastDsq = nLastDsq;
    info.nTimeLastChecked = nTimeLastChecked;
    info.nTimeLastPaid = nTimeLastPaid;
    info.nTimeLastWatchdogVote = nTimeLastWatchdogVote;
    info.nTimeLastPing = lastPing.sigTime;
    info.nActiveState = nActiveState;
    info.nProtocolVersion = nProtocolVersion;
    info.fInfoValid = true;
    return info;
}

std::string CIndexnode::StateToString(int nStateIn) {
    switch (nStateIn) {
        case INDEXNODE_PRE_ENABLED:
            return "PRE_ENABLED";
        case INDEXNODE_ENABLED:
            return "ENABLED";
        case INDEXNODE_EXPIRED:
            return "EXPIRED";
        case INDEXNODE_OUTPOINT_SPENT:
            return "OUTPOINT_SPENT";
        case INDEXNODE_UPDATE_REQUIRED:
            return "UPDATE_REQUIRED";
        case INDEXNODE_WATCHDOG_EXPIRED:
            return "WATCHDOG_EXPIRED";
        case INDEXNODE_NEW_START_REQUIRED:
            return "NEW_START_REQUIRED";
        case INDEXNODE_POSE_BAN:
            return "POSE_BAN";
        default:
            return "UNKNOWN";
    }
}

std::string CIndexnode::GetStateString() const {
    return StateToString(nActiveState);
}

std::string CIndexnode::GetStatus() const {
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

void CIndexnode::SetStatus(int newState) {
    if(nActiveState!=newState){
        nActiveState = newState;
        if(IsMyIndexnode())
            GetMainSignals().UpdatedIndexnode(*this);
    }
}

void CIndexnode::SetLastPing(CIndexnodePing newIndexnodePing) {
    if(lastPing!=newIndexnodePing){
        lastPing = newIndexnodePing;
        if(IsMyIndexnode())
            GetMainSignals().UpdatedIndexnode(*this);
    }
}

void CIndexnode::SetTimeLastPaid(int64_t newTimeLastPaid) {
     if(nTimeLastPaid!=newTimeLastPaid){
        nTimeLastPaid = newTimeLastPaid;
        if(IsMyIndexnode())
            GetMainSignals().UpdatedIndexnode(*this);
    }   
}

void CIndexnode::SetBlockLastPaid(int newBlockLastPaid) {
     if(nBlockLastPaid!=newBlockLastPaid){
        nBlockLastPaid = newBlockLastPaid;
        if(IsMyIndexnode())
            GetMainSignals().UpdatedIndexnode(*this);
    }   
}

void CIndexnode::SetRank(int newRank) {
     if(nRank!=newRank){
        nRank = newRank;
        if(nRank < 0 || nRank > mnodeman.size()) nRank = 0;
        if(IsMyIndexnode())
            GetMainSignals().UpdatedIndexnode(*this);
    }   
}

std::string CIndexnode::ToString() const {
    std::string str;
    str += "indexnode{";
    str += addr.ToString();
    str += " ";
    str += std::to_string(nProtocolVersion);
    str += " ";
    str += vin.prevout.ToStringShort();
    str += " ";
    str += CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString();
    str += " ";
    str += std::to_string(lastPing == CIndexnodePing() ? sigTime : lastPing.sigTime);
    str += " ";
    str += std::to_string(lastPing == CIndexnodePing() ? 0 : lastPing.sigTime - sigTime);
    str += " ";
    str += std::to_string(nBlockLastPaid);
    str += "}\n";
    return str;
}

UniValue CIndexnode::ToJSON() const {
    UniValue ret(UniValue::VOBJ);
    std::string payee = CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString();
    COutPoint outpoint = vin.prevout;
    UniValue outpointObj(UniValue::VOBJ);
    UniValue authorityObj(UniValue::VOBJ);
    outpointObj.push_back(Pair("txid", outpoint.hash.ToString().substr(0,64)));
    outpointObj.push_back(Pair("index", to_string(outpoint.n)));

    std::string authority = addr.ToString();
    std::string ip   = authority.substr(0, authority.find(":"));
    std::string port = authority.substr(authority.find(":")+1, authority.length());
    authorityObj.push_back(Pair("ip", ip));
    authorityObj.push_back(Pair("port", port));
    
    // get myIndexnode data
    bool isMine = false;
    string label;
    int fIndex=0;
    BOOST_FOREACH(CIndexnodeConfig::CIndexnodeEntry mne, indexnodeConfig.getEntries()) {
        CTxIn myVin = CTxIn(uint256S(mne.getTxHash()), uint32_t(atoi(mne.getOutputIndex().c_str())));
        if(outpoint.ToStringShort()==myVin.prevout.ToStringShort()){
            isMine = true;
            label = mne.getAlias();
            break;
        }
        fIndex++;
    }

    ret.push_back(Pair("rank", nRank));
    ret.push_back(Pair("outpoint", outpointObj));
    ret.push_back(Pair("status", GetStatus()));
    ret.push_back(Pair("protocolVersion", nProtocolVersion));
    ret.push_back(Pair("payeeAddress", payee));
    ret.push_back(Pair("lastSeen", (int64_t) lastPing.sigTime * 1000));
    ret.push_back(Pair("activeSince", (int64_t)(sigTime * 1000)));
    ret.push_back(Pair("lastPaidTime", (int64_t) GetLastPaidTime() * 1000));
    ret.push_back(Pair("lastPaidBlock", GetLastPaidBlock()));
    ret.push_back(Pair("authority", authorityObj));
    ret.push_back(Pair("isMine", isMine));
    if(isMine){
        ret.push_back(Pair("label", label));
        ret.push_back(Pair("position", fIndex));
    }

    UniValue qualify(UniValue::VOBJ);

    CIndexnode* indexnode = const_cast <CIndexnode*> (this);
    qualify = mnodeman.GetNotQualifyReasonToUniValue(*indexnode, chainActive.Tip()->nHeight, true, mnodeman.CountEnabled());
    ret.push_back(Pair("qualify", qualify));

    return ret;
}

int CIndexnode::GetCollateralAge() {
    int nHeight;
    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain || !chainActive.Tip()) return -1;
        nHeight = chainActive.Height();
    }

    if (nCacheCollateralBlock == 0) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge > 0) {
            nCacheCollateralBlock = nHeight - nInputAge;
        } else {
            return nInputAge;
        }
    }

    return nHeight - nCacheCollateralBlock;
}

void CIndexnode::UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack) {
    if (!pindex) {
        LogPrintf("CIndexnode::UpdateLastPaid pindex is NULL\n");
        return;
    }

    const Consensus::Params &params = Params().GetConsensus();
    const CBlockIndex *BlockReading = pindex;

    CScript mnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());
    LogPrint("indexnode", "CIndexnode::UpdateLastPaidBlock -- searching for block with payment to %s\n", vin.prevout.ToStringShort());

    LOCK(cs_mapIndexnodeBlocks);

    for (int i = 0; BlockReading && BlockReading->nHeight > nBlockLastPaid && i < nMaxBlocksToScanBack; i++) {
//        LogPrintf("mnpayments.mapIndexnodeBlocks.count(BlockReading->nHeight)=%s\n", mnpayments.mapIndexnodeBlocks.count(BlockReading->nHeight));
//        LogPrintf("mnpayments.mapIndexnodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)=%s\n", mnpayments.mapIndexnodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2));
        if (mnpayments.mapIndexnodeBlocks.count(BlockReading->nHeight) &&
            mnpayments.mapIndexnodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)) {
            // LogPrintf("i=%s, BlockReading->nHeight=%s\n", i, BlockReading->nHeight);
            CBlock block;
            if (!ReadBlockFromDisk(block, BlockReading, Params().GetConsensus())) // shouldn't really happen
            {
                LogPrintf("ReadBlockFromDisk failed\n");
                continue;
            }
            CAmount nIndexnodePayment = GetIndexnodePayment(params, false,BlockReading->nHeight);

            BOOST_FOREACH(CTxOut txout, block.vtx[0].vout)
            if (mnpayee == txout.scriptPubKey && nIndexnodePayment == txout.nValue) {
                SetBlockLastPaid(BlockReading->nHeight);
                SetTimeLastPaid(BlockReading->nTime);
                LogPrint("indexnode", "CIndexnode::UpdateLastPaidBlock -- searching for block with payment to %s -- found new %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
                return;
            }
        }

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    // Last payment for this indexnode wasn't found in latest mnpayments blocks
    // or it was found in mnpayments blocks but wasn't found in the blockchain.
    // LogPrint("indexnode", "CIndexnode::UpdateLastPaidBlock -- searching for block with payment to %s -- keeping old %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
}

bool CIndexnodeBroadcast::Create(std::string strService, std::string strKeyIndexnode, std::string strTxHash, std::string strOutputIndex, std::string &strErrorRet, CIndexnodeBroadcast &mnbRet, bool fOffline) {
    LogPrintf("CIndexnodeBroadcast::Create\n");
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyIndexnodeNew;
    CKey keyIndexnodeNew;
    //need correct blocks to send ping
    if (!fOffline && !indexnodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Indexnode";
        LogPrintf("CIndexnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    //TODO
    if (!darkSendSigner.GetKeysFromSecret(strKeyIndexnode, keyIndexnodeNew, pubKeyIndexnodeNew)) {
        strErrorRet = strprintf("Invalid indexnode key %s", strKeyIndexnode);
        LogPrintf("CIndexnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!pwalletMain->GetIndexnodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for indexnode %s", strTxHash, strOutputIndex, strService);
        LogPrintf("CIndexnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    CService service = CService(strService);
    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            strErrorRet = strprintf("Invalid port %u for indexnode %s, only %d is supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
            LogPrintf("CIndexnodeBroadcast::Create -- %s\n", strErrorRet);
            return false;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for indexnode %s, %d is the only supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
        LogPrintf("CIndexnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    return Create(txin, CService(strService), keyCollateralAddressNew, pubKeyCollateralAddressNew, keyIndexnodeNew, pubKeyIndexnodeNew, strErrorRet, mnbRet);
}

bool CIndexnodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyIndexnodeNew, CPubKey pubKeyIndexnodeNew, std::string &strErrorRet, CIndexnodeBroadcast &mnbRet) {
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("indexnode", "CIndexnodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyIndexnodeNew.GetID() = %s\n",
             CBitcoinAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
             pubKeyIndexnodeNew.GetID().ToString());


    CIndexnodePing mnp(txin);
    if (!mnp.Sign(keyIndexnodeNew, pubKeyIndexnodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, indexnode=%s", txin.prevout.ToStringShort());
        LogPrintf("CIndexnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CIndexnodeBroadcast();
        return false;
    }

    int nHeight = chainActive.Height();
    if (nHeight < ZC_MODULUS_V2_START_BLOCK) {
        mnbRet = CIndexnodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyIndexnodeNew, MIN_PEER_PROTO_VERSION);
    } else {
        mnbRet = CIndexnodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyIndexnodeNew, PROTOCOL_VERSION);
    }

    if (!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address, indexnode=%s", txin.prevout.ToStringShort());
        LogPrintf("CIndexnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CIndexnodeBroadcast();
        return false;
    }
    mnbRet.SetLastPing(mnp);
    if (!mnbRet.Sign(keyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, indexnode=%s", txin.prevout.ToStringShort());
        LogPrintf("CIndexnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CIndexnodeBroadcast();
        return false;
    }

    return true;
}

bool CIndexnodeBroadcast::SimpleCheck(int &nDos) {
    nDos = 0;

    // make sure addr is valid
    if (!IsValidNetAddr()) {
        LogPrintf("CIndexnodeBroadcast::SimpleCheck -- Invalid addr, rejected: indexnode=%s  addr=%s\n",
                  vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CIndexnodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: indexnode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/unknown blockhash
    if (lastPing == CIndexnodePing() || !lastPing.SimpleCheck(nDos)) {
        // one of us is probably forked or smth, just mark it as expired and check the rest of the rules
        SetStatus(INDEXNODE_EXPIRED);
    }

    if (nProtocolVersion < mnpayments.GetMinIndexnodePaymentsProto()) {
        LogPrintf("CIndexnodeBroadcast::SimpleCheck -- ignoring outdated Indexnode: indexnode=%s  nProtocolVersion=%d\n", vin.prevout.ToStringShort(), nProtocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if (pubkeyScript.size() != 25) {
        LogPrintf("CIndexnodeBroadcast::SimpleCheck -- pubKeyCollateralAddress has the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyIndexnode.GetID());

    if (pubkeyScript2.size() != 25) {
        LogPrintf("CIndexnodeBroadcast::SimpleCheck -- pubKeyIndexnode has the wrong size\n");
        nDos = 100;
        return false;
    }

    if (!vin.scriptSig.empty()) {
        LogPrintf("CIndexnodeBroadcast::SimpleCheck -- Ignore Not Empty ScriptSig %s\n", vin.ToString());
        nDos = 100;
        return false;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != mainnetDefaultPort) return false;
    } else if (addr.GetPort() == mainnetDefaultPort) return false;

    return true;
}

bool CIndexnodeBroadcast::Update(CIndexnode *pmn, int &nDos) {
    nDos = 0;

    if (pmn->sigTime == sigTime && !fRecovery) {
        // mapSeenIndexnodeBroadcast in CIndexnodeMan::CheckMnbAndUpdateIndexnodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return false;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if (pmn->sigTime > sigTime) {
        LogPrintf("CIndexnodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Indexnode %s %s\n",
                  sigTime, pmn->sigTime, vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    pmn->Check();

    // indexnode is banned by PoSe
    if (pmn->IsPoSeBanned()) {
        LogPrintf("CIndexnodeBroadcast::Update -- Banned by PoSe, indexnode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // IsVnAssociatedWithPubkey is validated once in CheckOutpoint, after that they just need to match
    if (pmn->pubKeyCollateralAddress != pubKeyCollateralAddress) {
        LogPrintf("CIndexnodeBroadcast::Update -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CIndexnodeBroadcast::Update -- CheckSignature() failed, indexnode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // if ther was no indexnode broadcast recently or if it matches our Indexnode privkey...
    if (!pmn->IsBroadcastedWithin(INDEXNODE_MIN_MNB_SECONDS) || (fIndexNode && pubKeyIndexnode == activeIndexnode.pubKeyIndexnode)) {
        // take the newest entry
        LogPrintf("CIndexnodeBroadcast::Update -- Got UPDATED Indexnode entry: addr=%s\n", addr.ToString());
        if (pmn->UpdateFromNewBroadcast((*this))) {
            pmn->Check();
            RelayIndexNode();
        }
        indexnodeSync.AddedIndexnodeList();
        GetMainSignals().UpdatedIndexnode(*pmn);
    }

    return true;
}

bool CIndexnodeBroadcast::CheckOutpoint(int &nDos) {
    // we are a indexnode with the same vin (i.e. already activated) and this mnb is ours (matches our Indexnode privkey)
    // so nothing to do here for us
    if (fIndexNode && vin.prevout == activeIndexnode.vin.prevout && pubKeyIndexnode == activeIndexnode.pubKeyIndexnode) {
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CIndexnodeBroadcast::CheckOutpoint -- CheckSignature() failed, indexnode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            // not mnb fault, let it to be checked again later
            LogPrint("indexnode", "CIndexnodeBroadcast::CheckOutpoint -- Failed to aquire lock, addr=%s", addr.ToString());
            mnodeman.mapSeenIndexnodeBroadcast.erase(GetHash());
            return false;
        }

        CCoins coins;
        if (!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
            (unsigned int) vin.prevout.n >= coins.vout.size() ||
            coins.vout[vin.prevout.n].IsNull()) {
            LogPrint("indexnode", "CIndexnodeBroadcast::CheckOutpoint -- Failed to find Indexnode UTXO, indexnode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if (coins.vout[vin.prevout.n].nValue != INDEXNODE_COIN_REQUIRED * COIN) {
            LogPrint("indexnode", "CIndexnodeBroadcast::CheckOutpoint -- Indexnode UTXO should have 1000 IDX, indexnode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if (chainActive.Height() - coins.nHeight + 1 < Params().GetConsensus().nIndexnodeMinimumConfirmations) {
            LogPrintf("CIndexnodeBroadcast::CheckOutpoint -- Indexnode UTXO must have at least %d confirmations, indexnode=%s\n",
                      Params().GetConsensus().nIndexnodeMinimumConfirmations, vin.prevout.ToStringShort());
            // maybe we miss few blocks, let this mnb to be checked again later
            mnodeman.mapSeenIndexnodeBroadcast.erase(GetHash());
            return false;
        }
    }

    LogPrint("indexnode", "CIndexnodeBroadcast::CheckOutpoint -- Indexnode UTXO verified\n");

    // make sure the vout that was signed is related to the transaction that spawned the Indexnode
    //  - this is expensive, so it's only done once per Indexnode
    if (!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubKeyCollateralAddress)) {
        LogPrintf("CIndexnodeMan::CheckOutpoint -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 1000 IDX tx got nIndexnodeMinimumConfirmations
    uint256 hashBlock = uint256();
    CTransaction tx2;
    GetTransaction(vin.prevout.hash, tx2, Params().GetConsensus(), hashBlock, true);
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex *pMNIndex = (*mi).second; // block for 1000 IDX tx -> 1 confirmation
            CBlockIndex *pConfIndex = chainActive[pMNIndex->nHeight + Params().GetConsensus().nIndexnodeMinimumConfirmations - 1]; // block where tx got nIndexnodeMinimumConfirmations
            if (pConfIndex->GetBlockTime() > sigTime) {
                LogPrintf("CIndexnodeBroadcast::CheckOutpoint -- Bad sigTime %d (%d conf block is at %d) for Indexnode %s %s\n",
                          sigTime, Params().GetConsensus().nIndexnodeMinimumConfirmations, pConfIndex->GetBlockTime(), vin.prevout.ToStringShort(), addr.ToString());
                return false;
            }
        }
    }

    return true;
}

bool CIndexnodeBroadcast::Sign(CKey &keyCollateralAddress) {
    std::string strError;
    std::string strMessage;

    sigTime = GetAdjustedTime();

    strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                 pubKeyCollateralAddress.GetID().ToString() + pubKeyIndexnode.GetID().ToString() +
                 boost::lexical_cast<std::string>(nProtocolVersion);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, keyCollateralAddress)) {
        LogPrintf("CIndexnodeBroadcast::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
        LogPrintf("CIndexnodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CIndexnodeBroadcast::CheckSignature(int &nDos) {
    std::string strMessage;
    std::string strError = "";
    nDos = 0;

    strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                 pubKeyCollateralAddress.GetID().ToString() + pubKeyIndexnode.GetID().ToString() +
                 boost::lexical_cast<std::string>(nProtocolVersion);

    LogPrint("indexnode", "CIndexnodeBroadcast::CheckSignature -- strMessage: %s  pubKeyCollateralAddress address: %s  sig: %s\n", strMessage, CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString(), EncodeBase64(&vchSig[0], vchSig.size()));

    if (!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
        LogPrintf("CIndexnodeBroadcast::CheckSignature -- Got bad Indexnode announce signature, error: %s\n", strError);
        nDos = 100;
        return false;
    }

    return true;
}

void CIndexnodeBroadcast::RelayIndexNode() {
    LogPrintf("CIndexnodeBroadcast::RelayIndexNode\n");
    CInv inv(MSG_INDEXNODE_ANNOUNCE, GetHash());
    RelayInv(inv);
}

CIndexnodePing::CIndexnodePing(CTxIn &vinNew) {
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    vin = vinNew;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
    vchSig = std::vector < unsigned char > ();
}

bool CIndexnodePing::Sign(CKey &keyIndexnode, CPubKey &pubKeyIndexnode) {
    std::string strError;
    std::string strIndexNodeSignMessage;

    sigTime = GetAdjustedTime();
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, keyIndexnode)) {
        LogPrintf("CIndexnodePing::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubKeyIndexnode, vchSig, strMessage, strError)) {
        LogPrintf("CIndexnodePing::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CIndexnodePing::CheckSignature(CPubKey &pubKeyIndexnode, int &nDos) {
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";
    nDos = 0;

    if (!darkSendSigner.VerifyMessage(pubKeyIndexnode, vchSig, strMessage, strError)) {
        LogPrintf("CIndexnodePing::CheckSignature -- Got bad Indexnode ping signature, indexnode=%s, error: %s\n", vin.prevout.ToStringShort(), strError);
        nDos = 33;
        return false;
    }
    return true;
}

bool CIndexnodePing::SimpleCheck(int &nDos) {
    // don't ban by default
    nDos = 0;

    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CIndexnodePing::SimpleCheck -- Signature rejected, too far into the future, indexnode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    {
//        LOCK(cs_main);
        AssertLockHeld(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint("indexnode", "CIndexnodePing::SimpleCheck -- Indexnode ping is invalid, unknown block hash: indexnode=%s blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
    }
    LogPrint("indexnode", "CIndexnodePing::SimpleCheck -- Indexnode ping verified: indexnode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);
    return true;
}

bool CIndexnodePing::CheckAndUpdate(CIndexnode *pmn, bool fFromNewBroadcast, int &nDos) {
    // don't ban by default
    nDos = 0;

    if (!SimpleCheck(nDos)) {
        return false;
    }

    if (pmn == NULL) {
        LogPrint("indexnode", "CIndexnodePing::CheckAndUpdate -- Couldn't find Indexnode entry, indexnode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    if (!fFromNewBroadcast) {
        if (pmn->IsUpdateRequired()) {
            LogPrint("indexnode", "CIndexnodePing::CheckAndUpdate -- indexnode protocol is outdated, indexnode=%s\n", vin.prevout.ToStringShort());
            return false;
        }

        if (pmn->IsNewStartRequired()) {
            LogPrint("indexnode", "CIndexnodePing::CheckAndUpdate -- indexnode is completely expired, new start is required, indexnode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            // LogPrintf("CIndexnodePing::CheckAndUpdate -- Indexnode ping is invalid, block hash is too old: indexnode=%s  blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // nDos = 1;
            return false;
        }
    }

    LogPrint("indexnode", "CIndexnodePing::CheckAndUpdate -- New ping: indexnode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);

    // LogPrintf("mnping - Found corresponding mn for vin: %s\n", vin.prevout.ToStringShort());
    // update only if there is no known ping for this indexnode or
    // last ping was more then INDEXNODE_MIN_MNP_SECONDS-60 ago comparing to this one
    if (pmn->IsPingedWithin(INDEXNODE_MIN_MNP_SECONDS - 60, sigTime)) {
        LogPrint("indexnode", "CIndexnodePing::CheckAndUpdate -- Indexnode ping arrived too early, indexnode=%s\n", vin.prevout.ToStringShort());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pmn->pubKeyIndexnode, nDos)) return false;

    // so, ping seems to be ok

    // if we are still syncing and there was no known ping for this mn for quite a while
    // (NOTE: assuming that INDEXNODE_EXPIRATION_SECONDS/2 should be enough to finish mn list sync)
    if (!indexnodeSync.IsIndexnodeListSynced() && !pmn->IsPingedWithin(INDEXNODE_EXPIRATION_SECONDS / 2)) {
        // let's bump sync timeout
        LogPrint("indexnode", "CIndexnodePing::CheckAndUpdate -- bumping sync timeout, indexnode=%s\n", vin.prevout.ToStringShort());
        indexnodeSync.AddedIndexnodeList();
        GetMainSignals().UpdatedIndexnode(*pmn);
    }

    // let's store this ping as the last one
    LogPrint("indexnode", "CIndexnodePing::CheckAndUpdate -- Indexnode ping accepted, indexnode=%s\n", vin.prevout.ToStringShort());
    pmn->SetLastPing(*this);

    // and update mnodeman.mapSeenIndexnodeBroadcast.lastPing which is probably outdated
    CIndexnodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if (mnodeman.mapSeenIndexnodeBroadcast.count(hash)) {
        mnodeman.mapSeenIndexnodeBroadcast[hash].second.SetLastPing(*this);
    }

    pmn->Check(true); // force update, ignoring cache
    if (!pmn->IsEnabled()) return false;

    LogPrint("indexnode", "CIndexnodePing::CheckAndUpdate -- Indexnode ping acceepted and relayed, indexnode=%s\n", vin.prevout.ToStringShort());
    Relay();

    return true;
}

void CIndexnodePing::Relay() {
    CInv inv(MSG_INDEXNODE_PING, GetHash());
    RelayInv(inv);
}

//void CIndexnode::AddGovernanceVote(uint256 nGovernanceObjectHash)
//{
//    if(mapGovernanceObjectsVotedOn.count(nGovernanceObjectHash)) {
//        mapGovernanceObjectsVotedOn[nGovernanceObjectHash]++;
//    } else {
//        mapGovernanceObjectsVotedOn.insert(std::make_pair(nGovernanceObjectHash, 1));
//    }
//}

//void CIndexnode::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
//{
//    std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.find(nGovernanceObjectHash);
//    if(it == mapGovernanceObjectsVotedOn.end()) {
//        return;
//    }
//    mapGovernanceObjectsVotedOn.erase(it);
//}

void CIndexnode::UpdateWatchdogVoteTime() {
    LOCK(cs);
    nTimeLastWatchdogVote = GetTime();
}

/**
*   FLAG GOVERNANCE ITEMS AS DIRTY
*
*   - When indexnode come and go on the network, we must flag the items they voted on to recalc it's cached flags
*
*/
//void CIndexnode::FlagGovernanceItemsAsDirty()
//{
//    std::vector<uint256> vecDirty;
//    {
//        std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.begin();
//        while(it != mapGovernanceObjectsVotedOn.end()) {
//            vecDirty.push_back(it->first);
//            ++it;
//        }
//    }
//    for(size_t i = 0; i < vecDirty.size(); ++i) {
//        mnodeman.AddDirtyGovernanceObjectHash(vecDirty[i]);
//    }
//}
