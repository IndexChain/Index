// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeindexnode.h"
#include "addrman.h"
#include "darksend.h"
//#include "governance.h"
#include "indexnode-payments.h"
#include "indexnode-sync.h"
#include "indexnode.h"
#include "indexnodeconfig.h"
#include "indexnodeman.h"
#include "netfulfilledman.h"
#include "util.h"
#include "validationinterface.h"

/** Indexnode manager */
CIndexnodeMan mnodeman;

const std::string CIndexnodeMan::SERIALIZATION_VERSION_STRING = "CIndexnodeMan-Version-4";

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, CIndexnode*>& t1,
                    const std::pair<int, CIndexnode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<int64_t, CIndexnode*>& t1,
                    const std::pair<int64_t, CIndexnode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

CIndexnodeIndex::CIndexnodeIndex()
    : nSize(0),
      mapIndex(),
      mapReverseIndex()
{}

bool CIndexnodeIndex::Get(int nIndex, CTxIn& vinIndexnode) const
{
    rindex_m_cit it = mapReverseIndex.find(nIndex);
    if(it == mapReverseIndex.end()) {
        return false;
    }
    vinIndexnode = it->second;
    return true;
}

int CIndexnodeIndex::GetIndexnodeIndex(const CTxIn& vinIndexnode) const
{
    index_m_cit it = mapIndex.find(vinIndexnode);
    if(it == mapIndex.end()) {
        return -1;
    }
    return it->second;
}

void CIndexnodeIndex::AddIndexnodeVIN(const CTxIn& vinIndexnode)
{
    index_m_it it = mapIndex.find(vinIndexnode);
    if(it != mapIndex.end()) {
        return;
    }
    int nNextIndex = nSize;
    mapIndex[vinIndexnode] = nNextIndex;
    mapReverseIndex[nNextIndex] = vinIndexnode;
    ++nSize;
}

void CIndexnodeIndex::Clear()
{
    mapIndex.clear();
    mapReverseIndex.clear();
    nSize = 0;
}
struct CompareByAddr

