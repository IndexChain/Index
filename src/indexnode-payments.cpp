// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeindexnode.h"
#include "darksend.h"
#include "indexnode-payments.h"
#include "indexnode-sync.h"
#include "indexnodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

#include <boost/lexical_cast.hpp>

/** Object for who's going to get paid on which blocks */
CIndexnodePayments mnpayments;

CCriticalSection cs_vecPayees;
CCriticalSection cs_mapIndexnodeBlocks;
CCriticalSection cs_mapIndexnodePaymentVotes;

/**
* IsBlockValueValid
*
*   Determine if coinbase outgoing created money is the correct value
*
*   Why is this needed?
*   - In Index some blocks are superblocks, which output much higher amounts of coins
*   - Otherblocks are 10% lower in outgoing value, so in total, no extra coins are created
*   - When non-superblocks are detected, the normal schedule should be maintained
*/

bool IsBlockValueValid(const CBlock &block, int nBlockHeight, CAmount blockReward, std::string &strErrorRet) {
    strErrorRet = "";

    bool isBlockRewardValueMet = (block.vtx[0].GetValueOut() <= blockReward);
    if (fDebug) LogPrintf("block.vtx[0].GetValueOut() %lld <= blockReward %lld\n", block.vtx[0].GetValueOut(), blockReward);

    // we are still using budgets, but we have no data about them anymore,
    // all we know is predefined budget cycle and window

//    const Consensus::Params &consensusParams = Params().GetConsensus();
//
////    if (nBlockHeight < consensusParams.nSuperblockStartBlock) {
//        int nOffset = nBlockHeight % consensusParams.nBudgetPaymentsCycleBlocks;
//        if (nBlockHeight >= consensusParams.nBudgetPaymentsStartBlock &&
//            nOffset < consensusParams.nBudgetPaymentsWindowBlocks) {
//            // NOTE: make sure SPORK_13_OLD_SUPERBLOCK_FLAG is disabled when 12.1 starts to go live
//            if (indexnodeSync.IsSynced() && !sporkManager.IsSporkActive(SPORK_13_OLD_SUPERBLOCK_FLAG)) {
//                // no budget blocks should be accepted here, if SPORK_13_OLD_SUPERBLOCK_FLAG is disabled
//                LogPrint("gobject", "IsBlockValueValid -- Client synced but budget spork is disabled, checking block value against block reward\n");
//                if (!isBlockRewardValueMet) {
//                    strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, budgets are disabled",
//                                            nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//                }
//                return isBlockRewardValueMet;
//            }
//            LogPrint("gobject", "IsBlockValueValid -- WARNING: Skipping budget block value checks, accepting block\n");
//            // TODO: reprocess blocks to make sure they are legit?
//            return true;
//        }
//        // LogPrint("gobject", "IsBlockValueValid -- Block is not in budget cycle window, checking block value against block reward\n");
//        if (!isBlockRewardValueMet) {
//            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, block is not in budget cycle window",
//                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//        }
//        return isBlockRewardValueMet;
//    }

    // superblocks started

//    CAmount nSuperblockMaxValue =  blockReward + CSuperblock::GetPaymentsLimit(nBlockHeight);
//    bool isSuperblockMaxValueMet = (block.vtx[0].GetValueOut() <= nSuperblockMaxValue);
//    bool isSuperblockMaxValueMet = false;

//    LogPrint("gobject", "block.vtx[0].GetValueOut() %lld <= nSuperblockMaxValue %lld\n", block.vtx[0].GetValueOut(), nSuperblockMaxValue);

    if (!indexnodeSync.IsSynced()) {
        // not enough data but at least it must NOT exceed superblock max value
//        if(CSuperblock::IsValidBlockHeight(nBlockHeight)) {
//            if(fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, checking superblock max bounds only\n");
//            if(!isSuperblockMaxValueMet) {
//                strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded superblock max value",
//                                        nBlockHeight, block.vtx[0].GetValueOut(), nSuperblockMaxValue);
//            }
//            return isSuperblockMaxValueMet;
//        }
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, only regular blocks are allowed at this height",
                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
        }
        // it MUST be a regular block otherwise
        return isBlockRewardValueMet;
    }

    // we are synced, let's try to check as much data as we can

    if (sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED)) {
////        if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
////            if(CSuperblockManager::IsValid(block.vtx[0], nBlockHeight, blockReward)) {
////                LogPrint("gobject", "IsBlockValueValid -- Valid superblock at height %d: %s", nBlockHeight, block.vtx[0].ToString());
////                // all checks are done in CSuperblock::IsValid, nothing to do here
////                return true;
////            }
////
////            // triggered but invalid? that's weird
////            LogPrintf("IsBlockValueValid -- ERROR: Invalid superblock detected at height %d: %s", nBlockHeight, block.vtx[0].ToString());
////            // should NOT allow invalid superblocks, when superblocks are enabled
////            strErrorRet = strprintf("invalid superblock detected at height %d", nBlockHeight);
////            return false;
////        }
//        LogPrint("gobject", "IsBlockValueValid -- No triggered superblock detected at height %d\n", nBlockHeight);
//        if(!isBlockRewardValueMet) {
//            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, no triggered superblock detected",
//                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
//        }
    } else {
//        // should NOT allow superblocks at all, when superblocks are disabled
        LogPrint("gobject", "IsBlockValueValid -- Superblocks are disabled, no superblocks allowed\n");
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, superblocks are disabled",
                                    nBlockHeight, block.vtx[0].GetValueOut(), blockReward);
        }
    }

    // it MUST be a regular block
    return isBlockRewardValueMet;
}

