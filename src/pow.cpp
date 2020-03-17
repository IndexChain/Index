// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"
#include "main.h"
#include "arith_uint256.h"
#include "chain.h"
#include "primitives/block.h"
#include "consensus/consensus.h"
#include "uint256.h"
#include <iostream>
#include "util.h"
#include "chainparams.h"
#include "libzerocoin/bitcoin_bignum/bignum.h"
#include "utilstrencodings.h"
#include "fixed.h"
static CBigNum bnProofOfWorkLimit(~arith_uint256(0) >> 8);

double GetDifficultyHelper(unsigned int nBits) {
    int nShift = (nBits >> 24) & 0xff;
    double dDiff = (double) 0x0000ffff / (double) (nBits & 0x00ffffff);

    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

unsigned int static DarkGravityWave(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params,bool fProofOfStake = false ) {
    /* current difficulty formula, dash - DarkGravity v3, written by Evan Duffield - evan@dash.org */
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    //Various hardfork phases
    bool shouldReduceAvg = pindexLast->nHeight + 1 > params.nLowerAvgHFHeight;
    bool shouldCalcAvgSeperate = pindexLast->nHeight + 1 > params.nSeperateCalcDiffHeight;
    bool shouldTakeLargerAvg = pindexLast->nHeight + 1 > params.nLargerDGWAvgHeight;

    int64_t nPastBlocks = (shouldTakeLargerAvg && !shouldReduceAvg) ? 40:5;
    if(shouldReduceAvg)
        nPastBlocks = 25;

    // make sure we have at least (nPastBlocks + 1) blocks, otherwise just return powLimit
    if (!pindexLast || pindexLast->nHeight < nPastBlocks) {
        return bnPowLimit.GetCompact();
    }

    if (params.fPowAllowMinDifficultyBlocks) {
        // recent block is more than 2 hours old
        if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + 2 * 60 * 60) {
            return bnPowLimit.GetCompact();
        }
        // recent block is more than 10 minutes old
        if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 4) {
            arith_uint256 bnNew = arith_uint256().SetCompact(pindexLast->nBits) * 10;
            if (bnNew > bnPowLimit) {
                bnNew = bnPowLimit;
            }
            return bnNew.GetCompact();
        }
    }

    const CBlockIndex *pindex = pindexLast;
    //Get last block of specific type,can be either PoW or PoS
    pindex = GetLastBlockIndex(pindexLast,fProofOfStake);

    arith_uint256 bnPastTargetAvg;

    for (unsigned int nCountBlocks = 1; nCountBlocks <= nPastBlocks; nCountBlocks++) {
        arith_uint256 bnTarget = arith_uint256().SetCompact(pindex->nBits);
        if (nCountBlocks == 1) {
            bnPastTargetAvg = bnTarget;
        } else {
            // NOTE: that's not an average really...
            bnPastTargetAvg = (bnPastTargetAvg * nCountBlocks + bnTarget) / (nCountBlocks + 1);
        }

        if(nCountBlocks != nPastBlocks) {
            assert(pindex->pprev); // should never fail
            pindex = shouldCalcAvgSeperate ? GetLastBlockIndex(pindex->pprev,fProofOfStake) : pindex->pprev;
        }
    }

    arith_uint256 bnNew(bnPastTargetAvg);

    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindex->GetBlockTime();
    // NOTE: is this accurate? nActualTimespan counts it for (nPastBlocks - 1) blocks only...
    int64_t nTargetTimespan = nPastBlocks * params.nPowTargetSpacing;

    if (nActualTimespan < nTargetTimespan/3)
        nActualTimespan = nTargetTimespan/3;
    if (nActualTimespan > nTargetTimespan*3)
        nActualTimespan = nTargetTimespan*3;

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnPowLimit) {
        bnNew = bnPowLimit;
    }

    return bnNew.GetCompact();
}

