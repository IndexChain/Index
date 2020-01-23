// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2016-2017 The Zcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BTZC_DEFINITION_H
#define BTZC_DEFINITION_H
#include "zerocoin_params.h"
enum {
    // primary version
    BLOCK_VERSION_DEFAULT = (1 << 0),
    // modifiers
    BLOCK_VERSION_AUXPOW = (1 << 8),
    // bits allocated for chain ID
    BLOCK_VERSION_CHAIN_START = (1 << 16),
    BLOCK_VERSION_CHAIN_END = (1 << 30),
};
static const int64_t nStartRewardTime = ZC_GENESIS_BLOCK_TIME;//Date and time (GMT): Thursday, January 23, 2020 9:40:53 PM

#endif //BTZC_DEFINITION_H