bool IsBlockPayeeValid(const CTransaction &txNew, int nBlockHeight, CAmount blockReward) {
    // we can only check indexnode payment /
    const Consensus::Params &consensusParams = Params().GetConsensus();

    if (nBlockHeight < consensusParams.nIndexnodePaymentsStartBlock) {
        //there is no budget data to use to check anything, let's just accept the longest chain
        if (fDebug) LogPrintf("IsBlockPayeeValid -- indexnode isn't start\n");
        return true;
    }
    if (!indexnodeSync.IsSynced() && Params().NetworkIDString() != CBaseChainParams::REGTEST) {
        //there is no budget data to use to check anything, let's just accept the longest chain
        if (fDebug) LogPrintf("IsBlockPayeeValid -- WARNING: Client not synced, skipping block payee checks\n");
        return true;
    }

    //check for indexnode payee
    if (mnpayments.IsTransactionValid(txNew, nBlockHeight, false)) {
        LogPrint("mnpayments", "IsBlockPayeeValid -- Valid indexnode payment at height %d: %s", nBlockHeight, txNew.ToString());
        return true;
    } else {
        if(sporkManager.IsSporkActive(SPORK_8_INDEXNODE_PAYMENT_ENFORCEMENT)){
            return false;
        } else {
            LogPrintf("IndexNode payment enforcement is disabled, accepting block\n");
            return true;
        }
    }
}

void FillBlockPayments(CMutableTransaction &txNew, int nBlockHeight, CAmount indexnodePayment, CTxOut &txoutIndexnodeRet, std::vector <CTxOut> &voutSuperblockRet) {
    // only create superblocks if spork is enabled AND if superblock is actually triggered
    // (height should be validated inside)
//    if(sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED) &&
//        CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
//            LogPrint("gobject", "FillBlockPayments -- triggered superblock creation at height %d\n", nBlockHeight);
//            CSuperblockManager::CreateSuperblock(txNew, nBlockHeight, voutSuperblockRet);
//            return;
//    }

    // FILL BLOCK PAYEE WITH INDEXNODE PAYMENT OTHERWISE
    mnpayments.FillBlockPayee(txNew, nBlockHeight, indexnodePayment, txoutIndexnodeRet);
    LogPrint("mnpayments", "FillBlockPayments -- nBlockHeight %d indexnodePayment %lld txoutIndexnodeRet %s txNew %s",
             nBlockHeight, indexnodePayment, txoutIndexnodeRet.ToString(), txNew.ToString());
}

std::string GetRequiredPaymentsString(int nBlockHeight) {
    // IF WE HAVE A ACTIVATED TRIGGER FOR THIS HEIGHT - IT IS A SUPERBLOCK, GET THE REQUIRED PAYEES
//    if(CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
//        return CSuperblockManager::GetRequiredPaymentsString(nBlockHeight);
//    }

    // OTHERWISE, PAY INDEXNODE
    return mnpayments.GetRequiredPaymentsString(nBlockHeight);
}

void CIndexnodePayments::Clear() {
    LOCK2(cs_mapIndexnodeBlocks, cs_mapIndexnodePaymentVotes);
    mapIndexnodeBlocks.clear();
    mapIndexnodePaymentVotes.clear();
}

bool CIndexnodePayments::CanVote(COutPoint outIndexnode, int nBlockHeight) {
    LOCK(cs_mapIndexnodePaymentVotes);

    if (mapIndexnodesLastVote.count(outIndexnode) && mapIndexnodesLastVote[outIndexnode] == nBlockHeight) {
        return false;
    }

    //record this indexnode voted
    mapIndexnodesLastVote[outIndexnode] = nBlockHeight;
    return true;
}

std::string CIndexnodePayee::ToString() const {
    CTxDestination address1;
    ExtractDestination(scriptPubKey, address1);
    CBitcoinAddress address2(address1);
    std::string str;
    str += "(address: ";
    str += address2.ToString();
    str += ")\n";
    return str;
}

