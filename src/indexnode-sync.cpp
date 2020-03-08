// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeindexnode.h"
#include "checkpoints.h"
#include "main.h"
#include "indexnode.h"
#include "indexnode-payments.h"
#include "indexnode-sync.h"
#include "indexnodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"
#include "validationinterface.h"

class CIndexnodeSync;

CIndexnodeSync indexnodeSync;

bool CIndexnodeSync::CheckNodeHeight(CNode *pnode, bool fDisconnectStuckNodes) {
    CNodeStateStats stats;
    if (!GetNodeStateStats(pnode->id, stats) || stats.nCommonHeight == -1 || stats.nSyncHeight == -1) return false; // not enough info about this peer

    // Check blocks and headers, allow a small error margin of 1 block
    if (pCurrentBlockIndex->nHeight - 1 > stats.nCommonHeight) {
        // This peer probably stuck, don't sync any additional data from it
        if (fDisconnectStuckNodes) {
            // Disconnect to free this connection slot for another peer.
            pnode->fDisconnect = true;
            LogPrintf("CIndexnodeSync::CheckNodeHeight -- disconnecting from stuck peer, nHeight=%d, nCommonHeight=%d, peer=%d\n",
                      pCurrentBlockIndex->nHeight, stats.nCommonHeight, pnode->id);
        } else {
            LogPrintf("CIndexnodeSync::CheckNodeHeight -- skipping stuck peer, nHeight=%d, nCommonHeight=%d, peer=%d\n",
                      pCurrentBlockIndex->nHeight, stats.nCommonHeight, pnode->id);
        }
        return false;
    } else if (pCurrentBlockIndex->nHeight < stats.nSyncHeight - 1) {
        // This peer announced more headers than we have blocks currently
        LogPrint("indexnode", "CIndexnodeSync::CheckNodeHeight -- skipping peer, who announced more headers than we have blocks currently, nHeight=%d, nSyncHeight=%d, peer=%d\n",
                  pCurrentBlockIndex->nHeight, stats.nSyncHeight, pnode->id);
        return false;
    }

    return true;
}

bool CIndexnodeSync::GetBlockchainSynced(bool fBlockAccepted){
    bool currentBlockchainSynced = fBlockchainSynced;
    IsBlockchainSynced(fBlockAccepted);
    if(currentBlockchainSynced != fBlockchainSynced){
        GetMainSignals().UpdateSyncStatus();
    }
    return fBlockchainSynced;
}

bool CIndexnodeSync::IsBlockchainSynced(bool fBlockAccepted) {
    static int64_t nTimeLastProcess = GetTime();
    static int nSkipped = 0;
    static bool fFirstBlockAccepted = false;

    // If the last call to this function was more than 60 minutes ago 
    // (client was in sleep mode) reset the sync process
    if (GetTime() - nTimeLastProcess > 60 * 60) {
        LogPrintf("CIndexnodeSync::IsBlockchainSynced time-check fBlockchainSynced=%s\n", 
                  fBlockchainSynced);
        Reset();
        fBlockchainSynced = false;
    }

    if (!pCurrentBlockIndex || !pindexBestHeader || fImporting || fReindex) 
        return false;

    if (fBlockAccepted) {
        // This should be only triggered while we are still syncing.
        if (!IsSynced()) {
            // We are trying to download smth, reset blockchain sync status.
            fFirstBlockAccepted = true;
            fBlockchainSynced = false;
            nTimeLastProcess = GetTime();
            return false;
        }
    } else {
        // Dont skip on REGTEST to make the tests run faster.
        if(Params().NetworkIDString() != CBaseChainParams::REGTEST) {
            // skip if we already checked less than 1 tick ago.
            if (GetTime() - nTimeLastProcess < INDEXNODE_SYNC_TICK_SECONDS) {
                nSkipped++;
                return fBlockchainSynced;
            }
        }
    }

    LogPrint("indexnode-sync", 
             "CIndexnodeSync::IsBlockchainSynced -- state before check: %ssynced, skipped %d times\n", 
             fBlockchainSynced ? "" : "not ", 
             nSkipped);

    nTimeLastProcess = GetTime();
    nSkipped = 0;

    if (fBlockchainSynced){
        return true;
    }

    if (fCheckpointsEnabled && 
        pCurrentBlockIndex->nHeight < Checkpoints::GetTotalBlocksEstimate(Params().Checkpoints())) {
        
        return false;
    }

    std::vector < CNode * > vNodesCopy = CopyNodeVector();
    // We have enough peers and assume most of them are synced
    if (vNodesCopy.size() >= INDEXNODE_SYNC_ENOUGH_PEERS) {
        // Check to see how many of our peers are (almost) at the same height as we are
        int nNodesAtSameHeight = 0;
        BOOST_FOREACH(CNode * pnode, vNodesCopy)
        {
            // Make sure this peer is presumably at the same height
            if (!CheckNodeHeight(pnode)) {
                continue;
            }
            nNodesAtSameHeight++;
            // if we have decent number of such peers, most likely we are synced now
            if (nNodesAtSameHeight >= INDEXNODE_SYNC_ENOUGH_PEERS) {
                LogPrintf("CIndexnodeSync::IsBlockchainSynced -- found enough peers on the same height as we are, done\n");
                fBlockchainSynced = true;
                ReleaseNodeVector(vNodesCopy);
                return fBlockchainSynced;
            }
        }
    }
    ReleaseNodeVector(vNodesCopy);

    // wait for at least one new block to be accepted
    if (!fFirstBlockAccepted){ 
        fBlockchainSynced = false;
        return false;
    }

    // same as !IsInitialBlockDownload() but no cs_main needed here
    int64_t nMaxBlockTime = std::max(pCurrentBlockIndex->GetBlockTime(), pindexBestHeader->GetBlockTime());
    fBlockchainSynced = pindexBestHeader->nHeight - pCurrentBlockIndex->nHeight < 24 * 6 &&
                        GetTime() - nMaxBlockTime < Params().MaxTipAge();
    return fBlockchainSynced;
}