unsigned int Lwma3CalculateNextWorkRequired(const CBlockIndex* pindexLast, const Consensus::Params& params, bool fProofOfStake)
{
    const arith_uint256 workLimit = fProofOfStake ? UintToArith256(params.posLimit) : UintToArith256(params.powLimit);
    const int64_t T = params.nPowTargetSpacing;
    const int64_t N = 25;
    const int64_t k = N * (N + 1) * T / 2;
    const int64_t height = GetLastBlockIndex(pindexLast, fProofOfStake)->nHeight;

    if (height < N) { return workLimit.GetCompact(); }

    arith_uint256 sumTarget, nextTarget;
    int64_t thisTimestamp, previousTimestamp;
    int64_t t = 0, j = 0;
    previousTimestamp = GetLastBlockIndex(pindexLast, fProofOfStake)->GetAncestor(height - N)->GetBlockTime();

    // Loop through N most recent blocks.
    for (int64_t i = height - N + 1; i <= height; i++) {
        const CBlockIndex* block = GetLastBlockIndex(pindexLast, fProofOfStake)->GetAncestor(i);
        thisTimestamp = (block->GetBlockTime() > previousTimestamp) ?
                         block->GetBlockTime() : previousTimestamp + 1;
        int64_t solvetime = std::min(6 * T, thisTimestamp - previousTimestamp);
        previousTimestamp = thisTimestamp;
        j++;
        t += solvetime * j; // Weighted solvetime sum.
        arith_uint256 target;
        target.SetCompact(block->nBits);
        sumTarget += target / (k * N);
    }
    nextTarget = t * sumTarget;
    if (nextTarget > workLimit)
        nextTarget = workLimit;

    return nextTarget.GetCompact();
}

unsigned int GetNextTargetRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params, bool fProofOfStake)
{
    unsigned int nTargetLimit = UintToArith256(Params().GetConsensus().posLimit).GetCompact();
    bool fLWMAPoS = pindexLast->nHeight + 1 > params.nLWMAPoSHeight;

    // Genesis block or first proof-of-stake block
    if (pindexLast == NULL || pindexLast->nHeight == Params().GetConsensus().nFirstPOSBlock)
        return UintToArith256(params.posLimit).GetCompact();

    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);

    if (pindexPrev->pprev == NULL)
        return nTargetLimit; // first block
    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
    if (pindexPrevPrev->pprev == NULL)
        return nTargetLimit; // second block
    
    if (pindexPrev->nHeight + 1 > params.nDGWPoSHeight && !fLWMAPoS)// Use DGW after the nDGWPoS Height
        return DarkGravityWave(pindexLast,pblock,params,fProofOfStake);
    else if(fLWMAPoS)
        return Lwma3CalculateNextWorkRequired(pindexPrev,params,fProofOfStake);

    return CalculateNextTargetRequired(pindexPrev, pindexPrevPrev->GetBlockTime(), params, fProofOfStake);
}

unsigned int CalculateNextTargetRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params, bool fProofOfStake)
{
    int64_t nTargetSpacing = Params().GetConsensus().nPowTargetSpacing;
    int64_t nActualSpacing = pindexLast->GetBlockTime() - nFirstBlockTime;

    // retarget with exponential moving toward target spacing
    const arith_uint256 bnTargetLimit = UintToArith256(Params().GetConsensus().posLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    int64_t nInterval = params.nPowTargetTimespan / nTargetSpacing;
    bnNew *= ((nInterval - 1) * nTargetSpacing + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * nTargetSpacing);

    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}

// Index GetNextWorkRequired
unsigned int GetNextWorkRequired(const CBlockIndex *pindexLast, const CBlockHeader *pblock, const Consensus::Params &params) {
    // Special rule for regtest: we never retarget.
    if (params.fPowNoRetargeting) {
        return pindexLast->nBits;
    }
    return DarkGravityWave(pindexLast, pblock, params,false);

}

unsigned int CalculateNextWorkRequired(const CBlockIndex *pindexLast, int64_t nFirstBlockTime, const Consensus::Params &params) {
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan / 4)
        nActualTimespan = params.nPowTargetTimespan / 4;
    if (nActualTimespan > params.nPowTargetTimespan * 4)
        nActualTimespan = params.nPowTargetTimespan * 4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params &params) {
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit)){
        return false;
    }
        // Check proof of work matches claimed amount
        if (UintToArith256(hash) > bnTarget){
           return error("CheckProofOfWork() : hash doesn't match nBits\n");
        }
    return true;
}