/**
*   FillBlockPayee
*
*   Fill Indexnode ONLY payment block
*/

void CIndexnodePayments::FillBlockPayee(CMutableTransaction &txNew, int nBlockHeight, CAmount indexnodePayment, CTxOut &txoutIndexnodeRet) {
    // make sure it's not filled yet
    txoutIndexnodeRet = CTxOut();

    CScript payee;
    bool foundMaxVotedPayee = true;

    if (!mnpayments.GetBlockPayee(nBlockHeight, payee)) {
        // no indexnode detected...
        // LogPrintf("no indexnode detected...\n");
        foundMaxVotedPayee = false;
        int nCount = 0;
        CIndexnode *winningNode = mnodeman.GetNextIndexnodeInQueueForPayment(nBlockHeight, true, nCount);
        if (!winningNode) {
            if(Params().NetworkIDString() != CBaseChainParams::REGTEST) {
                // ...and we can't calculate it on our own
                LogPrintf("CIndexnodePayments::FillBlockPayee -- Failed to detect indexnode to pay\n");
                return;
            }
        }
        // fill payee with locally calculated winner and hope for the best
        if (winningNode) {
            payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
            LogPrintf("payee=%s\n", winningNode->ToString());
        }
        else
            payee = txNew.vout[0].scriptPubKey;//This is only for unit tests scenario on REGTEST
    }
    txoutIndexnodeRet = CTxOut(indexnodePayment, payee);
    txNew.vout.push_back(txoutIndexnodeRet);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);
    if (foundMaxVotedPayee) {
        LogPrintf("CIndexnodePayments::FillBlockPayee::foundMaxVotedPayee -- Indexnode payment %lld to %s\n", indexnodePayment, address2.ToString());
    } else {
        LogPrintf("CIndexnodePayments::FillBlockPayee -- Indexnode payment %lld to %s\n", indexnodePayment, address2.ToString());
    }

}

int CIndexnodePayments::GetMinIndexnodePaymentsProto() {
    return sporkManager.IsSporkActive(SPORK_10_INDEXNODE_PAY_UPDATED_NODES)
           ? MIN_INDEXNODE_PAYMENT_PROTO_VERSION_2
           : MIN_INDEXNODE_PAYMENT_PROTO_VERSION_1;
}