void CIndexnodeSync::Fail() {
    nTimeLastFailure = GetTime();
    nRequestedIndexnodeAssets = INDEXNODE_SYNC_FAILED;
    GetMainSignals().UpdateSyncStatus();
}

void CIndexnodeSync::Reset() {
    nRequestedIndexnodeAssets = INDEXNODE_SYNC_INITIAL;
    nRequestedIndexnodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    nTimeLastIndexnodeList = GetTime();
    nTimeLastPaymentVote = GetTime();
    nTimeLastGovernanceItem = GetTime();
    nTimeLastFailure = 0;
    nCountFailures = 0;
}

std::string CIndexnodeSync::GetAssetName() {
    switch (nRequestedIndexnodeAssets) {
        case (INDEXNODE_SYNC_INITIAL):
            return "INDEXNODE_SYNC_INITIAL";
        case (INDEXNODE_SYNC_SPORKS):
            return "INDEXNODE_SYNC_SPORKS";
        case (INDEXNODE_SYNC_LIST):
            return "INDEXNODE_SYNC_LIST";
        case (INDEXNODE_SYNC_MNW):
            return "INDEXNODE_SYNC_MNW";
        case (INDEXNODE_SYNC_FAILED):
            return "INDEXNODE_SYNC_FAILED";
        case INDEXNODE_SYNC_FINISHED:
            return "INDEXNODE_SYNC_FINISHED";
        default:
            return "UNKNOWN";
    }
}

