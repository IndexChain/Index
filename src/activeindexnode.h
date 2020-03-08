// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEINDEXNODE_H
#define ACTIVEINDEXNODE_H

#include "net.h"
#include "key.h"
#include "wallet/wallet.h"

class CActiveIndexnode;

static const int ACTIVE_INDEXNODE_INITIAL          = 0; // initial state
static const int ACTIVE_INDEXNODE_SYNC_IN_PROCESS  = 1;
static const int ACTIVE_INDEXNODE_INPUT_TOO_NEW    = 2;
static const int ACTIVE_INDEXNODE_NOT_CAPABLE      = 3;
static const int ACTIVE_INDEXNODE_STARTED          = 4;

extern CActiveIndexnode activeIndexnode;

// Responsible for activating the Indexnode and pinging the network
class CActiveIndexnode
{
public:
    enum indexnode_type_enum_t {
        INDEXNODE_UNKNOWN = 0,
        INDEXNODE_REMOTE  = 1,
        INDEXNODE_LOCAL   = 2
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    indexnode_type_enum_t eType;

    bool fPingerEnabled;

    /// Ping Indexnode
    bool SendIndexnodePing();

public:
    // Keys for the active Indexnode
    CPubKey pubKeyIndexnode;
    CKey keyIndexnode;

    // Initialized while registering Indexnode
    CTxIn vin;
    CService service;

    int nState; // should be one of ACTIVE_INDEXNODE_XXXX
    std::string strNotCapableReason;

    CActiveIndexnode()
        : eType(INDEXNODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeyIndexnode(),
          keyIndexnode(),
          vin(),
          service(),
          nState(ACTIVE_INDEXNODE_INITIAL)
    {}

    /// Manage state of active Indexnode
    void ManageState();

    // Change state if different and publish update
    void ChangeState(int newState);

    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;

private:
    void ManageStateInitial();
    void ManageStateRemote();
    void ManageStateLocal();
};

#endif