void CIndexnodePayments::ProcessMessage(CNode *pfrom, std::string &strCommand, CDataStream &vRecv) {

//    LogPrintf("CIndexnodePayments::ProcessMessage strCommand=%s\n", strCommand);
    // Ignore any payments messages until indexnode list is synced
    if (!indexnodeSync.IsIndexnodeListSynced()) return;

    if (fLiteMode) return; // disable all Index specific functionality

    bool fTestNet = (Params().NetworkIDString() == CBaseChainParams::TESTNET);

    if (strCommand == NetMsgType::INDEXNODEPAYMENTSYNC) { //Indexnode Payments Request Sync

        // Ignore such requests until we are fully synced.
        // We could start processing this after indexnode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!indexnodeSync.IsSynced()) return;

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (netfulfilledman.HasFulfilledRequest(pfrom->addr, NetMsgType::INDEXNODEPAYMENTSYNC)) {
            // Asking for the payments list multiple times in a short period of time is no good
            LogPrintf("INDEXNODEPAYMENTSYNC -- peer already asked me for the list, peer=%d\n", pfrom->id);
            if (!fTestNet) Misbehaving(pfrom->GetId(), 20);
            return;
        }
        netfulfilledman.AddFulfilledRequest(pfrom->addr, NetMsgType::INDEXNODEPAYMENTSYNC);

        Sync(pfrom);
        LogPrint("mnpayments", "INDEXNODEPAYMENTSYNC -- Sent Indexnode payment votes to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::INDEXNODEPAYMENTVOTE) { // Indexnode Payments Vote for the Winner

        CIndexnodePaymentVote vote;
        vRecv >> vote;

        if (pfrom->nVersion < GetMinIndexnodePaymentsProto()) return;

        if (!pCurrentBlockIndex) return;

        uint256 nHash = vote.GetHash();

        pfrom->setAskFor.erase(nHash);

        {
            LOCK(cs_mapIndexnodePaymentVotes);
            if (mapIndexnodePaymentVotes.count(nHash)) {
                LogPrint("mnpayments", "INDEXNODEPAYMENTVOTE -- hash=%s, nHeight=%d seen\n", nHash.ToString(), pCurrentBlockIndex->nHeight);
                return;
            }

            // Avoid processing same vote multiple times
            mapIndexnodePaymentVotes[nHash] = vote;
            // but first mark vote as non-verified,
            // AddPaymentVote() below should take care of it if vote is actually ok
            mapIndexnodePaymentVotes[nHash].MarkAsNotVerified();
        }

        int nFirstBlock = pCurrentBlockIndex->nHeight - GetStorageLimit();
        if (vote.nBlockHeight < nFirstBlock || vote.nBlockHeight > pCurrentBlockIndex->nHeight + 20) {
            LogPrint("mnpayments", "INDEXNODEPAYMENTVOTE -- vote out of range: nFirstBlock=%d, nBlockHeight=%d, nHeight=%d\n", nFirstBlock, vote.nBlockHeight, pCurrentBlockIndex->nHeight);
            return;
        }

        std::string strError = "";
        if (!vote.IsValid(pfrom, pCurrentBlockIndex->nHeight, strError)) {
            LogPrint("mnpayments", "INDEXNODEPAYMENTVOTE -- invalid message, error: %s\n", strError);
            return;
        }

        if (!CanVote(vote.vinIndexnode.prevout, vote.nBlockHeight)) {
            LogPrintf("INDEXNODEPAYMENTVOTE -- indexnode already voted, indexnode=%s\n", vote.vinIndexnode.prevout.ToStringShort());
            return;
        }

        indexnode_info_t mnInfo = mnodeman.GetIndexnodeInfo(vote.vinIndexnode);
        if (!mnInfo.fInfoValid) {
            // mn was not found, so we can't check vote, some info is probably missing
            LogPrintf("INDEXNODEPAYMENTVOTE -- indexnode is missing %s\n", vote.vinIndexnode.prevout.ToStringShort());
            mnodeman.AskForMN(pfrom, vote.vinIndexnode);
            return;
        }

        int nDos = 0;
        if (!vote.CheckSignature(mnInfo.pubKeyIndexnode, pCurrentBlockIndex->nHeight, nDos)) {
            if (nDos) {
                LogPrintf("INDEXNODEPAYMENTVOTE -- ERROR: invalid signature\n");
                if (!fTestNet) Misbehaving(pfrom->GetId(), nDos);
            } else {
                // only warn about anything non-critical (i.e. nDos == 0) in debug mode
                LogPrint("mnpayments", "INDEXNODEPAYMENTVOTE -- WARNING: invalid signature\n");
            }
            // Either our info or vote info could be outdated.
            // In case our info is outdated, ask for an update,
            mnodeman.AskForMN(pfrom, vote.vinIndexnode);
            // but there is nothing we can do if vote info itself is outdated
            // (i.e. it was signed by a mn which changed its key),
            // so just quit here.
            return;
        }

        CTxDestination address1;
        ExtractDestination(vote.payee, address1);
        CBitcoinAddress address2(address1);

        LogPrint("mnpayments", "INDEXNODEPAYMENTVOTE -- vote: address=%s, nBlockHeight=%d, nHeight=%d, prevout=%s\n", address2.ToString(), vote.nBlockHeight, pCurrentBlockIndex->nHeight, vote.vinIndexnode.prevout.ToStringShort());

        if (AddPaymentVote(vote)) {
            vote.Relay();
            indexnodeSync.AddedPaymentVote();
        }
    }
}

bool CIndexnodePaymentVote::Sign() {
    std::string strError;
    std::string strMessage = vinIndexnode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             ScriptToAsmStr(payee);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, activeIndexnode.keyIndexnode)) {
        LogPrintf("CIndexnodePaymentVote::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!darkSendSigner.VerifyMessage(activeIndexnode.pubKeyIndexnode, vchSig, strMessage, strError)) {
        LogPrintf("CIndexnodePaymentVote::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CIndexnodePayments::GetBlockPayee(int nBlockHeight, CScript &payee) {
    if (mapIndexnodeBlocks.count(nBlockHeight)) {
        return mapIndexnodeBlocks[nBlockHeight].GetBestPayee(payee);
    }

    return false;
}

// Is this indexnode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 blocks of votes
bool CIndexnodePayments::IsScheduled(CIndexnode &mn, int nNotBlockHeight) {
    LOCK(cs_mapIndexnodeBlocks);

    if (!pCurrentBlockIndex) return false;

    CScript mnpayee;
    mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = pCurrentBlockIndex->nHeight; h <= pCurrentBlockIndex->nHeight + 8; h++) {
        if (h == nNotBlockHeight) continue;
        if (mapIndexnodeBlocks.count(h) && mapIndexnodeBlocks[h].GetBestPayee(payee) && mnpayee == payee) {
            return true;
        }
    }

    return false;
}

bool CIndexnodePayments::AddPaymentVote(const CIndexnodePaymentVote &vote) {
    LogPrint("indexnode-payments", "CIndexnodePayments::AddPaymentVote\n");
    uint256 blockHash = uint256();
    if (!GetBlockHash(blockHash, vote.nBlockHeight - 101)) return false;

    if (HasVerifiedPaymentVote(vote.GetHash())) return false;

    LOCK2(cs_mapIndexnodeBlocks, cs_mapIndexnodePaymentVotes);

    mapIndexnodePaymentVotes[vote.GetHash()] = vote;

    if (!mapIndexnodeBlocks.count(vote.nBlockHeight)) {
        CIndexnodeBlockPayees blockPayees(vote.nBlockHeight);
        mapIndexnodeBlocks[vote.nBlockHeight] = blockPayees;
    }

    mapIndexnodeBlocks[vote.nBlockHeight].AddPayee(vote);

    return true;
}

bool CIndexnodePayments::HasVerifiedPaymentVote(uint256 hashIn) {
    LOCK(cs_mapIndexnodePaymentVotes);
    std::map<uint256, CIndexnodePaymentVote>::iterator it = mapIndexnodePaymentVotes.find(hashIn);
    return it != mapIndexnodePaymentVotes.end() && it->second.IsVerified();
}

void CIndexnodeBlockPayees::AddPayee(const CIndexnodePaymentVote &vote) {
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CIndexnodePayee & payee, vecPayees)
    {
        if (payee.GetPayee() == vote.payee) {
            payee.AddVoteHash(vote.GetHash());
            return;
        }
    }
    CIndexnodePayee payeeNew(vote.payee, vote.GetHash());
    vecPayees.push_back(payeeNew);
}