void CIndexnodeSync::SwitchToNextAsset() {
    switch (nRequestedIndexnodeAssets) {
        case (INDEXNODE_SYNC_FAILED):
            throw std::runtime_error("Can't switch to next asset from failed, should use Reset() first!");
            break;
        case (INDEXNODE_SYNC_INITIAL):
            ClearFulfilledRequests();
            nRequestedIndexnodeAssets = INDEXNODE_SYNC_SPORKS;
            LogPrintf("CIndexnodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case (INDEXNODE_SYNC_SPORKS):
            nTimeLastIndexnodeList = GetTime();
            nRequestedIndexnodeAssets = INDEXNODE_SYNC_LIST;
            LogPrintf("CIndexnodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case (INDEXNODE_SYNC_LIST):
            nTimeLastPaymentVote = GetTime();
            nRequestedIndexnodeAssets = INDEXNODE_SYNC_MNW;
            LogPrintf("CIndexnodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;

        case (INDEXNODE_SYNC_MNW):
            nTimeLastGovernanceItem = GetTime();
            LogPrintf("CIndexnodeSync::SwitchToNextAsset -- Sync has finished\n");
            nRequestedIndexnodeAssets = INDEXNODE_SYNC_FINISHED;
            break;
    }
    nRequestedIndexnodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    GetMainSignals().UpdateSyncStatus();
}

std::string CIndexnodeSync::GetSyncStatus() {
    switch (indexnodeSync.nRequestedIndexnodeAssets) {
        case INDEXNODE_SYNC_INITIAL:
            return _("Synchronization pending...");
        case INDEXNODE_SYNC_SPORKS:
            return _("Synchronizing sporks...");
        case INDEXNODE_SYNC_LIST:
            return _("Synchronizing indexnodes...");
        case INDEXNODE_SYNC_MNW:
            return _("Synchronizing indexnode payments...");
        case INDEXNODE_SYNC_FAILED:
            return _("Synchronization failed");
        case INDEXNODE_SYNC_FINISHED:
            return _("Synchronization finished");
        default:
            return "";
    }
}

void CIndexnodeSync::ProcessMessage(CNode *pfrom, std::string &strCommand, CDataStream &vRecv) {
    if (strCommand == NetMsgType::SYNCSTATUSCOUNT) { //Sync status count

        //do not care about stats if sync process finished or failed
        if (IsSynced() || IsFailed()) return;

        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        LogPrintf("SYNCSTATUSCOUNT -- got inventory count: nItemID=%d  nCount=%d  peer=%d\n", nItemID, nCount, pfrom->id);
    }
}

void CIndexnodeSync::ClearFulfilledRequests() {
    TRY_LOCK(cs_vNodes, lockRecv);
    if (!lockRecv) return;

    BOOST_FOREACH(CNode * pnode, vNodes)
    {
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "spork-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "indexnode-list-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "indexnode-payment-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "full-sync");
    }
}

void CIndexnodeSync::ProcessTick() {
    static int nTick = 0;
    if (nTick++ % INDEXNODE_SYNC_TICK_SECONDS != 0) return;
    if (!pCurrentBlockIndex) return;

    //the actual count of indexnodes we have currently
    int nMnCount = mnodeman.CountIndexnodes();

    LogPrint("ProcessTick", "CIndexnodeSync::ProcessTick -- nTick %d nMnCount %d\n", nTick, nMnCount);

    // INITIAL SYNC SETUP / LOG REPORTING
    double nSyncProgress = double(nRequestedIndexnodeAttempt + (nRequestedIndexnodeAssets - 1) * 8) / (8 * 4);
    LogPrint("ProcessTick", "CIndexnodeSync::ProcessTick -- nTick %d nRequestedIndexnodeAssets %d nRequestedIndexnodeAttempt %d nSyncProgress %f\n", nTick, nRequestedIndexnodeAssets, nRequestedIndexnodeAttempt, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(pCurrentBlockIndex->nHeight, nSyncProgress);

    // RESET SYNCING INCASE OF FAILURE
    {
        if (IsSynced()) {
            /*
                Resync if we lost all indexnodes from sleep/wake or failed to sync originally
            */
            if (nMnCount == 0) {
                LogPrintf("CIndexnodeSync::ProcessTick -- WARNING: not enough data, restarting sync\n");
                Reset();
            } else {
                std::vector < CNode * > vNodesCopy = CopyNodeVector();
                ReleaseNodeVector(vNodesCopy);
                return;
            }
        }

        //try syncing again
        if (IsFailed()) {
            if (nTimeLastFailure + (1 * 60) < GetTime()) { // 1 minute cooldown after failed sync
                Reset();
            }
            return;
        }
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && !IsBlockchainSynced() && nRequestedIndexnodeAssets > INDEXNODE_SYNC_SPORKS) {
        nTimeLastIndexnodeList = GetTime();
        nTimeLastPaymentVote = GetTime();
        nTimeLastGovernanceItem = GetTime();
        return;
    }
    if (nRequestedIndexnodeAssets == INDEXNODE_SYNC_INITIAL || (nRequestedIndexnodeAssets == INDEXNODE_SYNC_SPORKS && IsBlockchainSynced())) {
        SwitchToNextAsset();
    }

    std::vector < CNode * > vNodesCopy = CopyNodeVector();

    BOOST_FOREACH(CNode * pnode, vNodesCopy)
    {
        // Don't try to sync any data from outbound "indexnode" connections -
        // they are temporary and should be considered unreliable for a sync process.
        // Inbound connection this early is most likely a "indexnode" connection
        // initialted from another node, so skip it too.
        if (pnode->fIndexnode || (fIndexNode && pnode->fInbound)) continue;

        // QUICK MODE (REGTEST ONLY!)
        if (Params().NetworkIDString() == CBaseChainParams::REGTEST) {
            if (nRequestedIndexnodeAttempt <= 2) {
                pnode->PushMessage(NetMsgType::GETSPORKS); //get current network sporks
            } else if (nRequestedIndexnodeAttempt < 4) {
                mnodeman.DsegUpdate(pnode);
            } else if (nRequestedIndexnodeAttempt < 6) {
                int nMnCount = mnodeman.CountIndexnodes();
                pnode->PushMessage(NetMsgType::INDEXNODEPAYMENTSYNC, nMnCount); //sync payment votes
            } else {
                nRequestedIndexnodeAssets = INDEXNODE_SYNC_FINISHED;
                GetMainSignals().UpdateSyncStatus();
            }
            nRequestedIndexnodeAttempt++;
            ReleaseNodeVector(vNodesCopy);
            return;
        }

        // NORMAL NETWORK MODE - TESTNET/MAINNET
        {
            if (netfulfilledman.HasFulfilledRequest(pnode->addr, "full-sync")) {
                // We already fully synced from this node recently,
                // disconnect to free this connection slot for another peer.
                pnode->fDisconnect = true;
                LogPrintf("CIndexnodeSync::ProcessTick -- disconnecting from recently synced peer %d\n", pnode->id);
                continue;
            }

            // SPORK : ALWAYS ASK FOR SPORKS AS WE SYNC (we skip this mode now)

            if (!netfulfilledman.HasFulfilledRequest(pnode->addr, "spork-sync")) {
                // only request once from each peer
                netfulfilledman.AddFulfilledRequest(pnode->addr, "spork-sync");
                // get current network sporks
                pnode->PushMessage(NetMsgType::GETSPORKS);
                LogPrintf("CIndexnodeSync::ProcessTick -- nTick %d nRequestedIndexnodeAssets %d -- requesting sporks from peer %d\n", nTick, nRequestedIndexnodeAssets, pnode->id);
                continue; // always get sporks first, switch to the next node without waiting for the next tick
            }

            // MNLIST : SYNC INDEXNODE LIST FROM OTHER CONNECTED CLIENTS

            if (nRequestedIndexnodeAssets == INDEXNODE_SYNC_LIST) {
                // check for timeout first
                if (nTimeLastIndexnodeList < GetTime() - INDEXNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CIndexnodeSync::ProcessTick -- nTick %d nRequestedIndexnodeAssets %d -- timeout\n", nTick, nRequestedIndexnodeAssets);
                    if (nRequestedIndexnodeAttempt == 0) {
                        LogPrintf("CIndexnodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // there is no way we can continue without indexnode list, fail here and try later
                        Fail();
                        ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if (netfulfilledman.HasFulfilledRequest(pnode->addr, "indexnode-list-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "indexnode-list-sync");

                if (pnode->nVersion < mnpayments.GetMinIndexnodePaymentsProto()) continue;
                nRequestedIndexnodeAttempt++;

                mnodeman.DsegUpdate(pnode);

                ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // MNW : SYNC INDEXNODE PAYMENT VOTES FROM OTHER CONNECTED CLIENTS

            if (nRequestedIndexnodeAssets == INDEXNODE_SYNC_MNW) {
                LogPrint("mnpayments", "CIndexnodeSync::ProcessTick -- nTick %d nRequestedIndexnodeAssets %d nTimeLastPaymentVote %lld GetTime() %lld diff %lld\n", nTick, nRequestedIndexnodeAssets, nTimeLastPaymentVote, GetTime(), GetTime() - nTimeLastPaymentVote);
                // check for timeout first
                // This might take a lot longer than INDEXNODE_SYNC_TIMEOUT_SECONDS minutes due to new blocks,
                // but that should be OK and it should timeout eventually.
                if (nTimeLastPaymentVote < GetTime() - INDEXNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CIndexnodeSync::ProcessTick -- nTick %d nRequestedIndexnodeAssets %d -- timeout\n", nTick, nRequestedIndexnodeAssets);
                    if (nRequestedIndexnodeAttempt == 0) {
                        LogPrintf("CIndexnodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // probably not a good idea to proceed without winner list
                        Fail();
                        ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // check for data
                // if mnpayments already has enough blocks and votes, switch to the next asset
                // try to fetch data from at least two peers though
                if (nRequestedIndexnodeAttempt > 1 && mnpayments.IsEnoughData()) {
                    LogPrintf("CIndexnodeSync::ProcessTick -- nTick %d nRequestedIndexnodeAssets %d -- found enough data\n", nTick, nRequestedIndexnodeAssets);
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if (netfulfilledman.HasFulfilledRequest(pnode->addr, "indexnode-payment-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "indexnode-payment-sync");

                if (pnode->nVersion < mnpayments.GetMinIndexnodePaymentsProto()) continue;
                nRequestedIndexnodeAttempt++;

                // ask node for all payment votes it has (new nodes will only return votes for future payments)
                pnode->PushMessage(NetMsgType::INDEXNODEPAYMENTSYNC, mnpayments.GetStorageLimit());
                // ask node for missing pieces only (old nodes will not be asked)
                mnpayments.RequestLowDataPaymentBlocks(pnode);

                ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

        }
    }
    // looped through all nodes, release them
    ReleaseNodeVector(vNodesCopy);
}

void CIndexnodeSync::UpdatedBlockTip(const CBlockIndex *pindex) {
    pCurrentBlockIndex = pindex;
}
