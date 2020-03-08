// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeindexnode.h"
#include "consensus/consensus.h"
#include "indexnode.h"
#include "indexnode-sync.h"
#include "indexnode-payments.h"
#include "indexnodeman.h"
#include "protocol.h"
#include "validationinterface.h"

extern CWallet *pwalletMain;

// Keep track of the active Indexnode
CActiveIndexnode activeIndexnode;

void CActiveIndexnode::ManageState() {
    LogPrint("indexnode", "CActiveIndexnode::ManageState -- Start\n");
    if (!fIndexNode) {
        LogPrint("indexnode", "CActiveIndexnode::ManageState -- Not a indexnode, returning\n");
        return;
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && !indexnodeSync.GetBlockchainSynced()) {
        ChangeState(ACTIVE_INDEXNODE_SYNC_IN_PROCESS);
        LogPrintf("CActiveIndexnode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if (nState == ACTIVE_INDEXNODE_SYNC_IN_PROCESS) {
        ChangeState(ACTIVE_INDEXNODE_INITIAL);
    }

    LogPrint("indexnode", "CActiveIndexnode::ManageState -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    if (eType == INDEXNODE_UNKNOWN) {
        ManageStateInitial();
    }

    if (eType == INDEXNODE_REMOTE) {
        ManageStateRemote();
    } else if (eType == INDEXNODE_LOCAL) {
        // Try Remote Start first so the started local indexnode can be restarted without recreate indexnode broadcast.
        ManageStateRemote();
        if (nState != ACTIVE_INDEXNODE_STARTED)
            ManageStateLocal();
    }

    SendIndexnodePing();
}

std::string CActiveIndexnode::GetStateString() const {
    switch (nState) {
        case ACTIVE_INDEXNODE_INITIAL:
            return "INITIAL";
        case ACTIVE_INDEXNODE_SYNC_IN_PROCESS:
            return "SYNC_IN_PROCESS";
        case ACTIVE_INDEXNODE_INPUT_TOO_NEW:
            return "INPUT_TOO_NEW";
        case ACTIVE_INDEXNODE_NOT_CAPABLE:
            return "NOT_CAPABLE";
        case ACTIVE_INDEXNODE_STARTED:
            return "STARTED";
        default:
            return "UNKNOWN";
    }
}

void CActiveIndexnode::ChangeState(int state) {
    if(nState!=state){
        nState = state;
    }
}

std::string CActiveIndexnode::GetStatus() const {
    switch (nState) {
        case ACTIVE_INDEXNODE_INITIAL:
            return "Node just started, not yet activated";
        case ACTIVE_INDEXNODE_SYNC_IN_PROCESS:
            return "Sync in progress. Must wait until sync is complete to start Indexnode";
        case ACTIVE_INDEXNODE_INPUT_TOO_NEW:
            return strprintf("Indexnode input must have at least %d confirmations",
                             Params().GetConsensus().nIndexnodeMinimumConfirmations);
        case ACTIVE_INDEXNODE_NOT_CAPABLE:
            return "Not capable indexnode: " + strNotCapableReason;
        case ACTIVE_INDEXNODE_STARTED:
            return "Indexnode successfully started";
        default:
            return "Unknown";
    }
}

std::string CActiveIndexnode::GetTypeString() const {
    std::string strType;
    switch (eType) {
        case INDEXNODE_UNKNOWN:
            strType = "UNKNOWN";
            break;
        case INDEXNODE_REMOTE:
            strType = "REMOTE";
            break;
        case INDEXNODE_LOCAL:
            strType = "LOCAL";
            break;
        default:
            strType = "UNKNOWN";
            break;
    }
    return strType;
}

bool CActiveIndexnode::SendIndexnodePing() {
    if (!fPingerEnabled) {
        LogPrint("indexnode",
                 "CActiveIndexnode::SendIndexnodePing -- %s: indexnode ping service is disabled, skipping...\n",
                 GetStateString());
        return false;
    }

    if (!mnodeman.Has(vin)) {
        strNotCapableReason = "Indexnode not in indexnode list";
        ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
        LogPrintf("CActiveIndexnode::SendIndexnodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CIndexnodePing mnp(vin);
    if (!mnp.Sign(keyIndexnode, pubKeyIndexnode)) {
        LogPrintf("CActiveIndexnode::SendIndexnodePing -- ERROR: Couldn't sign Indexnode Ping\n");
        return false;
    }

    // Update lastPing for our indexnode in Indexnode list
    if (mnodeman.IsIndexnodePingedWithin(vin, INDEXNODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveIndexnode::SendIndexnodePing -- Too early to send Indexnode Ping\n");
        return false;
    }

    mnodeman.SetIndexnodeLastPing(vin, mnp);

    LogPrintf("CActiveIndexnode::SendIndexnodePing -- Relaying ping, collateral=%s\n", vin.ToString());
    mnp.Relay();

    return true;
}

void CActiveIndexnode::ManageStateInitial() {
    LogPrint("indexnode", "CActiveIndexnode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
        strNotCapableReason = "Indexnode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    bool fFoundLocal = false;
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CIndexnode::IsValidNetAddr(service);
        if (!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode * pnode, vNodes)
            {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CIndexnode::IsValidNetAddr(service);
                    if (fFoundLocal) break;
                }
            }
        }
    }

    if (!fFoundLocal) {
        ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(),
                                            mainnetDefaultPort);
            LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(),
                                        mainnetDefaultPort);
        LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveIndexnode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());
    //TODO
    if (!ConnectNode(CAddress(service, NODE_NETWORK), NULL, false, true)) {
        ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = INDEXNODE_REMOTE;

    // Check if wallet funds are available
    if (!pwalletMain) {
        LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: Wallet not available\n", GetStateString());
        return;
    }

    if (pwalletMain->IsLocked()) {
        LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: Wallet is locked\n", GetStateString());
        return;
    }

    if (pwalletMain->GetBalance() < INDEXNODE_COIN_REQUIRED * COIN) {
        LogPrintf("CActiveIndexnode::ManageStateInitial -- %s: Wallet balance is < 1000 IDX\n", GetStateString());
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    // If collateral is found switch to LOCAL mode
    if (pwalletMain->GetIndexnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        eType = INDEXNODE_LOCAL;
    }

    LogPrint("indexnode", "CActiveIndexnode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveIndexnode::ManageStateRemote() {
    LogPrint("indexnode",
             "CActiveIndexnode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyIndexnode.GetID() = %s\n",
             GetStatus(), fPingerEnabled, GetTypeString(), pubKeyIndexnode.GetID().ToString());

    mnodeman.CheckIndexnode(pubKeyIndexnode);
    indexnode_info_t infoMn = mnodeman.GetIndexnodeInfo(pubKeyIndexnode);

    if (infoMn.fInfoValid) {
        if (infoMn.nProtocolVersion < MIN_INDEXNODE_PAYMENT_PROTO_VERSION_1 || infoMn.nProtocolVersion > MIN_INDEXNODE_PAYMENT_PROTO_VERSION_2) {
            ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveIndexnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (service != infoMn.addr) {
            ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
            // LogPrintf("service: %s\n", service.ToString());
            // LogPrintf("infoMn.addr: %s\n", infoMn.addr.ToString());
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this indexnode changed recently.";
            LogPrintf("CActiveIndexnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (!CIndexnode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
            strNotCapableReason = strprintf("Indexnode in %s state", CIndexnode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveIndexnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (nState != ACTIVE_INDEXNODE_STARTED) {
            LogPrintf("CActiveIndexnode::ManageStateRemote -- STARTED!\n");
            vin = infoMn.vin;
            service = infoMn.addr;
            fPingerEnabled = true;
            ChangeState(ACTIVE_INDEXNODE_STARTED);
        }
    } else {
        ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
        strNotCapableReason = "Indexnode not in indexnode list";
        LogPrintf("CActiveIndexnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}

void CActiveIndexnode::ManageStateLocal() {
    LogPrint("indexnode", "CActiveIndexnode::ManageStateLocal -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
    if (nState == ACTIVE_INDEXNODE_STARTED) {
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    if (pwalletMain->GetIndexnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge < Params().GetConsensus().nIndexnodeMinimumConfirmations) {
            ChangeState(ACTIVE_INDEXNODE_INPUT_TOO_NEW);
            strNotCapableReason = strprintf(_("%s - %d confirmations"), GetStatus(), nInputAge);
            LogPrintf("CActiveIndexnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->LockCoin(vin.prevout);
        }

        CIndexnodeBroadcast mnb;
        std::string strError;
        if (!CIndexnodeBroadcast::Create(vin, service, keyCollateral, pubKeyCollateral, keyIndexnode,
                                     pubKeyIndexnode, strError, mnb)) {
            ChangeState(ACTIVE_INDEXNODE_NOT_CAPABLE);
            strNotCapableReason = "Error creating mastenode broadcast: " + strError;
            LogPrintf("CActiveIndexnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        fPingerEnabled = true;
        ChangeState(ACTIVE_INDEXNODE_STARTED);

        //update to indexnode list
        LogPrintf("CActiveIndexnode::ManageStateLocal -- Update Indexnode List\n");
        mnodeman.UpdateIndexnodeList(mnb);
        mnodeman.NotifyIndexnodeUpdates();

        //send to all peers
        LogPrintf("CActiveIndexnode::ManageStateLocal -- Relay broadcast, vin=%s\n", vin.ToString());
        mnb.RelayIndexNode();
    }
}