bool CIndexnodeBlockPayees::GetBestPayee(CScript &payeeRet) {
    LOCK(cs_vecPayees);
    LogPrint("mnpayments", "CIndexnodeBlockPayees::GetBestPayee, vecPayees.size()=%s\n", vecPayees.size());
    if (!vecPayees.size()) {
        LogPrint("mnpayments", "CIndexnodeBlockPayees::GetBestPayee -- ERROR: couldn't find any payee\n");
        return false;
    }

    int nVotes = -1;
    BOOST_FOREACH(CIndexnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() > nVotes) {
            payeeRet = payee.GetPayee();
            nVotes = payee.GetVoteCount();
        }
    }

    return (nVotes > -1);
}

bool CIndexnodeBlockPayees::HasPayeeWithVotes(CScript payeeIn, int nVotesReq) {
    LOCK(cs_vecPayees);

    BOOST_FOREACH(CIndexnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= nVotesReq && payee.GetPayee() == payeeIn) {
            return true;
        }
    }

//    LogPrint("mnpayments", "CIndexnodeBlockPayees::HasPayeeWithVotes -- ERROR: couldn't find any payee with %d+ votes\n", nVotesReq);
    return false;
}

bool CIndexnodeBlockPayees::IsTransactionValid(const CTransaction &txNew, bool fMTP,int nHeight) {
    LOCK(cs_vecPayees);

    int nMaxSignatures = 0;
    std::string strPayeesPossible = "";


    CAmount nIndexnodePayment = GetIndexnodePayment(Params().GetConsensus(), fMTP,nHeight);

    //require at least MNPAYMENTS_SIGNATURES_REQUIRED signatures

    BOOST_FOREACH(CIndexnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= nMaxSignatures) {
            nMaxSignatures = payee.GetVoteCount();
        }
    }
    LogPrintf("nmaxsig = %s \n",nMaxSignatures);
    // if we don't have at least MNPAYMENTS_SIGNATURES_REQUIRED signatures on a payee, approve whichever is the longest chain
    if (nMaxSignatures < MNPAYMENTS_SIGNATURES_REQUIRED) return true;

    bool hasValidPayee = false;

    BOOST_FOREACH(CIndexnodePayee & payee, vecPayees)
    {
        if (payee.GetVoteCount() >= MNPAYMENTS_SIGNATURES_REQUIRED) {
            hasValidPayee = true;

            BOOST_FOREACH(CTxOut txout, txNew.vout) {
                if (payee.GetPayee() == txout.scriptPubKey && nIndexnodePayment == txout.nValue) {
                    LogPrint("mnpayments", "CIndexnodeBlockPayees::IsTransactionValid -- Found required payment\n");
                    return true;
                }
            }

            CTxDestination address1;
            ExtractDestination(payee.GetPayee(), address1);
            CBitcoinAddress address2(address1);

            if (strPayeesPossible == "") {
                strPayeesPossible = address2.ToString();
            } else {
                strPayeesPossible += "," + address2.ToString();
            }
        }
    }

    LogPrintf("CIndexnodeBlockPayees::IsTransactionValid -- ERROR: Missing required payment, possible payees: '%s', amount: %f IDX\n", strPayeesPossible, (float) nIndexnodePayment / COIN);
    return false;
}