{
    bool operator()(const CIndexnode* t1,
                    const CIndexnode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

void CIndexnodeIndex::RebuildIndex()
{
    nSize = mapIndex.size();
    for(index_m_it it = mapIndex.begin(); it != mapIndex.end(); ++it) {
        mapReverseIndex[it->second] = it->first;
    }
}

CIndexnodeMan::CIndexnodeMan() : cs(),
  vIndexnodes(),
  mAskedUsForIndexnodeList(),
  mWeAskedForIndexnodeList(),
  mWeAskedForIndexnodeListEntry(),
  mWeAskedForVerification(),
  mMnbRecoveryRequests(),
  mMnbRecoveryGoodReplies(),
  listScheduledMnbRequestConnections(),
  nLastIndexRebuildTime(0),
  indexIndexnodes(),
  indexIndexnodesOld(),
  fIndexRebuilt(false),
  fIndexnodesAdded(false),
  fIndexnodesRemoved(false),
//  vecDirtyGovernanceObjectHashes(),
  nLastWatchdogVoteTime(0),
  mapSeenIndexnodeBroadcast(),
  mapSeenIndexnodePing(),
  nDsqCount(0)
{}

bool CIndexnodeMan::Add(CIndexnode &mn)
{
    LOCK(cs);

    CIndexnode *pmn = Find(mn.vin);
    if (pmn == NULL) {
        LogPrint("indexnode", "CIndexnodeMan::Add -- Adding new Indexnode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
        vIndexnodes.push_back(mn);
        indexIndexnodes.AddIndexnodeVIN(mn.vin);
        fIndexnodesAdded = true;
        return true;
    }

    return false;
}

void CIndexnodeMan::AskForMN(CNode* pnode, const CTxIn &vin)
{
    if(!pnode) return;

    LOCK(cs);

    std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it1 = mWeAskedForIndexnodeListEntry.find(vin.prevout);
    if (it1 != mWeAskedForIndexnodeListEntry.end()) {
        std::map<CNetAddr, int64_t>::iterator it2 = it1->second.find(pnode->addr);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CIndexnodeMan::AskForMN -- Asking same peer %s for missing indexnode entry again: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CIndexnodeMan::AskForMN -- Asking new peer %s for missing indexnode entry: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CIndexnodeMan::AskForMN -- Asking peer %s for missing indexnode entry for the first time: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
    }
    mWeAskedForIndexnodeListEntry[vin.prevout][pnode->addr] = GetTime() + DSEG_UPDATE_SECONDS;

    pnode->PushMessage(NetMsgType::DSEG, vin);
}

void CIndexnodeMan::Check()
{
    LOCK(cs);

//    LogPrint("indexnode", "CIndexnodeMan::Check -- nLastWatchdogVoteTime=%d, IsWatchdogActive()=%d\n", nLastWatchdogVoteTime, IsWatchdogActive());

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
        mn.Check();
    }
}

void CIndexnodeMan::CheckAndRemove()
{
    if(!indexnodeSync.IsIndexnodeListSynced()) return;

    LogPrintf("CIndexnodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateIndexnodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent indexnodes, prepare structures and make requests to reasure the state of inactive ones
        std::vector<CIndexnode>::iterator it = vIndexnodes.begin();
        std::vector<std::pair<int, CIndexnode> > vecIndexnodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES indexnode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        while(it != vIndexnodes.end()) {
            CIndexnodeBroadcast mnb = CIndexnodeBroadcast(*it);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if ((*it).IsOutpointSpent()) {
                LogPrint("indexnode", "CIndexnodeMan::CheckAndRemove -- Removing Indexnode: %s  addr=%s  %i now\n", (*it).GetStateString(), (*it).addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenIndexnodeBroadcast.erase(hash);
                mWeAskedForIndexnodeListEntry.erase((*it).vin.prevout);

                // and finally remove it from the list
//                it->FlagGovernanceItemsAsDirty();
                it = vIndexnodes.erase(it);
                fIndexnodesRemoved = true;
            } else {
                bool fAsk = pCurrentBlockIndex &&
                            (nAskForMnbRecovery > 0) &&
                            indexnodeSync.IsSynced() &&
                            it->IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash);
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CNetAddr> setRequested;
                    // calulate only once and only when it's needed
                    if(vecIndexnodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(pCurrentBlockIndex->nHeight);
                        vecIndexnodeRanks = GetIndexnodeRanks(nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL indexnodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecIndexnodeRanks.size(); i++) {
                        // avoid banning
                        if(mWeAskedForIndexnodeListEntry.count(it->vin.prevout) && mWeAskedForIndexnodeListEntry[it->vin.prevout].count(vecIndexnodeRanks[i].second.addr)) continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecIndexnodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if(fAskedForMnbRecovery) {
                        LogPrint("indexnode", "CIndexnodeMan::CheckAndRemove -- Recovery initiated, indexnode=%s\n", it->vin.prevout.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for INDEXNODE_NEW_START_REQUIRED indexnodes
        LogPrint("indexnode", "CIndexnodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CIndexnodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint("indexnode", "CIndexnodeMan::CheckAndRemove -- reprocessing mnb, indexnode=%s\n", itMnbReplies->second[0].vin.prevout.ToStringShort());
                    // mapSeenIndexnodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateIndexnodeList(NULL, itMnbReplies->second[0], nDos);
                }
                LogPrint("indexnode", "CIndexnodeMan::CheckAndRemove -- removing mnb recovery reply, indexnode=%s, size=%d\n", itMnbReplies->second[0].vin.prevout.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > >::iterator itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in INDEXNODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Indexnode list
        std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForIndexnodeList.begin();
        while(it1 != mAskedUsForIndexnodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForIndexnodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Indexnode list
        it1 = mWeAskedForIndexnodeList.begin();
        while(it1 != mWeAskedForIndexnodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForIndexnodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Indexnodes we've asked for
        std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it2 = mWeAskedForIndexnodeListEntry.begin();
        while(it2 != mWeAskedForIndexnodeListEntry.end()){
            std::map<CNetAddr, int64_t>::iterator it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForIndexnodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        std::map<CNetAddr, CIndexnodeVerification>::iterator it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenIndexnodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenIndexnodePing
        std::map<uint256, CIndexnodePing>::iterator it4 = mapSeenIndexnodePing.begin();
        while(it4 != mapSeenIndexnodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint("indexnode", "CIndexnodeMan::CheckAndRemove -- Removing expired Indexnode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenIndexnodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenIndexnodeVerification
        std::map<uint256, CIndexnodeVerification>::iterator itv2 = mapSeenIndexnodeVerification.begin();
        while(itv2 != mapSeenIndexnodeVerification.end()){
            if((*itv2).second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS){
                LogPrint("indexnode", "CIndexnodeMan::CheckAndRemove -- Removing expired Indexnode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenIndexnodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CIndexnodeMan::CheckAndRemove -- %s\n", ToString());

        if(fIndexnodesRemoved) {
            CheckAndRebuildIndexnodeIndex();
        }
    }

    if(fIndexnodesRemoved) {
        NotifyIndexnodeUpdates();
    }
}

void CIndexnodeMan::Clear()
{
    LOCK(cs);
    vIndexnodes.clear();
    mAskedUsForIndexnodeList.clear();
    mWeAskedForIndexnodeList.clear();
    mWeAskedForIndexnodeListEntry.clear();
    mapSeenIndexnodeBroadcast.clear();
    mapSeenIndexnodePing.clear();
    nDsqCount = 0;
    nLastWatchdogVoteTime = 0;
    indexIndexnodes.Clear();
    indexIndexnodesOld.Clear();
}

int CIndexnodeMan::CountIndexnodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinIndexnodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
        if(mn.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CIndexnodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinIndexnodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
        if(mn.nProtocolVersion < nProtocolVersion || !mn.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 indexnodes are allowed in 12.1, saving this for later
int CIndexnodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes)
        if ((nNetworkType == NET_IPV4 && mn.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mn.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mn.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CIndexnodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForIndexnodeList.find(pnode->addr);
            if(it != mWeAskedForIndexnodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CIndexnodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", pnode->addr.ToString());
                return;
            }
        }
    }
    
    pnode->PushMessage(NetMsgType::DSEG, CTxIn());
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForIndexnodeList[pnode->addr] = askAgain;

    LogPrint("indexnode", "CIndexnodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CIndexnode* CIndexnodeMan::Find(const std::string &txHash, const std::string outputIndex)
{
    LOCK(cs);

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes)
    {
        COutPoint outpoint = mn.vin.prevout;

        if(txHash==outpoint.hash.ToString().substr(0,64) &&
           outputIndex==to_string(outpoint.n))
            return &mn;
    }
    return NULL;
}

CIndexnode* CIndexnodeMan::Find(const CScript &payee)
{
    LOCK(cs);

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes)
    {
        if(GetScriptForDestination(mn.pubKeyCollateralAddress.GetID()) == payee)
            return &mn;
    }
    return NULL;
}

CIndexnode* CIndexnodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes)
    {
        if(mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return NULL;
}

CIndexnode* CIndexnodeMan::Find(const CPubKey &pubKeyIndexnode)
{
    LOCK(cs);

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes)
    {
        if(mn.pubKeyIndexnode == pubKeyIndexnode)
            return &mn;
    }
    return NULL;
}

bool CIndexnodeMan::Get(const CPubKey& pubKeyIndexnode, CIndexnode& indexnode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CIndexnode* pMN = Find(pubKeyIndexnode);
    if(!pMN)  {
        return false;
    }
    indexnode = *pMN;
    return true;
}

bool CIndexnodeMan::Get(const CTxIn& vin, CIndexnode& indexnode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CIndexnode* pMN = Find(vin);
    if(!pMN)  {
        return false;
    }
    indexnode = *pMN;
    return true;
}

indexnode_info_t CIndexnodeMan::GetIndexnodeInfo(const CTxIn& vin)
{
    indexnode_info_t info;
    LOCK(cs);
    CIndexnode* pMN = Find(vin);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

indexnode_info_t CIndexnodeMan::GetIndexnodeInfo(const CPubKey& pubKeyIndexnode)
{
    indexnode_info_t info;
    LOCK(cs);
    CIndexnode* pMN = Find(pubKeyIndexnode);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

bool CIndexnodeMan::Has(const CTxIn& vin)
{
    LOCK(cs);
    CIndexnode* pMN = Find(vin);
    return (pMN != NULL);
}

char* CIndexnodeMan::GetNotQualifyReason(CIndexnode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount)
{
    if (!mn.IsValidForPayment()) {
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'not valid for payment'");
        return reasonStr;
    }
    // //check protocol version
    if (mn.nProtocolVersion < mnpayments.GetMinIndexnodePaymentsProto()) {
        // LogPrintf("Invalid nProtocolVersion!\n");
        // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
        // LogPrintf("mnpayments.GetMinIndexnodePaymentsProto=%s!\n", mnpayments.GetMinIndexnodePaymentsProto());
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'Invalid nProtocolVersion', nProtocolVersion=%d", mn.nProtocolVersion);
        return reasonStr;
    }
    //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
    if (mnpayments.IsScheduled(mn, nBlockHeight)) {
        // LogPrintf("mnpayments.IsScheduled!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'is scheduled'");
        return reasonStr;
    }
    //it's too new, wait for a cycle
    if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
        // LogPrintf("it's too new, wait for a cycle!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'too new', sigTime=%s, will be qualifed after=%s",
                DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
        return reasonStr;
    }
    //make sure it has at least as many confirmations as there are indexnodes
    if (mn.GetCollateralAge() < nMnCount) {
        // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
        // LogPrintf("nMnCount=%s!\n", nMnCount);
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'collateralAge < znCount', collateralAge=%d, znCount=%d", mn.GetCollateralAge(), nMnCount);
        return reasonStr;
    }
    return NULL;
}

// Same method, different return type, to avoid Indexnode operator issues.
// TODO: discuss standardizing the JSON type here, as it's done everywhere else in the code.
UniValue CIndexnodeMan::GetNotQualifyReasonToUniValue(CIndexnode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount)
{
    UniValue ret(UniValue::VOBJ);
    UniValue data(UniValue::VOBJ);
    string description;

    if (!mn.IsValidForPayment()) {
        description = "not valid for payment";
    }

    //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
    else if (mnpayments.IsScheduled(mn, nBlockHeight)) {
        description = "Is scheduled";
    }

    // //check protocol version
    else if (mn.nProtocolVersion < mnpayments.GetMinIndexnodePaymentsProto()) {
        description = "Invalid nProtocolVersion";

        data.push_back(Pair("nProtocolVersion", mn.nProtocolVersion));
    }

    //it's too new, wait for a cycle
    else if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
        // LogPrintf("it's too new, wait for a cycle!\n");
        description = "Too new";

        //TODO unix timestamp
        data.push_back(Pair("sigTime", mn.sigTime));
        data.push_back(Pair("qualifiedAfter", mn.sigTime + (nMnCount * 2.6 * 60)));
    }
    //make sure it has at least as many confirmations as there are indexnodes
    else if (mn.GetCollateralAge() < nMnCount) {
        description = "collateralAge < znCount";

        data.push_back(Pair("collateralAge", mn.GetCollateralAge()));
        data.push_back(Pair("znCount", nMnCount));
    }

    ret.push_back(Pair("result", description.empty()));
    if(!description.empty()){
        ret.push_back(Pair("description", description));
    }
    if(!data.empty()){
        ret.push_back(Pair("data", data));
    }

    return ret;
}

//
// Deterministically select the oldest/best indexnode to pay on the network
//
CIndexnode* CIndexnodeMan::GetNextIndexnodeInQueueForPayment(bool fFilterSigTime, int& nCount)
{
    if(!pCurrentBlockIndex) {
        nCount = 0;
        return NULL;
    }
    return GetNextIndexnodeInQueueForPayment(pCurrentBlockIndex->nHeight, fFilterSigTime, nCount);
}

CIndexnode* CIndexnodeMan::GetNextIndexnodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount)
{
    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    CIndexnode *pBestIndexnode = NULL;
    std::vector<std::pair<int, CIndexnode*> > vecIndexnodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */
    int nMnCount = CountEnabled();
    int index = 0;
    BOOST_FOREACH(CIndexnode &mn, vIndexnodes)
    {
        index += 1;
        // LogPrintf("index=%s, mn=%s\n", index, mn.ToString());
        /*if (!mn.IsValidForPayment()) {
            LogPrint("indexnodeman", "Indexnode, %s, addr(%s), not-qualified: 'not valid for payment'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        // //check protocol version
        if (mn.nProtocolVersion < mnpayments.GetMinIndexnodePaymentsProto()) {
            // LogPrintf("Invalid nProtocolVersion!\n");
            // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
            // LogPrintf("mnpayments.GetMinIndexnodePaymentsProto=%s!\n", mnpayments.GetMinIndexnodePaymentsProto());
            LogPrint("indexnodeman", "Indexnode, %s, addr(%s), not-qualified: 'invalid nProtocolVersion'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (mnpayments.IsScheduled(mn, nBlockHeight)) {
            // LogPrintf("mnpayments.IsScheduled!\n");
            LogPrint("indexnodeman", "Indexnode, %s, addr(%s), not-qualified: 'IsScheduled'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's too new, wait for a cycle
        if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
            // LogPrintf("it's too new, wait for a cycle!\n");
            LogPrint("indexnodeman", "Indexnode, %s, addr(%s), not-qualified: 'it's too new, wait for a cycle!', sigTime=%s, will be qualifed after=%s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
            continue;
        }
        //make sure it has at least as many confirmations as there are indexnodes
        if (mn.GetCollateralAge() < nMnCount) {
            // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
            // LogPrintf("nMnCount=%s!\n", nMnCount);
            LogPrint("indexnodeman", "Indexnode, %s, addr(%s), not-qualified: 'mn.GetCollateralAge() < nMnCount', CollateralAge=%d, nMnCount=%d\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), mn.GetCollateralAge(), nMnCount);
            continue;
        }*/
        char* reasonStr = GetNotQualifyReason(mn, nBlockHeight, fFilterSigTime, nMnCount);
        if (reasonStr != NULL) {
            LogPrint("indexnodeman", "Indexnode, %s, addr(%s), qualify %s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), reasonStr);
            delete [] reasonStr;
            continue;
        }
        vecIndexnodeLastPaid.push_back(std::make_pair(mn.GetLastPaidBlock(), &mn));
    }
    nCount = (int)vecIndexnodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCount < nMnCount / 3) {
        // LogPrintf("Need Return, nCount=%s, nMnCount/3=%s\n", nCount, nMnCount/3);
        return GetNextIndexnodeInQueueForPayment(nBlockHeight, false, nCount);
    }

    // Sort them low to high
    sort(vecIndexnodeLastPaid.begin(), vecIndexnodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CIndexnode::GetNextIndexnodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return NULL;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    BOOST_FOREACH (PAIRTYPE(int, CIndexnode*)& s, vecIndexnodeLastPaid){
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestIndexnode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    return pBestIndexnode;
}

CIndexnode* CIndexnodeMan::FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinIndexnodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CIndexnodeMan::FindRandomNotInVec -- %d enabled indexnodes, %d indexnodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return NULL;

    // fill a vector of pointers
    std::vector<CIndexnode*> vpIndexnodesShuffled;
    BOOST_FOREACH(CIndexnode &mn, vIndexnodes) {
        vpIndexnodesShuffled.push_back(&mn);
    }

    InsecureRand insecureRand;
    // shuffle pointers
    std::random_shuffle(vpIndexnodesShuffled.begin(), vpIndexnodesShuffled.end(), insecureRand);
    bool fExclude;

    // loop through
    BOOST_FOREACH(CIndexnode* pmn, vpIndexnodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        BOOST_FOREACH(const CTxIn &txinToExclude, vecToExclude) {
            if(pmn->vin.prevout == txinToExclude.prevout) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("indexnode", "CIndexnodeMan::FindRandomNotInVec -- found, indexnode=%s\n", pmn->vin.prevout.ToStringShort());
        return pmn;
    }

    LogPrint("indexnode", "CIndexnodeMan::FindRandomNotInVec -- failed\n");
    return NULL;
}

int CIndexnodeMan::GetIndexnodeRank(const CTxIn& vin, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CIndexnode*> > vecIndexnodeScores;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return -1;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive) {
            if(!mn.IsEnabled()) continue;
        }
        else {
            if(!mn.IsValidForPayment()) continue;
        }
        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecIndexnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecIndexnodeScores.rbegin(), vecIndexnodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CIndexnode*)& scorePair, vecIndexnodeScores) {
        nRank++;
        if(scorePair.second->vin.prevout == vin.prevout) return nRank;
    }

    return -1;
}

std::vector<std::pair<int, CIndexnode> > CIndexnodeMan::GetIndexnodeRanks(int nBlockHeight, int nMinProtocol)
{
    std::vector<std::pair<int64_t, CIndexnode*> > vecIndexnodeScores;
    std::vector<std::pair<int, CIndexnode> > vecIndexnodeRanks;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return vecIndexnodeRanks;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {

        if(mn.nProtocolVersion < nMinProtocol || !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecIndexnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecIndexnodeScores.rbegin(), vecIndexnodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CIndexnode*)& s, vecIndexnodeScores) {
        nRank++;
        s.second->SetRank(nRank);
        vecIndexnodeRanks.push_back(std::make_pair(nRank, *s.second));
    }

    return vecIndexnodeRanks;
}

CIndexnode* CIndexnodeMan::GetIndexnodeByRank(int nRank, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CIndexnode*> > vecIndexnodeScores;

    LOCK(cs);

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight)) {
        LogPrintf("CIndexnode::GetIndexnodeByRank -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight);
        return NULL;
    }

    // Fill scores
    BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {

        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive && !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecIndexnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecIndexnodeScores.rbegin(), vecIndexnodeScores.rend(), CompareScoreMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CIndexnode*)& s, vecIndexnodeScores){
        rank++;
        if(rank == nRank) {
            return s.second;
        }
    }

    return NULL;
}

void CIndexnodeMan::ProcessIndexnodeConnections()
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if(pnode->fIndexnode) {
            if(darkSendPool.pSubmittedToIndexnode != NULL && pnode->addr == darkSendPool.pSubmittedToIndexnode->addr) continue;
            // LogPrintf("Closing Indexnode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    }
}

std::pair<CService, std::set<uint256> > CIndexnodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}


void CIndexnodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

//    LogPrint("indexnode", "CIndexnodeMan::ProcessMessage, strCommand=%s\n", strCommand);
    if(fLiteMode) return; // disable all Index specific functionality
    if(!indexnodeSync.IsBlockchainSynced()) return;

    if (strCommand == NetMsgType::MNANNOUNCE) { //Indexnode Broadcast
        CIndexnodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        LogPrintf("MNANNOUNCE -- Indexnode announce, indexnode=%s\n", mnb.vin.prevout.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateIndexnodeList(pfrom, mnb, nDos)) {
            // use announced Indexnode as a peer
            addrman.Add(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            Misbehaving(pfrom->GetId(), nDos);
        }

        if(fIndexnodesAdded) {
            NotifyIndexnodeUpdates();
        }
    } else if (strCommand == NetMsgType::MNPING) { //Indexnode Ping

        CIndexnodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        LogPrint("indexnode", "MNPING -- Indexnode ping, indexnode=%s\n", mnp.vin.prevout.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenIndexnodePing.count(nHash)) return; //seen
        mapSeenIndexnodePing.insert(std::make_pair(nHash, mnp));

        LogPrint("indexnode", "MNPING -- Indexnode ping, indexnode=%s new\n", mnp.vin.prevout.ToStringShort());

        // see if we have this Indexnode
        CIndexnode* pmn = mnodeman.Find(mnp.vin);

        // too late, new MNANNOUNCE is required
        if(pmn && pmn->IsNewStartRequired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a indexnode entry once
        AskForMN(pfrom, mnp.vin);

    } else if (strCommand == NetMsgType::DSEG) { //Get Indexnode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after indexnode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!indexnodeSync.IsSynced()) return;

        CTxIn vin;
        vRecv >> vin;

        LogPrint("indexnode", "DSEG -- Indexnode list, indexnode=%s\n", vin.prevout.ToStringShort());

        LOCK(cs);

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForIndexnodeList.find(pfrom->addr);
                if (i != mAskedUsForIndexnodeList.end()){
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("DSEG -- peer already asked me for the list, peer=%d\n", pfrom->id);
                        return;
                    }
                }
                int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
                mAskedUsForIndexnodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int nInvCount = 0;

        BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
            if (vin != CTxIn() && vin != mn.vin) continue; // asked for specific vin but we are not there yet
            if (mn.addr.IsRFC1918() || mn.addr.IsLocal()) continue; // do not send local network indexnode
            if (mn.IsUpdateRequired()) continue; // do not send outdated indexnodes

            LogPrint("indexnode", "DSEG -- Sending Indexnode entry: indexnode=%s  addr=%s\n", mn.vin.prevout.ToStringShort(), mn.addr.ToString());
            CIndexnodeBroadcast mnb = CIndexnodeBroadcast(mn);
            uint256 hash = mnb.GetHash();
            pfrom->PushInventory(CInv(MSG_INDEXNODE_ANNOUNCE, hash));
            pfrom->PushInventory(CInv(MSG_INDEXNODE_PING, mn.lastPing.GetHash()));
            nInvCount++;

            if (!mapSeenIndexnodeBroadcast.count(hash)) {
                mapSeenIndexnodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));
            }

            if (vin == mn.vin) {
                LogPrintf("DSEG -- Sent 1 Indexnode inv to peer %d\n", pfrom->id);
                return;
            }
        }

        if(vin == CTxIn()) {
            pfrom->PushMessage(NetMsgType::SYNCSTATUSCOUNT, INDEXNODE_SYNC_LIST, nInvCount);
            LogPrintf("DSEG -- Sent %d Indexnode invs to peer %d\n", nInvCount, pfrom->id);
            return;
        }
        // smth weird happen - someone asked us for vin we have no idea about?
        LogPrint("indexnode", "DSEG -- No invs sent to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::MNVERIFY) { // Indexnode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CIndexnodeVerification mnv;
        vRecv >> mnv;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some indexnode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some indexnode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

// Verification of indexnodes via unique direct requests.

void CIndexnodeMan::DoFullVerificationStep()
{
    if(activeIndexnode.vin == CTxIn()) return;
    if(!indexnodeSync.IsSynced()) return;

    std::vector<std::pair<int, CIndexnode> > vecIndexnodeRanks = GetIndexnodeRanks(pCurrentBlockIndex->nHeight - 1, MIN_POSE_PROTO_VERSION);

    // Need LOCK2 here to ensure consistent locking order because the SendVerifyRequest call below locks cs_main
    // through GetHeight() signal in ConnectNode
    LOCK2(cs_main, cs);

    int nCount = 0;

    int nMyRank = -1;
    int nRanksTotal = (int)vecIndexnodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    std::vector<std::pair<int, CIndexnode> >::iterator it = vecIndexnodeRanks.begin();
    while(it != vecIndexnodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint("indexnode", "CIndexnodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.vin == activeIndexnode.vin) {
            nMyRank = it->first;
            LogPrint("indexnode", "CIndexnodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d indexnodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this indexnode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to MAX_POSE_CONNECTIONS indexnodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecIndexnodeRanks.size()) return;

    std::vector<CIndexnode*> vSortedByAddr;
    BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
        vSortedByAddr.push_back(&mn);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecIndexnodeRanks.begin() + nOffset;
    while(it != vecIndexnodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("indexnode", "CIndexnodeMan::DoFullVerificationStep -- Already %s%s%s indexnode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.vin.prevout.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecIndexnodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint("indexnode", "CIndexnodeMan::DoFullVerificationStep -- Verifying indexnode %s rank %d/%d address %s\n",
                    it->second.vin.prevout.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if(SendVerifyRequest(CAddress(it->second.addr, NODE_NETWORK), vSortedByAddr)) {
            nCount++;
            if(nCount >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecIndexnodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    LogPrint("indexnode", "CIndexnodeMan::DoFullVerificationStep -- Sent verification requests to %d indexnodes\n", nCount);
}

// This function tries to find indexnodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CIndexnodeMan::CheckSameAddr()
{
    if(!indexnodeSync.IsSynced() || vIndexnodes.empty()) return;

    std::vector<CIndexnode*> vBan;
    std::vector<CIndexnode*> vSortedByAddr;

    {
        LOCK(cs);

        CIndexnode* pprevIndexnode = NULL;
        CIndexnode* pverifiedIndexnode = NULL;

        BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
            vSortedByAddr.push_back(&mn);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        BOOST_FOREACH(CIndexnode* pmn, vSortedByAddr) {
            // check only (pre)enabled indexnodes
            if(!pmn->IsEnabled() && !pmn->IsPreEnabled()) continue;
            // initial step
            if(!pprevIndexnode) {
                pprevIndexnode = pmn;
                pverifiedIndexnode = pmn->IsPoSeVerified() ? pmn : NULL;
                continue;
            }
            // second+ step
            if(pmn->addr == pprevIndexnode->addr) {
                if(pverifiedIndexnode) {
                    // another indexnode with the same ip is verified, ban this one
                    vBan.push_back(pmn);
                } else if(pmn->IsPoSeVerified()) {
                    // this indexnode with the same ip is verified, ban previous one
                    vBan.push_back(pprevIndexnode);
                    // and keep a reference to be able to ban following indexnodes with the same ip
                    pverifiedIndexnode = pmn;
                }
            } else {
                pverifiedIndexnode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevIndexnode = pmn;
        }
    }

    // ban duplicates
    BOOST_FOREACH(CIndexnode* pmn, vBan) {
        LogPrintf("CIndexnodeMan::CheckSameAddr -- increasing PoSe ban score for indexnode %s\n", pmn->vin.prevout.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CIndexnodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<CIndexnode*>& vSortedByAddr)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("indexnode", "CIndexnodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    CNode* pnode = ConnectNode(addr, NULL, false, true);
    if(pnode == NULL) {
        LogPrintf("CIndexnodeMan::SendVerifyRequest -- can't connect to node to verify it, addr=%s\n", addr.ToString());
        return false;
    }

    netfulfilledman.AddFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
    // use random nonce, store it and require node to reply with correct one later
    CIndexnodeVerification mnv(addr, GetRandInt(999999), pCurrentBlockIndex->nHeight - 1);
    mWeAskedForVerification[addr] = mnv;
    LogPrintf("CIndexnodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);

    return true;
}

void CIndexnodeMan::SendVerifyReply(CNode* pnode, CIndexnodeVerification& mnv)
{
    // only indexnodes can sign this, why would someone ask regular node?
    if(!fIndexNode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
//        // peer should not ask us that often
        LogPrintf("IndexnodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("IndexnodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strMessage = strprintf("%s%d%s", activeIndexnode.service.ToString(), mnv.nonce, blockHash.ToString());

    if(!darkSendSigner.SignMessage(strMessage, mnv.vchSig1, activeIndexnode.keyIndexnode)) {
        LogPrintf("IndexnodeMan::SendVerifyReply -- SignMessage() failed\n");
        return;
    }

    std::string strError;

    if(!darkSendSigner.VerifyMessage(activeIndexnode.pubKeyIndexnode, mnv.vchSig1, strMessage, strError)) {
        LogPrintf("IndexnodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
        return;
    }

    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CIndexnodeMan::ProcessVerifyReply(CNode* pnode, CIndexnodeVerification& mnv)
{
    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintf("CIndexnodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CIndexnodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CIndexnodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("IndexnodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

//    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintf("CIndexnodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CIndexnode* prealIndexnode = NULL;
        std::vector<CIndexnode*> vpIndexnodesToBan;
        std::vector<CIndexnode>::iterator it = vIndexnodes.begin();
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(), mnv.nonce, blockHash.ToString());
        while(it != vIndexnodes.end()) {
            if(CAddress(it->addr, NODE_NETWORK) == pnode->addr) {
                if(darkSendSigner.VerifyMessage(it->pubKeyIndexnode, mnv.vchSig1, strMessage1, strError)) {
                    // found it!
                    prealIndexnode = &(*it);
                    if(!it->IsPoSeVerified()) {
                        it->DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated indexnode
                    if(activeIndexnode.vin == CTxIn()) continue;
                    // update ...
                    mnv.addr = it->addr;
                    mnv.vin1 = it->vin;
                    mnv.vin2 = activeIndexnode.vin;
                    std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                            mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());
                    // ... and sign it
                    if(!darkSendSigner.SignMessage(strMessage2, mnv.vchSig2, activeIndexnode.keyIndexnode)) {
                        LogPrintf("IndexnodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                        return;
                    }

                    std::string strError;

                    if(!darkSendSigner.VerifyMessage(activeIndexnode.pubKeyIndexnode, mnv.vchSig2, strMessage2, strError)) {
                        LogPrintf("IndexnodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                        return;
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mnv.Relay();

                } else {
                    vpIndexnodesToBan.push_back(&(*it));
                }
            }
            ++it;
        }
        // no real indexnode found?...
        if(!prealIndexnode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CIndexnodeMan::ProcessVerifyReply -- ERROR: no real indexnode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CIndexnodeMan::ProcessVerifyReply -- verified real indexnode %s for addr %s\n",
                    prealIndexnode->vin.prevout.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        BOOST_FOREACH(CIndexnode* pmn, vpIndexnodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrint("indexnode", "CIndexnodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealIndexnode->vin.prevout.ToStringShort(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        LogPrintf("CIndexnodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake indexnodes, addr %s\n",
                    (int)vpIndexnodesToBan.size(), pnode->addr.ToString());
    }
}

void CIndexnodeMan::ProcessVerifyBroadcast(CNode* pnode, const CIndexnodeVerification& mnv)
{
    std::string strError;

    if(mapSeenIndexnodeVerification.find(mnv.GetHash()) != mapSeenIndexnodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenIndexnodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
        LogPrint("indexnode", "IndexnodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    pCurrentBlockIndex->nHeight, mnv.nBlockHeight, pnode->id);
        return;
    }

    if(mnv.vin1.prevout == mnv.vin2.prevout) {
        LogPrint("indexnode", "IndexnodeMan::ProcessVerifyBroadcast -- ERROR: same vins %s, peer=%d\n",
                    mnv.vin1.prevout.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("IndexnodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    int nRank = GetIndexnodeRank(mnv.vin2, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION);

    if (nRank == -1) {
        LogPrint("indexnode", "CIndexnodeMan::ProcessVerifyBroadcast -- Can't calculate rank for indexnode %s\n",
                    mnv.vin2.prevout.ToStringShort());
        return;
    }

    if(nRank > MAX_POSE_RANK) {
        LogPrint("indexnode", "CIndexnodeMan::ProcessVerifyBroadcast -- Mastrernode %s is not in top %d, current rank %d, peer=%d\n",
                    mnv.vin2.prevout.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString());
        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());

        CIndexnode* pmn1 = Find(mnv.vin1);
        if(!pmn1) {
            LogPrintf("CIndexnodeMan::ProcessVerifyBroadcast -- can't find indexnode1 %s\n", mnv.vin1.prevout.ToStringShort());
            return;
        }

        CIndexnode* pmn2 = Find(mnv.vin2);
        if(!pmn2) {
            LogPrintf("CIndexnodeMan::ProcessVerifyBroadcast -- can't find indexnode2 %s\n", mnv.vin2.prevout.ToStringShort());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CIndexnodeMan::ProcessVerifyBroadcast -- addr %s do not match %s\n", mnv.addr.ToString(), pnode->addr.ToString());
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn1->pubKeyIndexnode, mnv.vchSig1, strMessage1, strError)) {
            LogPrintf("IndexnodeMan::ProcessVerifyBroadcast -- VerifyMessage() for indexnode1 failed, error: %s\n", strError);
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn2->pubKeyIndexnode, mnv.vchSig2, strMessage2, strError)) {
            LogPrintf("IndexnodeMan::ProcessVerifyBroadcast -- VerifyMessage() for indexnode2 failed, error: %s\n", strError);
            return;
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CIndexnodeMan::ProcessVerifyBroadcast -- verified indexnode %s for addr %s\n",
                    pmn1->vin.prevout.ToStringShort(), pnode->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
            if(mn.addr != mnv.addr || mn.vin.prevout == mnv.vin1.prevout) continue;
            mn.IncreasePoSeBanScore();
            nCount++;
            LogPrint("indexnode", "CIndexnodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mn.vin.prevout.ToStringShort(), mn.addr.ToString(), mn.nPoSeBanScore);
        }
        LogPrintf("CIndexnodeMan::ProcessVerifyBroadcast -- PoSe score incresed for %d fake indexnodes, addr %s\n",
                    nCount, pnode->addr.ToString());
    }
}

std::string CIndexnodeMan::ToString() const
{
    std::ostringstream info;

    info << "Indexnodes: " << (int)vIndexnodes.size() <<
            ", peers who asked us for Indexnode list: " << (int)mAskedUsForIndexnodeList.size() <<
            ", peers we asked for Indexnode list: " << (int)mWeAskedForIndexnodeList.size() <<
            ", entries in Indexnode list we asked for: " << (int)mWeAskedForIndexnodeListEntry.size() <<
            ", indexnode index size: " << indexIndexnodes.GetSize() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

void CIndexnodeMan::UpdateIndexnodeList(CIndexnodeBroadcast mnb)
{
    try {
        LogPrintf("CIndexnodeMan::UpdateIndexnodeList\n");
        LOCK2(cs_main, cs);
        mapSeenIndexnodePing.insert(std::make_pair(mnb.lastPing.GetHash(), mnb.lastPing));
        mapSeenIndexnodeBroadcast.insert(std::make_pair(mnb.GetHash(), std::make_pair(GetTime(), mnb)));

        LogPrintf("CIndexnodeMan::UpdateIndexnodeList -- indexnode=%s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());

        CIndexnode *pmn = Find(mnb.vin);
        if (pmn == NULL) {
            CIndexnode mn(mnb);
            if (Add(mn)) {
                indexnodeSync.AddedIndexnodeList();
                GetMainSignals().UpdatedIndexnode(mn);
            }
        } else {
            CIndexnodeBroadcast mnbOld = mapSeenIndexnodeBroadcast[CIndexnodeBroadcast(*pmn).GetHash()].second;
            if (pmn->UpdateFromNewBroadcast(mnb)) {
                indexnodeSync.AddedIndexnodeList();
                GetMainSignals().UpdatedIndexnode(*pmn);
                mapSeenIndexnodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } catch (const std::exception &e) {
        PrintExceptionContinue(&e, "UpdateIndexnodeList");
    }
}

bool CIndexnodeMan::CheckMnbAndUpdateIndexnodeList(CNode* pfrom, CIndexnodeBroadcast mnb, int& nDos)
{
    // Need LOCK2 here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- indexnode=%s\n", mnb.vin.prevout.ToStringShort());

        uint256 hash = mnb.GetHash();
        if (mapSeenIndexnodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- indexnode=%s seen\n", mnb.vin.prevout.ToStringShort());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if (GetTime() - mapSeenIndexnodeBroadcast[hash].first > INDEXNODE_NEW_START_REQUIRED_SECONDS - INDEXNODE_MIN_MNP_SECONDS * 2) {
                LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- indexnode=%s seen update\n", mnb.vin.prevout.ToStringShort());
                mapSeenIndexnodeBroadcast[hash].first = GetTime();
                indexnodeSync.AddedIndexnodeList();
                GetMainSignals().UpdatedIndexnode(mnb);
            }
            // did we ask this node for it?
            if (pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- mnb=%s seen request\n", hash.ToString());
                if (mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if (mnb.lastPing.sigTime > mapSeenIndexnodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CIndexnode mnTemp = CIndexnode(mnb);
                        mnTemp.Check();
                        LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetTime() - mnb.lastPing.sigTime) / 60, mnTemp.GetStateString());
                        if (mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- indexnode=%s seen good\n", mnb.vin.prevout.ToStringShort());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenIndexnodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- indexnode=%s new\n", mnb.vin.prevout.ToStringShort());

        if (!mnb.SimpleCheck(nDos)) {
            LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- SimpleCheck() failed, indexnode=%s\n", mnb.vin.prevout.ToStringShort());
            return false;
        }

        // search Indexnode list
        CIndexnode *pmn = Find(mnb.vin);
        if (pmn) {
            CIndexnodeBroadcast mnbOld = mapSeenIndexnodeBroadcast[CIndexnodeBroadcast(*pmn).GetHash()].second;
            if (!mnb.Update(pmn, nDos)) {
                LogPrint("indexnode", "CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- Update() failed, indexnode=%s\n", mnb.vin.prevout.ToStringShort());
                return false;
            }
            if (hash != mnbOld.GetHash()) {
                mapSeenIndexnodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } // end of LOCK(cs);

    if(mnb.CheckOutpoint(nDos)) {
        if(Add(mnb)){
            GetMainSignals().UpdatedIndexnode(mnb);  
        }
        indexnodeSync.AddedIndexnodeList();
        // if it matches our Indexnode privkey...
        if(fIndexNode && mnb.pubKeyIndexnode == activeIndexnode.pubKeyIndexnode) {
            mnb.nPoSeBanScore = -INDEXNODE_POSE_BAN_MAX_SCORE;
            if(mnb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- Got NEW Indexnode entry: indexnode=%s  sigTime=%lld  addr=%s\n",
                            mnb.vin.prevout.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                activeIndexnode.ManageState();
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        mnb.RelayIndexNode();
    } else {
        LogPrintf("CIndexnodeMan::CheckMnbAndUpdateIndexnodeList -- Rejected Indexnode entry: %s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());
        return false;
    }

    return true;
}

void CIndexnodeMan::UpdateLastPaid()
{
    LOCK(cs);
    if(fLiteMode) return;
    if(!pCurrentBlockIndex) {
        // LogPrintf("CIndexnodeMan::UpdateLastPaid, pCurrentBlockIndex=NULL\n");
        return;
    }

    static bool IsFirstRun = true;
    // Do full scan on first run or if we are not a indexnode
    // (MNs should update this info on every block, so limited scan should be enough for them)
    int nMaxBlocksToScanBack = (IsFirstRun || !fIndexNode) ? mnpayments.GetStorageLimit() : LAST_PAID_SCAN_BLOCKS;

    LogPrint("mnpayments", "CIndexnodeMan::UpdateLastPaid -- nHeight=%d, nMaxBlocksToScanBack=%d, IsFirstRun=%s\n",
                             pCurrentBlockIndex->nHeight, nMaxBlocksToScanBack, IsFirstRun ? "true" : "false");

    BOOST_FOREACH(CIndexnode& mn, vIndexnodes) {
        mn.UpdateLastPaid(pCurrentBlockIndex, nMaxBlocksToScanBack);
    }

    // every time is like the first time if winners list is not synced
    IsFirstRun = !indexnodeSync.IsWinnersListSynced();
}

void CIndexnodeMan::CheckAndRebuildIndexnodeIndex()
{
    LOCK(cs);

    if(GetTime() - nLastIndexRebuildTime < MIN_INDEX_REBUILD_TIME) {
        return;
    }

    if(indexIndexnodes.GetSize() <= MAX_EXPECTED_INDEX_SIZE) {
        return;
    }

    if(indexIndexnodes.GetSize() <= int(vIndexnodes.size())) {
        return;
    }

    indexIndexnodesOld = indexIndexnodes;
    indexIndexnodes.Clear();
    for(size_t i = 0; i < vIndexnodes.size(); ++i) {
        indexIndexnodes.AddIndexnodeVIN(vIndexnodes[i].vin);
    }

    fIndexRebuilt = true;
    nLastIndexRebuildTime = GetTime();
}

void CIndexnodeMan::UpdateWatchdogVoteTime(const CTxIn& vin)
{
    LOCK(cs);
    CIndexnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->UpdateWatchdogVoteTime();
    nLastWatchdogVoteTime = GetTime();
}

bool CIndexnodeMan::IsWatchdogActive()
{
    LOCK(cs);
    // Check if any indexnodes have voted recently, otherwise return false
    return (GetTime() - nLastWatchdogVoteTime) <= INDEXNODE_WATCHDOG_MAX_SECONDS;
}

void CIndexnodeMan::CheckIndexnode(const CTxIn& vin, bool fForce)
{
    LOCK(cs);
    CIndexnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

void CIndexnodeMan::CheckIndexnode(const CPubKey& pubKeyIndexnode, bool fForce)
{
    LOCK(cs);
    CIndexnode* pMN = Find(pubKeyIndexnode);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

int CIndexnodeMan::GetIndexnodeState(const CTxIn& vin)
{
    LOCK(cs);
    CIndexnode* pMN = Find(vin);
    if(!pMN)  {
        return CIndexnode::INDEXNODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

int CIndexnodeMan::GetIndexnodeState(const CPubKey& pubKeyIndexnode)
{
    LOCK(cs);
    CIndexnode* pMN = Find(pubKeyIndexnode);
    if(!pMN)  {
        return CIndexnode::INDEXNODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

bool CIndexnodeMan::IsIndexnodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CIndexnode* pMN = Find(vin);
    if(!pMN) {
        return false;
    }
    return pMN->IsPingedWithin(nSeconds, nTimeToCheckAt);
}

void CIndexnodeMan::SetIndexnodeLastPing(const CTxIn& vin, const CIndexnodePing& mnp)
{
    LOCK(cs);
    CIndexnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->SetLastPing(mnp);
    mapSeenIndexnodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CIndexnodeBroadcast mnb(*pMN);
    uint256 hash = mnb.GetHash();
    if(mapSeenIndexnodeBroadcast.count(hash)) {
        mapSeenIndexnodeBroadcast[hash].second.SetLastPing(mnp);
    }
}

void CIndexnodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("indexnode", "CIndexnodeMan::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);

    CheckSameAddr();
    
    if(fIndexNode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid();
    }
}

void CIndexnodeMan::NotifyIndexnodeUpdates()
{
    // Avoid double locking
    bool fIndexnodesAddedLocal = false;
    bool fIndexnodesRemovedLocal = false;
    {
        LOCK(cs);
        fIndexnodesAddedLocal = fIndexnodesAdded;
        fIndexnodesRemovedLocal = fIndexnodesRemoved;
    }

    if(fIndexnodesAddedLocal) {
//        governance.CheckIndexnodeOrphanObjects();
//        governance.CheckIndexnodeOrphanVotes();
    }
    if(fIndexnodesRemovedLocal) {
//        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fIndexnodesAdded = false;
    fIndexnodesRemoved = false;
}