std::string CIndexnodeBlockPayees::GetRequiredPaymentsString() {
    LOCK(cs_vecPayees);

    std::string strRequiredPayments = "Unknown";

    BOOST_FOREACH(CIndexnodePayee & payee, vecPayees)
    {
        CTxDestination address1;
        ExtractDestination(payee.GetPayee(), address1);
        CBitcoinAddress address2(address1);

        if (strRequiredPayments != "Unknown") {
            strRequiredPayments += ", " + address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        } else {
            strRequiredPayments = address2.ToString() + ":" + boost::lexical_cast<std::string>(payee.GetVoteCount());
        }
    }

    return strRequiredPayments;
}

std::string CIndexnodePayments::GetRequiredPaymentsString(int nBlockHeight) {
    LOCK(cs_mapIndexnodeBlocks);

    if (mapIndexnodeBlocks.count(nBlockHeight)) {
        return mapIndexnodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CIndexnodePayments::IsTransactionValid(const CTransaction &txNew, int nBlockHeight, bool fMTP) {
    LOCK(cs_mapIndexnodeBlocks);

    if (mapIndexnodeBlocks.count(nBlockHeight)) {
        return mapIndexnodeBlocks[nBlockHeight].IsTransactionValid(txNew, fMTP,nBlockHeight);
    }

    return true;
}

void CIndexnodePayments::CheckAndRemove() {
    if (!pCurrentBlockIndex) return;

    LOCK2(cs_mapIndexnodeBlocks, cs_mapIndexnodePaymentVotes);

    int nLimit = GetStorageLimit();

    std::map<uint256, CIndexnodePaymentVote>::iterator it = mapIndexnodePaymentVotes.begin();
    while (it != mapIndexnodePaymentVotes.end()) {
        CIndexnodePaymentVote vote = (*it).second;

        if (pCurrentBlockIndex->nHeight - vote.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CIndexnodePayments::CheckAndRemove -- Removing old Indexnode payment: nBlockHeight=%d\n", vote.nBlockHeight);
            mapIndexnodePaymentVotes.erase(it++);
            mapIndexnodeBlocks.erase(vote.nBlockHeight);
        } else {
            ++it;
        }
    }
    LogPrintf("CIndexnodePayments::CheckAndRemove -- %s\n", ToString());
}

bool CIndexnodePaymentVote::IsValid(CNode *pnode, int nValidationHeight, std::string &strError) {
    CIndexnode *pmn = mnodeman.Find(vinIndexnode);

    if (!pmn) {
        strError = strprintf("Unknown Indexnode: prevout=%s", vinIndexnode.prevout.ToStringShort());
        // Only ask if we are already synced and still have no idea about that Indexnode
        if (indexnodeSync.IsIndexnodeListSynced()) {
            mnodeman.AskForMN(pnode, vinIndexnode);
        }

        return false;
    }

    int nMinRequiredProtocol;
    if (nBlockHeight >= nValidationHeight) {
        // new votes must comply SPORK_10_INDEXNODE_PAY_UPDATED_NODES rules
        nMinRequiredProtocol = mnpayments.GetMinIndexnodePaymentsProto();
    } else {
        // allow non-updated indexnodes for old blocks
        nMinRequiredProtocol = MIN_INDEXNODE_PAYMENT_PROTO_VERSION_1;
    }

    if (pmn->nProtocolVersion < nMinRequiredProtocol) {
        strError = strprintf("Indexnode protocol is too old: nProtocolVersion=%d, nMinRequiredProtocol=%d", pmn->nProtocolVersion, nMinRequiredProtocol);
        return false;
    }

    // Only indexnodes should try to check indexnode rank for old votes - they need to pick the right winner for future blocks.
    // Regular clients (miners included) need to verify indexnode rank for future block votes only.
    if (!fIndexNode && nBlockHeight < nValidationHeight) return true;

    int nRank = mnodeman.GetIndexnodeRank(vinIndexnode, nBlockHeight - 101, nMinRequiredProtocol, false);

    if (nRank == -1) {
        LogPrint("mnpayments", "CIndexnodePaymentVote::IsValid -- Can't calculate rank for indexnode %s\n",
                 vinIndexnode.prevout.ToStringShort());
        return false;
    }

    if (nRank > MNPAYMENTS_SIGNATURES_TOTAL) {
        // It's common to have indexnodes mistakenly think they are in the top 10
        // We don't want to print all of these messages in normal mode, debug mode should print though
        strError = strprintf("Indexnode is not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL, nRank);
        // Only ban for new mnw which is out of bounds, for old mnw MN list itself might be way too much off
        if (nRank > MNPAYMENTS_SIGNATURES_TOTAL * 2 && nBlockHeight > nValidationHeight) {
            strError = strprintf("Indexnode is not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, nRank);
            LogPrintf("CIndexnodePaymentVote::IsValid -- Error: %s\n", strError);
            Misbehaving(pnode->GetId(), 20);
        }
        // Still invalid however
        return false;
    }

    return true;
}

bool CIndexnodePayments::ProcessBlock(int nBlockHeight) {

    // DETERMINE IF WE SHOULD BE VOTING FOR THE NEXT PAYEE

    if (fLiteMode || !fIndexNode) {
        return false;
    }

    // We have little chances to pick the right winner if winners list is out of sync
    // but we have no choice, so we'll try. However it doesn't make sense to even try to do so
    // if we have not enough data about indexnodes.
    if (!indexnodeSync.IsIndexnodeListSynced()) {
        return false;
    }

    int nRank = mnodeman.GetIndexnodeRank(activeIndexnode.vin, nBlockHeight - 101, GetMinIndexnodePaymentsProto(), false);

    if (nRank == -1) {
        LogPrint("mnpayments", "CIndexnodePayments::ProcessBlock -- Unknown Indexnode\n");
        return false;
    }

    if (nRank > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("mnpayments", "CIndexnodePayments::ProcessBlock -- Indexnode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, nRank);
        return false;
    }

    // LOCATE THE NEXT INDEXNODE WHICH SHOULD BE PAID

    LogPrintf("CIndexnodePayments::ProcessBlock -- Start: nBlockHeight=%d, indexnode=%s\n", nBlockHeight, activeIndexnode.vin.prevout.ToStringShort());

    // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
    int nCount = 0;
    CIndexnode *pmn = mnodeman.GetNextIndexnodeInQueueForPayment(nBlockHeight, true, nCount);

    if (pmn == NULL) {
        LogPrintf("CIndexnodePayments::ProcessBlock -- ERROR: Failed to find indexnode to pay\n");
        return false;
    }

    LogPrintf("CIndexnodePayments::ProcessBlock -- Indexnode found by GetNextIndexnodeInQueueForPayment(): %s\n", pmn->vin.prevout.ToStringShort());


    CScript payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());

    CIndexnodePaymentVote voteNew(activeIndexnode.vin, nBlockHeight, payee);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CBitcoinAddress address2(address1);

    // SIGN MESSAGE TO NETWORK WITH OUR INDEXNODE KEYS

    if (voteNew.Sign()) {
        if (AddPaymentVote(voteNew)) {
            voteNew.Relay();
            return true;
        }
    }

    return false;
}

void CIndexnodePaymentVote::Relay() {
    // do not relay until synced
    if (!indexnodeSync.IsWinnersListSynced()) {
        LogPrint("indexnode", "CIndexnodePaymentVote::Relay - indexnodeSync.IsWinnersListSynced() not sync\n");
        return;
    }
    CInv inv(MSG_INDEXNODE_PAYMENT_VOTE, GetHash());
    RelayInv(inv);
}

bool CIndexnodePaymentVote::CheckSignature(const CPubKey &pubKeyIndexnode, int nValidationHeight, int &nDos) {
    // do not ban by default
    nDos = 0;

    std::string strMessage = vinIndexnode.prevout.ToStringShort() +
                             boost::lexical_cast<std::string>(nBlockHeight) +
                             ScriptToAsmStr(payee);

    std::string strError = "";
    if (!darkSendSigner.VerifyMessage(pubKeyIndexnode, vchSig, strMessage, strError)) {
        // Only ban for future block vote when we are already synced.
        // Otherwise it could be the case when MN which signed this vote is using another key now
        // and we have no idea about the old one.
        if (indexnodeSync.IsIndexnodeListSynced() && nBlockHeight > nValidationHeight) {
            nDos = 20;
        }
        return error("CIndexnodePaymentVote::CheckSignature -- Got bad Indexnode payment signature, indexnode=%s, error: %s", vinIndexnode.prevout.ToStringShort().c_str(), strError);
    }

    return true;
}

std::string CIndexnodePaymentVote::ToString() const {
    std::ostringstream info;

    info << vinIndexnode.prevout.ToStringShort() <<
         ", " << nBlockHeight <<
         ", " << ScriptToAsmStr(payee) <<
         ", " << (int) vchSig.size();

    return info.str();
}

// Send only votes for future blocks, node should request every other missing payment block individually
void CIndexnodePayments::Sync(CNode *pnode) {
    LOCK(cs_mapIndexnodeBlocks);

    if (!pCurrentBlockIndex) return;

    int nInvCount = 0;

    for (int h = pCurrentBlockIndex->nHeight; h < pCurrentBlockIndex->nHeight + 20; h++) {
        if (mapIndexnodeBlocks.count(h)) {
            BOOST_FOREACH(CIndexnodePayee & payee, mapIndexnodeBlocks[h].vecPayees)
            {
                std::vector <uint256> vecVoteHashes = payee.GetVoteHashes();
                BOOST_FOREACH(uint256 & hash, vecVoteHashes)
                {
                    if (!HasVerifiedPaymentVote(hash)) continue;
                    pnode->PushInventory(CInv(MSG_INDEXNODE_PAYMENT_VOTE, hash));
                    nInvCount++;
                }
            }
        }
    }

    LogPrintf("CIndexnodePayments::Sync -- Sent %d votes to peer %d\n", nInvCount, pnode->id);
    pnode->PushMessage(NetMsgType::SYNCSTATUSCOUNT, INDEXNODE_SYNC_MNW, nInvCount);
}

// Request low data/unknown payment blocks in batches directly from some node instead of/after preliminary Sync.
void CIndexnodePayments::RequestLowDataPaymentBlocks(CNode *pnode) {
    if (!pCurrentBlockIndex) return;

    LOCK2(cs_main, cs_mapIndexnodeBlocks);

    std::vector <CInv> vToFetch;
    int nLimit = GetStorageLimit();

    const CBlockIndex *pindex = pCurrentBlockIndex;

    while (pCurrentBlockIndex->nHeight - pindex->nHeight < nLimit) {
        if (!mapIndexnodeBlocks.count(pindex->nHeight)) {
            // We have no idea about this block height, let's ask
            vToFetch.push_back(CInv(MSG_INDEXNODE_PAYMENT_BLOCK, pindex->GetBlockHash()));
            // We should not violate GETDATA rules
            if (vToFetch.size() == MAX_INV_SZ) {
                LogPrintf("CIndexnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d blocks\n", pnode->id, MAX_INV_SZ);
                pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
                // Start filling new batch
                vToFetch.clear();
            }
        }
        if (!pindex->pprev) break;
        pindex = pindex->pprev;
    }

    std::map<int, CIndexnodeBlockPayees>::iterator it = mapIndexnodeBlocks.begin();

    while (it != mapIndexnodeBlocks.end()) {
        int nTotalVotes = 0;
        bool fFound = false;
        BOOST_FOREACH(CIndexnodePayee & payee, it->second.vecPayees)
        {
            if (payee.GetVoteCount() >= MNPAYMENTS_SIGNATURES_REQUIRED) {
                fFound = true;
                break;
            }
            nTotalVotes += payee.GetVoteCount();
        }
        // A clear winner (MNPAYMENTS_SIGNATURES_REQUIRED+ votes) was found
        // or no clear winner was found but there are at least avg number of votes
        if (fFound || nTotalVotes >= (MNPAYMENTS_SIGNATURES_TOTAL + MNPAYMENTS_SIGNATURES_REQUIRED) / 2) {
            // so just move to the next block
            ++it;
            continue;
        }
        // DEBUG
//        DBG (
//            // Let's see why this failed
//            BOOST_FOREACH(CIndexnodePayee& payee, it->second.vecPayees) {
//                CTxDestination address1;
//                ExtractDestination(payee.GetPayee(), address1);
//                CBitcoinAddress address2(address1);
//                printf("payee %s votes %d\n", address2.ToString().c_str(), payee.GetVoteCount());
//            }
//            printf("block %d votes total %d\n", it->first, nTotalVotes);
//        )
        // END DEBUG
        // Low data block found, let's try to sync it
        uint256 hash;
        if (GetBlockHash(hash, it->first)) {
            vToFetch.push_back(CInv(MSG_INDEXNODE_PAYMENT_BLOCK, hash));
        }
        // We should not violate GETDATA rules
        if (vToFetch.size() == MAX_INV_SZ) {
            LogPrintf("CIndexnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, MAX_INV_SZ);
            pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
            // Start filling new batch
            vToFetch.clear();
        }
        ++it;
    }
    // Ask for the rest of it
    if (!vToFetch.empty()) {
        LogPrintf("CIndexnodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, vToFetch.size());
        pnode->PushMessage(NetMsgType::GETDATA, vToFetch);
    }
}

std::string CIndexnodePayments::ToString() const {
    std::ostringstream info;

    info << "Votes: " << (int) mapIndexnodePaymentVotes.size() <<
         ", Blocks: " << (int) mapIndexnodeBlocks.size();

    return info.str();
}

bool CIndexnodePayments::IsEnoughData() {
    float nAverageVotes = (MNPAYMENTS_SIGNATURES_TOTAL + MNPAYMENTS_SIGNATURES_REQUIRED) / 2;
    int nStorageLimit = GetStorageLimit();
    return GetBlockCount() > nStorageLimit && GetVoteCount() > nStorageLimit * nAverageVotes;
}

int CIndexnodePayments::GetStorageLimit() {
    return std::max(int(mnodeman.size() * nStorageCoeff), nMinBlocksToStore);
}

void CIndexnodePayments::UpdatedBlockTip(const CBlockIndex *pindex) {
    pCurrentBlockIndex = pindex;
    LogPrint("mnpayments", "CIndexnodePayments::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);
    
    ProcessBlock(pindex->nHeight + 5);
}
