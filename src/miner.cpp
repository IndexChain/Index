// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "amount.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "hash.h"
#include "main.h"
#include "base58.h"
#include "net.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validationinterface.h"
#include "wallet/wallet.h"
#include "definition.h"
#include "crypto/scrypt.h"
#include "indexnode-payments.h"
#include "indexnode-sync.h"
#include "indexnodeman.h"
#include "zerocoin.h"
#include "sigma.h"
#include "sigma/remint.h"
#include <algorithm>
#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#include <queue>
#include <unistd.h>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
uint64_t nLastBlockWeight = 0;
int64_t nLastCoinStakeSearchInterval = 0;
unsigned int nMinerSleep = 4000;
class ScoreCompare
{
public:
    ScoreCompare() {}

    bool operator()(const CTxMemPool::txiter a, const CTxMemPool::txiter b)
    {
        return CompareTxMemPoolEntryByScore()(*b,*a); // Convert to less than
    }
};

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams,false);

    return nNewTime - nOldTime;
}

BlockAssembler::BlockAssembler(const CChainParams& _chainparams) : chainparams(_chainparams)
{
    // Block resource limits
    // If neither -blockmaxsize or -blockmaxweight is given, limit to DEFAULT_BLOCK_MAX_*
    // If only one is given, only restrict the specified resource.
    // If both are given, restrict both.
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
    nBlockMaxSize = DEFAULT_BLOCK_MAX_SIZE;
    bool fWeightSet = false;
    if (mapArgs.count("-blockmaxweight")) {
        nBlockMaxWeight = GetArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
        nBlockMaxSize = MAX_BLOCK_SERIALIZED_SIZE;
        fWeightSet = true;
    }
    if (mapArgs.count("-blockmaxsize")) {
        nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
        if (!fWeightSet) {
            nBlockMaxWeight = nBlockMaxSize * WITNESS_SCALE_FACTOR;
        }
    }

    // Limit weight to between 4K and MAX_BLOCK_WEIGHT-4K for sanity:
    nBlockMaxWeight = std::max((unsigned int)4000, std::min((unsigned int)(MAX_BLOCK_WEIGHT-4000), nBlockMaxWeight));
    // Limit size to between 1K and MAX_BLOCK_SERIALIZED_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SERIALIZED_SIZE-1000), nBlockMaxSize));

    // Whether we need to account for byte usage (in addition to weight usage)
    fNeedSizeAccounting = (nBlockMaxSize < MAX_BLOCK_SERIALIZED_SIZE-1000);
}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockSize = 1000;
    nBlockWeight = 4000;
    nBlockSigOpsCost = 100;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;

    lastFewTxs = 0;
    blockFinished = false;
}

CBlockTemplate* BlockAssembler::CreateNewBlock(
    const CScript& scriptPubKeyIn,
    const vector<uint256>& tx_ids,bool fProofOfStake)
{
    // Create new block
    LogPrintf("BlockAssembler::CreateNewBlock()\n");

    const Consensus::Params &params = Params().GetConsensus();
    uint32_t nBlockTime;
    bool fMTP;
    {
        LOCK2(cs_main, mempool.cs);
        nBlockTime = GetAdjustedTime();
    }

    fMTP = false;
    int nFeeReductionFactor = 1;
    CAmount coin = COIN / nFeeReductionFactor;

    resetBlock();
    unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
        return NULL;
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience
    // Create coinbase tx
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    CBlockIndex* pindexPrev = chainActive.Tip();
    const int nHeight = pindexPrev->nHeight + 1;
    if (fProofOfStake)
    {
        // Make the coinbase tx empty in case of proof of stake
        coinbaseTx.vout[0].SetEmpty();
    } else {
        coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
        coinbaseTx.vout[0].nValue = 0;
    }
            //Initial premine enforcement
        if (params.IsMain() && (GetAdjustedTime() <= nStartRewardTime)) {
                throw std::runtime_error("CreateNewBlock() : Create new block too early");
        }
        if (nHeight == 2) {
        std::string snapshotaddrs[318] = {
         "i4gnB9PemtPbfCZMLeAGZ7mVKMcUGztSr6",
         "iLfh789rmG7pEBzwSmU7cfFeSxwFwViCi3",
         "iLmsmXwg17wNo5nHQ5aoJfnwxw4GgXF8iF",
         "iKBRADXNEvsDpib7mTxtFsk3p6G48amjiW",
         "iGFvJ9hQMHfD4t9HN37MNcVPUyNFj2VahM",
         "iRoMcni22BKoTBGMNb1tJz3gddERvS6CKo",
         "iQw8bMpJgNMAUmHYWhwa7KVENjoXCz1DFV",
         "i4EagztkxhELDwSmcZpx21moKwwc3B2XRb",
         "iHLhGZ32uoikX2xUjMPjtKNawBJ91PYMJn",
         "iHBKFt65BbB1yFxi7YTnnRqu9ve4AR8EUK",
         "iQ4ozHBzGciUj3yHwbYBmWY9zWuMEyRpTU",
         "iRxtyutHEQRw8pTpxXGvmEXAevKcM5HkD3",
         "iQQBaa66kqbMcEpVL4Beh6gBHgGUyp3BRY",
         "iHkpCSeEyvhyPLT3v1esBJSTJxCAWyZ5SM",
         "iE3joA7atYdSKMPm7qczQHFVQj5ruW85gU",
         "iMZoCwoyfT3REepjEQpjjiL855MZGs9m9t",
         "iPNUrRCyP1wRgabrDygXju7pKRJWNyyQEf",
         "iMQs9qofJtdq3ptG95qMzAD1xt5kAUnRWe",
         "i9brYMv2c4fg2D6sepERJeHx12JbbTfC4C",
         "iGFoeee6zZVqgK3Ndq48qRJSDb22cYwS6h",
         "i4CW5t57oY7N78yzPEggq9SALuTwRsW88t",
         "iAY1wDJCNxfQWR8V8mcm33LfbVvcNEvwWU",
         "iGGnkyoVeBzmVmo4TaYrSKRAvroDLXtbgM",
         "i4C3prh5wdi49F77CFG8ooEbG4sb4Pfwtn",
         "iL3ZKGzvfKiF263Bz6MTUFV3qNomDPeYdM",
         "iSZx8hNer7xut8MbzD7qDVMURUU82Eg7Bq",
         "iJh7gnWwbFvzU4VvqpjotxnuEGKDXvDmSg",
         "iA8KR5ChTMMcHdZTJEEMdR9CGJLwoZwhig",
         "iEdyFFpADzcQnsJTjTRukErsFqicM7mUV2",
         "iKAaT9s7Zqs2KoDVzubW5RpWwfTRkGzsed",
         "i3rL5c7K9TxgLs8HPhRoP58tSS8MkE5Tj9",
         "iMpYGzZuXXjkBcBF1aTtDwUFBmsxExFp6k",
         "iLQ2rezd7VYhH6ffbYWLyvrG4novGAutK4",
         "iRWcvLGP6VbHmMkTWjSdWmUAthrKdNExor",
         "iDKZ8VAFULrGQs1wncAaYR2emcn3HNH21M",
         "i5eF3Tm9QvmrXBrs19RnaqgUceoPDS5wGL",
         "iH6XvkkDSiyk9X9zrZvgKDEP3EFnKwrbkq",
         "iQxtgpKd2CHWqsaB9yZP6gkXTW2HHWs9LS",
         "iAvqDvfSKKF6QuLd6L6nStKTdnrhS7Yd95",
         "i51EyEgTWXapNs8myKfyavZH4nCWrEmL5z",
         "iPA2Yv8ZWcxW87TUi3Tvq53NgUabA9L2wB",
         "iJN6x4fdw9vnCPTDZU7fpGKeSMAg9TZzPe",
         "i4v5u39XXCax8U6HtMS3qG8w3b2hwYvFB8",
         "i7rajcFrHvL5WxeMjZHqxoLq7wZ81WGDTv",
         "iJ2RkPmyqHGqFtKVTbhezUXewCNpm9iv6m",
         "iBmbozecwYe2TubYZ55FeX7b2JWQtdvUif",
         "i8u339utPkpuvi71unxrv1fJKGGAgbu9nM",
         "i8YeAkniaQmo6moZis8jXDTyqCFbnBUaHa",
         "iDrykvqvU4sAHVHJ5FB87zdDa9GLsHZ1Az",
         "iCvEFQn4Csf2mgNQjZs9qhedhtdu4gfEV8",
         "iMirNTkAbeXmctcDvkURHAGU4FmbAmKeu6",
         "iLbULtz5ECevyTrP1hN5TBywc6p4pWBC3C",
         "iERv9co4hb45myvYczuMnXbBDM6jrjNoLU",
         "i8T7F6EAxbSNtJBDT82Xz3jwkN38jS5JHM",
         "iEQwnpaX8MAhcHr1mAFwnMgQqTCWtu2KCx",
         "iC2TCBxF7roaTi34eytcCZXJ3ytVUum4nn",
         "i8Ek9opYUKLcwrab3yvxLeZwmo15dK28Po",
         "i8nuwf4bjDU5sJWFtGh28YLmfKhhuYhmAg",
         "iK5NobVNNCHobtRHW7CzicN1cXUAZDhF4i",
         "i8BySXHDnhgRmrDfDAMQzZkikNpw8FmJwu",
         "iDDZn6ziz9hpyF6S81aMWA2PZmRCWAJqmJ",
         "iQqK1tT8nzmD3hXPFHNbJaK9p6Sc4g61FV",
         "i8LRxoGvyCXjt6SuRf3h5gcVMJDXkYayK4",
         "iE4khbboMdYQTztB6dZDpkuPsbC8DymEQM",
         "iSK6qfMRVQFRTtaW8sRNkxt9Fxx5Nrmr3N",
         "iSVEJJmscLZruVqFr9K7qbPQEgGu2cuk4x",
         "iLscBXTVHbzWorDEPp1r9zrcdSGBoe2jLL",
         "iJa1tp49QZMbd2RR3iBMzyRma18wUd5upa",
         "i67vjL36rrGvqMr75gTNQ5ihXD9Z893tbq",
         "i7jC5PZg36cT1zqPkSgrndRqsRGKSHvpvP",
         "iD2bWjH8rtqfMeqEoqeLGg6s7KrzopPQ6h",
         "i5CFNbydf91eCWJGu8KsPE92A96VJvjg2B",
         "iPokKp4nnJyNeCmJpoKEUXZvjPfETkDmNq",
         "iBAKLA2dN42fqGvmVEbJtBk3xjrWkEaTev",
         "iKPh3cGfcG6Zq8hVbQei7GBq2SfwstBPGw",
         "iKS7D5mBfMGzZqqFi8FZZkyWkw3WwcDQER",
         "i8c4LXZ9VRw7TVKscqQArLe2Ghsjvc3Yox",
         "i7kjvSFZTrTg1FtTraHzexxZCYnzn3T9Bm",
         "iJhgKW3Y2FCbu5EMBkvGvjKfSYFFjwsX68",
         "iPk1zPR46yvcairXkK878Xi4jrD6qZU1tF",
         "iDJyxtdqbmC4qdfpAbsu9dgStrctc2zkvH",
         "i3iiFHoALTXnyAbe4ijL4cHWW24o4AYpq1",
         "i6W1QYugj4o87Ze4iQS2hjsPYZ8xeWkjoc",
         "iJJqbbMpAWBe28szPVbSyz5sUebrUvCsQ1",
         "iJP2i6a93krApoXrqSzUQsfMCgvJQHJzK6",
         "i9bKss9dVJ4kWDP5XyLDGKHETJDqLkv1Uv",
         "iS1dw1cTvrbkFKssGsF6RkP2crvX7VbPLm",
         "iH9SBCLSzvV1tc3eYT5eeHrCDpTWGQ6BhR",
         "iDsNyfVRoLKhKXdfZMJYowcVFmRbnxSae1",
         "iGkqQXGBQQKtkhcyJ8cVYeakaeiuu1cSuZ",
         "iLKUBdQ8ZfXSLJj7bu56rNbiZpje7pn4Kz",
         "iQ8QeYDPLkHfBPPVxw8WZxv4RQzRXgiHE4",
         "i71vH8C6xrjbXFtnjyg2WKbJuw1ZETBMPw",
         "iL2PojbMx2hc8m8YYEWdHMWx5cyemGA6YN",
         "iBT1QQkW67EjeRbAWPCttnyVyjbcoahLCh",
         "i4nvgbv8dbVTXfLbioBBmduuNdDAzkLJp4",
         "iJobn8pdT7W6ftyWWtB6c6XRYjVkQtTz1j",
         "iSMpiyHxXDS6QiJrrXWVwdiijtjg9M7ta1",
         "iEiyw6CoqVGs8RZB3zX9GYpnv66o15eexY",
         "iGuaS1HG6B7WyCGsEnrcNz7KAW5u9d1WoG",
         "i621KCL4A4kdKKAvgRKYFPTRhznaHM7S4u",
         "iEYNNqeYSQUbufo8eFDnBSMEBc9LqYm4sq",
         "i3vnvjqSoBh4QdeVQCBC4TwVmd9GW1JYUL",
         "iJ4EL4gSTNwbdYUN3efAA2DkD7X2ubZUV5",
         "iCyn5mPT5HPukq9x2vwat84QyhEvyA2ERo",
         "iCcd9t8SeoUYyKraxUf1Rm6VN2q6bq4Zq7",
         "iAQ7h3kfWNiugMXdDP3g8cKd59pGt3RPjU",
         "i6C4qbvhMK9V8xiTTwCuDpBPNfvnotkYYn",
         "iAqhbZiErb5WYUKp2Q1ZNihEFk2yUM1Xnk",
         "i9ApgSzuFqjdz8SjptCNLSn7rGHiNbc8wU",
         "iAjh6gGyxPgL75yKxmvKUULyKCJJ8VHjuG",
         "i8RCmw1hQFXjEVSv2KhyghQfzayD1nj737",
         "iEXvwVJr9VNP3XV2AHr4wVTPcbd8NEugCN",
         "iEo3Z1WxL8ebuWY7zRjrexuimo8F9Y3KPJ",
         "iAxY2KnETLGjwJBvyEKFK9xFexpm764F6r",
         "iPrHHDN971RfWhBocJMgvbi4jYcaTCGB2N",
         "i4X1JVJ2uFStXbYy3XZVznUZEy93LQBEwM",
         "iMXpJF5TsBTENEXgzeVerBeDmvpJwe4FqC",
         "iEe9xkRojTKvXnJNUtfEH3j4gmWL2f5scp",
         "iLmaqHzwr5wchP9znR1NSuFkmtHEK7Jt6R",
         "iL1Ehw4ByvQBZQPDk9nZYbBkR5vrUoJwjh",
         "i9J9ZSVvwcoNPB31UbLvYUb7giWX1NTd7A",
         "iGrbTSa6AE4iAvEPcDTr2ehC9ynCWpcd4b",
         "iNKUUQuZJYqnw9fqBaU63KzcaPCZVYxUpf",
         "iKcUYnmXN2PsiBWmc5ZfwB6T4Z5RwHZ3je",
         "i4upoo6zwRqZwrjjXyMfawTAhNpWtHabrG",
         "iJoeb3jXykJXYVnvvG7RFayKzNPqGA2CoE",
         "i9AHA25g61Vgb528fYqayyGjPvG55DiUR4",
         "i9GV4vFcSbkkKTd9etuDE4NjU3TsgyPb6w",
         "i6KzbgainPiuEyAoDRQZQvevj8pGcr6693",
         "iQYxA5tiKxBtmuGSYU48wKwCYFv8X13AyM",
         "iMMM8i97CDW94XA95r3SjXXALwttddPK5j",
         "iL42TiGutk8PvrvtHCXmECex896ABv5eRD",
         "iEDV6jcT83uwwgPRvWJvB9kbuJ4fCxBfL3",
         "i3zggEd6EUCG4y6rpVFNa1BUtKqG1GU6c9",
         "iSjGZF5z6WxMc5Ho5hGrpzLVXLPPg7Jqdi",
         "i3yw6wUHAg5MwwuHmPScYkXB7XsmrKaLyo",
         "iGQrMK8VXBRaz82XdUazy7rNm1DSS1QkAk",
         "iSEpx1e7yPQ2jhG4AnSSFLir4c8yWExQiM",
         "i6QjnAQaJwK6C9nr6A3oapGAdhVRbsXQHf",
         "i6ixKiBoDQCcX45MNbqJU7vNB4To7ZBPnW",
         "i4mVrDQJfBD4vRBnSvUMgcttTjGj3DHuRz",
         "iAHXMJzWS7qMzoo2txBsXHXcZviwe2WsFd",
         "i94qdxPX8x6pVTUkoUpsqtfY4PjFpR8Q22",
         "iR3hrvYEu9KYaWFrPXzJCRVtCo9qkKuCEW",
         "i44L5hKRgVAG1WJThLVkvowsxhVxEUtE9K",
         "i7y7GSnkKHctbpmdj7tpZC3BkA98hrydBb",
         "i9sjxPfV42vAwQSsG5BN8ZHTYdGLsTCcwC",
         "iPCeBjvFcTkdfwVMXWXvrRrWH3z3ZvuZXw",
         "iAAYEVoWQG9HWkmvdbE6Fvvt1h3inCbTih",
         "iNGEukA3Dte6LGd9wPyP3CA5MGnrNh1un9",
         "iNe795aa4PorPicaY2h4eEMmbQtFqbTWE4",
         "i5eansLpvBuwgxF59WJsSCw8Xj4CMDGRor",
         "iFtZTMUhgxajksiSPfky3UjwMtnwaTYq4z",
         "iHo95XqSt9QLrmX3rKWXL13bq6TvpVPYpZ",
         "iLxHfHshUGU6SBESXVvVkEFT1YVxSEuKvF",
         "iLj7iKeGPxNJSXRA7mYt6QRwkWNDhThzs9",
         "iAz8nAuyafKTSYERxf9KrbQtcGh1c5s1z1",
         "i5wiGg2RnJijLXZzQb78eMyVtMmeSSczyw",
         "iNrPPCcoidij66dCHgoctBgzT8BR31RvRD",
         "i8snKZ5yhP3pnTv6zBUo2UmWeBnsGf1CAU",
         "i7HnnYQGUEofQGNEsc8tzXFYiAHhRaVaZ6",
         "iRkaPyP3Tjq4qmHazLs3gFuN1Wov6ruSbo",
         "i8pXVbvoCJJEa8xAfsi1SEiXqFygNNiY9T",
         "iNbjcVh7yNkCnYfQgiMWZP4gHvcczcAtkK",
         "i4AYmJr4UbKqxiBozXXG6MKFGw2KG8JWcU",
         "iQxrRkg2grctGczNN5ZP8MVEFS1JgAA8Kf",
         "i93mfzsXBmpKBaKijUXwxqvYz5UweaAanB",
         "i4cCtA3q4YwBsbTxB8xjVFma7wSFtjbxsq",
         "iQDturbkhPwntpJpL5rW471gEvRvdG533d",
         "iS34xQ8Ktq8jFAhAQFtoTEidamkHg1yY5u",
         "iFp56wkwvLi9X5SfwMXP9fRUcQqJ2QbhPz",
         "iLt2NmiSxkSCJ5zeLACMeAEqRZ9oVQMxb4",
         "i5pn8NruAQ9RU5u6jK2AW7GQEBpRnqQtdu",
         "iDdrk3ZMQjUdjbnPCAeV7tNVoJnjdfRGKs",
         "iEiwgUAcdF8syZ5dT7HD9Ye2mkhqx5uGDZ",
         "iQZ8M6TfT4bJpPWXEJVYsVgU7iPg85anzC",
         "iPbC7FJpQx8Arf815DU8yJCiZXQgJt4GVV",
         "i4mAoM8dzssUkRu9vj9EFxQdLrWKC5Uq4Q",
         "iHPYTDs1PcwitYxrChwz5huhx5R5DoAnnM",
         "i8WQ1Kg9mBKimGTSDbbspq9yQhqGiaC58e",
         "iSnc9bmsBaF6QxWfNkixGXzqRBGTqRrf5W",
         "i7VRsEsNJ1PenBbWxqKo1136ZVLoCuh2si",
         "iS1wjj66nVrbEc6gsecMT4c3iQYyxiuv3U",
         "i9Ue9BaWDrG6PM7uJqNnNqm17zGxRQRSP2",
         "iF5EVeYz8E1AnEqZkbGEpdYZsdwdMo4B8b",
         "iCJg6tbiz4VBLSo4RutQNHJuRbki1Ecg8M",
         "iJmrX1Biz5MP9ZaVAiq7tVa54G7whtYv52",
         "i8MKK224AX3KN6PMKBmWDeCw3q6gSEYq4o",
         "iKmzY8hyHUY36xQ27WJKfmAawkzbH9LgkL",
         "i7vtSVLBHTtBVh5FmJVaAwVCo29VbFzGiJ",
         "i3ktUre9Yes9Ae1wYEcyJyr7DQ3rdaWef4",
         "i41r94wBWZgzKebnjaCW4yGGNXxzN6Xw8f",
         "iSinWcLBhtZa7dJStr8FKMyn4Wsm4WUUqz",
         "i5vE4Aq7UjLF6rVaTXrEkcuviqAXiXfGE9",
         "iRa7YPKW9k9DTzWJRS9k5WbM5QBzkaiLF6",
         "iDo6DjiYif5wmok9qagu1nCtu446oCw8hg",
         "iQu6B2eEiADogpLWT44hwpDNQMCxCvzCar",
         "iQJiR1ra96rQCZ4HWShm9m9UQmXXnpveEL",
         "iAHk7GHXHtwEbzYEsJWPYveGJdnZqGTnUV",
         "iENgyYomhq4Bc9Lyd9ahLA5LYiSSF1n7EG",
         "iQwFEC2TPmPcd7jTUPU8YzvGi64z9aCU8B",
         "iSk5WhEN3zUpVYee77SfQy4arjKUzPEJSs",
         "i9moyCLM4YXofbvmz74f7s6JmyBC41djW7",
         "iMPV4qrcNVbC5XfCTZnSfTrNRxJvwQaLho",
         "iBGvkntujLWndgU1uJS4Rx1H4WgJG8KgJr",
         "iDitVyNhj3J1k9Aj3RTiUXpbhm2q92cPhF",
         "i6Zs2QwwcrsAixQab6wqt7epeCs9cuDbSn",
         "i6oKz7H3TzoeSddyDts9RKCEcwqjQW6iRh",
         "iMWczNcmUwEMe1CLPMert9DNeCqbvS6AGN",
         "iCatDSn67HCebSEmWxS7sk3gAVbLNGAXj4",
         "iB1ryXELiMWSnnUiNdQowET3EdbHi6h4YZ",
         "i7Cc5PVQWPWAowdUsFBXfPgU1mED6dPKGN",
         "iMgn7U9v7hHCTe6v1g22fAF1owCpZ6bC4U",
         "i9E89CHH2DbqPv8s53aCS9wHYB5AmZscgW",
         "i8KbstWAfAd4iz99mzJFQZ9FacVezPbFsS",
         "i6keqv7nt6og9hF2TgnNWsddUeEwghYj1w",
         "iGGHxgxkCWSjP6JFCRd6JNsEFRaaDDGNV8",
         "iM324fDvNkofkUmtWySXhHFTib1JeVtG87",
         "iQPFtbmD6UnQ3ocJb1yA2juNWPy4U92Mp5",
         "iNTAvqhbFybvmy4dPAhGJqcfbE1hEYqG5X",
         "iFg5NvnGUt21wrcq58KkinxJHiwq32kHtf",
         "iNX1UCo4JC5NFk3zAcYJ6tZPVXtuPpefqD",
         "iDuR6NZhoa7oYkv5gj2saKpC26JGSjAiF4",
         "iN5L9nS8xTaqWEbAizQZW3XgEmZZh91myf",
         "iCgJcpgkuxcXG32AtU7oaSohnTZ4CDKmur",
         "i3X8i76ogn4kEtxWpjsUC1VWAMj8RL7WiF",
         "iJswxKYi5xXB44Fpac2cGUzfqdX986zWiA",
         "iEctZMQsmfZ4yUYKNajEBKKAmEhA36aaqL",
         "i9i8Y5AKrpgy4yRG55NBwseQpr9rdJSJRw",
         "iEV9siE7e3Zo4Bt8MRVZTVS28xh7wDV2iP",
         "i6kZ6fQ1vGWDpcgLifsoHva7MZLFZqRYDY",
         "iSRFx5nf8UyKcw8P1pNerJYHizfpQKdTS1",
         "iQY7UUfBXyjrkDpszu6b2PMpfzVnbCnEfW",
         "iH6DHG3LLEu1iWXPFqhwXQdcFkjveFN6Za",
         "iKrPDuEgRVJvefaQKGKAm7tJE1jaFdg82Q",
         "iRAMKwgEinKw4YU3VfXSoJMNctoQnhN9dh",
         "iMfzbwU6yyYjhpp3r2RgL6xnXxbRKY8HBv",
         "i7fZ7yzLiYbTpEeE3nDU3sWZQTHRyzranC",
         "iFPBUaEaDgHzMjoTE8fqznns2H2qDz9EvX",
         "iP9VM3cTH4px198sEJrT6w2MLCo4QkLgF5",
         "i8nmVXxqkEyRroN3P6WCSA48Q2A9FZAk6W",
         "iPZaXz3Gn6sZ6s58Rxgp8yeMVfNRWHkC9h",
         "i9roeFEtj1CNBfPpV6HHjE61CKCGi1EPPQ",
         "iRFQH4WayThurGt2kSvDxCYxBh41sCCfiN",
         "iGKch61nJJaHxhpdkj1Mdq5sasiS7B65qV",
         "iKsWBCSygANrFg47nY62fBFrjmieRDQxC2",
         "iQPEP8nFX7XyxidC39hya2PpYtZrxQv32q",
         "iAUUnnCqgLTYmnwCiK55aSU3jRYv7gAgMR",
         "iLdvRT9xuT58FQVkj93KxmojVC8CHqaSVb",
         "iCozg6LZ3oqce8FmWUbP1gb6ZkQvMNe8yE",
         "i5qcY4ZgCcnmfU7Q9cjZBj9xfUpJnSHX6N",
         "iHNNs3D2irWjiwCT9nCPqvFJTJyjojByCn",
         "iFZujk8FM5hgogrkvYvubKm7txyMWK1NdE",
         "iMsirE26zq1j7vVSPAYbvAJbvnF7wmBiqU",
         "iR5JES9UtMqxtUT2xW5xB3FNMKcKUxAH5V",
         "i4ZKm59skTdRwdiMjjqCnVnVM1aQdeGWCY",
         "iJ7aAqoWacF3N4JJ9dYkLYoPoEgZcX6ufs",
         "i5gwRpuBGXHU47Rt61aVFKVzyiJai3fgXb",
         "i7D4fe19C4T2NpypSPPxqDcSCkHNg9XVg9",
         "iG7AepXiUwLrCjzojNUBoxm11ctGgTDB3G",
         "iAXERH9RdAfJZK6jJjYYFiVek1wnzaUMQG",
         "iEsCPFRESEBbQD5PKZviJKqyyadhjuGRZh",
         "iJFP98zzBrHdhvjhPsaAQkWKMtdp45EpkT",
         "iATuLZmLjAas48Tuf7ertfKd2Xsj3tFMn8",
         "iRMKTLH8GqtfcZtcp867c6gQzk3kyXmHkk",
         "iFSXftRU14mPj2i9Fb9gd1bPsbxpn58pPh",
         "i9Tqw5KpbsFsvVNvYJ9kt6bXsKznX9aQ25",
         "i4ddwiBmF9hS3pQC733dTCDaRyTgK1i9Wj",
         "iMxiJjVmRXdsurthc3Eabzjdo763GScdpq",
         "iNPM1T9y5dbpbnFafp32Jo69ckERc3hrdU",
         "i3XsfLk14smc7eTaSLXQWfDD4GvyDjGLR3",
         "iGQAswZub6y897tYR5G5UxeqJf3Mhwuhni",
         "i4nQU5aZXWX1fmbiMqqwoecP9Mu9C5V7UV",
         "iB5q1n3KEw55DXkaiS6LVaGVgEnz3nbhBT",
         "iR3SytJE9aSApgbXirERb9h3LHJJ9VHzor",
         "iR1Td9YtwytKcLbh1VMh4EV9NgboUSuNKZ",
         "iS1cyae3YAPyyqJ2VEvbtNS4qgBL1MvZw3",
         "iCLoKUVwpnCf4FkfiTywcKY4cRzDThnwp5",
         "iA4TGtfJzhS1dca35YFridvshtw5Pu4tNq",
         "iJx97KA753NnnJAc1gBgy2Z8xaRb4B7CQ9",
         "i8YJG8UaiXeZ5G6kH9Xv9kf2Ak3LeZ4Cq7",
         "iMv1oZHmDkUBTNwthRt8QcCDZ3Q9cr1sCp",
         "iF22KxStf3vUgiL5uGJUbhdjJtrhuMQ5Rn",
         "iJfNXtLoEg9pFvZW6f3X6Nuvn3MnCUVu3f",
         "iJYHqgS7LyeYSyAM2KNkzQi9RtGwWg9AFE",
         "iG1wXx9247WJx1GkEV6y1NySiYw6LkZLty",
         "i7e5drXBy3eAdLisK9ms27hehkmRj4Tgs1",
         "iCJy7b4eJiowGXD5G4bZDaZDpJNt7o8P7K",
         "iQr7r2G9FUbpy2FakhWSoBwh2zVbr6YTcL",
         "i8VpEPf6fi8npHij3FR1fy93cPgzeFbQjw",
         "iKyVdwSBH1MaLMgD3bfmwTNtYY6YaUjShA",
         "iH6x4AgEffiNYuiYwZ2Pp9dETVahRh8kpm",
         "i3iVv5v1bpxgnBqqwXVPcKczUjAaJzG9uR",
         "iScn4vg4X1Qd1h78oSzt7rYm9a7Kck2Pds",
         "iH8TUpmr5s8r328VZpeyn1ctGWKYPpoa4c",
         "iEk99a6zpLB31UuMNCuKbkHpiDk8EExtxb",
         "iNwLWWK2fx8Jdhjp7f3EMFBtdLuqfMerRY",
         "iNSR8CbdKxhMaxRqRZvfnimFMmu1Wpgq7D",
         "i5VKdrR9TtdpMGbLWio2eB7R1d889tLjTm",
         "i4FmmvcNyvY1HXoyMi1DRbFQVVPvfDhhQN",
         "iRxrJiBsHP5LdAt9MHmJ9KjUoz236jgiZX",
         "iCsLy7Cp9JFPz42sZ75KVsmCjyLpeXjX15",
         "iKm8MkQWXfNYzSDwMY26BfkwD96wp2F1Y9",
         "iCboKXVpagpFKtKUm8yTPWciAvFbaaexGQ",
         "i6BSQxX4wMqZz5DiZESTqEDVcwRjPbhjSD",
         "iRduEEBraeZRz4uw9v18qtiJUCogcwEX5X",
         "iKqyUavRZthsrXFhiKNCGvE33FS9Ntj4rp",
         "iPteD3LsAzTStV5pxUnKRT4rWUpuS62p9c",
         "iCFpEbneFHQQXWBCtSWfKgfPnKGN6spEhx",
         "iBxppu7F5YWrcvbvvWKEFniTRVdhUVi63y",
         "i5wzAnBggp6icEDDQ3AhKD8kvqZY558Tjb",
         "iPAcdapnxNzEXmhJGEcimKNGKcsnR6g6VU",
         "iPzVNv4qNCTmZNa7XpC13ui2FZUPYvpff9",
         "i4aq4nMbdVfetD1paTeonoqahyQMvLEPKg",
         "i6Zd6excRXMgcJ5hGaoJA8yXc1J59vHrhn",
         "i5RzbkMQvD9yKzuXGdqrA2k9wWmzUVFSRp",
         "iKMwdH46usxWscWeESYYt1y2FSBxiDx1LB"
        };
        CAmount addrbalances[] = {
    28800007644000000,
    598919544994000,
    598772778996000,
    497565473989000,
    6568180982626,
    6567108000000,
    6567104000000,
    6567101000000,
    6567091000000,
    6567088000000,
    6567085000000,
    6567078000000,
    6567078000000,
    6567075000000,
    6567071000000,
    6567069000000,
    6567065000000,
    5973074998000,
    4362051993000,
    1100016000000,
    628061999000,
    625482000000,
    521763101157,
    511031000000,
    510179000000,
    507661000000,
    506497000000,
    502387000000,
    502324000000,
    502307000000,
    501022000000,
    500116999000,
    500000000000,
    500000000000,
    96259999000,
    75515000000,
    75503000000,
    70000000000,
    65492998000,
    50440000000,
    50219000000,
    22236000000,
    20710900000,
    20000000000,
    10588999960,
    10213000000,
    10204000000,
    10204000000,
    10204000000,
    10204000000,
    10201000000,
    10198000000,
    10192000000,
    10135000000,
    10000000000,
    7230002000,
    6808998278,
    5000000000,
    4935945093,
    4443531523,
    3000000000,
    1788112580,
    1500125500,
    1359000000,
    1288685187,
    1268161127,
    1229830000,
    1118000000,
    1006000000,
    1000000000,
    996410000,
    967450000,
    896000000,
    879630000,
    876489209,
    861026380,
    825903504,
    776420573,
    745994052,
    711711833,
    647999999,
    641999999,
    638999999,
    629999999,
    626510610,
    610999000,
    595013651,
    589989003,
    559405056,
    499870000,
    489999999,
    485555555,
    483998000,
    482998999,
    480999999,
    476316017,
    475999998,
    439229560,
    435883233,
    426999000,
    420999999,
    383050000,
    372260500,
    361999999,
    361352292,
    353553556,
    352000000,
    349906488,
    347260725,
    337999999,
    334999000,
    333999000,
    330999000,
    330999000,
    327999000,
    327999000,
    327000000,
    325999999,
    325999999,
    325999999,
    325999999,
    325999000,
    325999000,
    324999999,
    323999999,
    323999999,
    323999000,
    323999000,
    323000000,
    322999999,
    322999000,
    322999000,
    321999000,
    319999000,
    318999999,
    318897472,
    315999999,
    313927850,
    310000000,
    308000000,
    306000000,
    304734379,
    300000000,
    299999999,
    292442445,
    288175880,
    279478382,
    271998000,
    249877723,
    225747071,
    212000000,
    209000000,
    209000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    200000000,
    199000000,
    199000000,
    196000000,
    196000000,
    193000000,
    193000000,
    187999000,
    187000000,
    187000000,
    187000000,
    184000000,
    182020478,
    178999000,
    178000000,
    178000000,
    175000000,
    175000000,
    173000000,
    172000000,
    172000000,
    171999000,
    166999000,
    166000000,
    166000000,
    164000000,
    163000000,
    161999999,
    161999000,
    154000000,
    151000000,
    150934788,
    139000000,
    134416920,
    134239617,
    130000000,
    121000000,
    116140003,
    115189493,
    115000000,
    112000000,
    109000000,
    109000000,
    109000000,
    108837450,
    106404989,
    106000000,
    104350000,
    103667306,
    103000000,
    103000000,
    100827081,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    100000000,
    93477737,
    69457846,
    63998000,
    62864664,
    59477000,
    57999000,
    53000000,
    48580000,
    41990059,
    39675647,
    38787900,
    36026160,
    32790263,
    30200122,
    30000000,
    30000000,
    28241704,
    28030290,
    24927069,
    21788631,
    19502360,
    18001374,
    16966160,
    15001065,
    14585485,
    12874977,
    11293880,
    9451927,
    7957138,
    6324740,
    6019580,
    5998000,
    4639099,
    4628406,
    4043225,
    3212294,
    3109753,
    3102627,
    3056058,
    3044103,
    3001180,
    2986000,
    2720073,
    2588800,
    2553959,
    2350440,
    1968200,
    1920000,
    1753520,
    1260000,
    1103041,
    1034524,
    1024400,
    1006033,
    995928,
    980361,
    978823,
    930785,
    835345,
    704563,
    351008,
    268835,
    220000,
    118342,
    89000,
    40000,
    7446
        };
        CScript SNAPSHOT_PAYEE_SCRIPT;
        coinbaseTx.vout[0].nValue = -306001473.12730116 * COIN;
        for(int i=0;i<318;i++){
            // Take some reward away from us
            SNAPSHOT_PAYEE_SCRIPT = GetScriptForDestination(CBitcoinAddress(snapshotaddrs[i]).Get());
            // And give it to the snapshot payee
            if(SNAPSHOT_PAYEE_SCRIPT != CScript())
                coinbaseTx.vout.push_back(CTxOut(addrbalances[i], SNAPSHOT_PAYEE_SCRIPT));
            continue;
        }
    }
    // Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to between 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SERIALIZED_SIZE - 1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    CTxMemPool::setEntries inBlock;
    CTxMemPool::setEntries waitSet;

    // This vector will be sorted into a priority queue:
    vector<TxCoinAgePriority> vecPriority;
    TxCoinAgePriorityCompare pricomparer;
    std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash> waitPriMap;
    typedef std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash>::iterator waitPriIter;
    double actualPriority = -1;

    std::priority_queue<CTxMemPool::txiter, std::vector<CTxMemPool::txiter>, ScoreCompare> clearedTxs;
    bool fPrintPriority = GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    uint64_t nBlockSize = 1500;
    uint64_t nBlockTx = 0;
    unsigned int nBlockSigOps = 100;
    int lastFewTxs = 0;
    CAmount nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        pblock->nTime = nBlockTime;
        const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

        pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
        // -regtest only: allow overriding block.nVersion with
        // -blockversion=N to test forking scenarios
        if (chainparams.MineBlocksOnDemand())
            pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

        int64_t nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                                  ? nMedianTimePast
                                  : pblock->GetBlockTime();

        bool fPriorityBlock = nBlockPrioritySize > 0;
        if (fPriorityBlock) {
            vecPriority.reserve(mempool.mapTx.size());
            for (CTxMemPool::indexed_transaction_set::iterator mi = mempool.mapTx.begin();
                 mi != mempool.mapTx.end(); ++mi)
            {
                double dPriority = mi->GetPriority(nHeight);
                CAmount dummy;
                mempool.ApplyDeltas(mi->GetTx().GetHash(), dPriority, dummy);
                vecPriority.push_back(TxCoinAgePriority(dPriority, mi));
            }
            std::make_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
        }

        CTxMemPool::indexed_transaction_set::nth_index<3>::type::iterator mi = mempool.mapTx.get<3>().begin();
        CTxMemPool::txiter iter;
        std::size_t nSigmaSpend = 0;
        CAmount nValueSigmaSpend(0);

        while (mi != mempool.mapTx.get<3>().end() || !clearedTxs.empty())
        {
            bool priorityTx = false;
            if (fPriorityBlock && !vecPriority.empty()) { // add a tx from priority queue to fill the blockprioritysize
                priorityTx = true;
                iter = vecPriority.front().second;
                actualPriority = vecPriority.front().first;
                std::pop_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
                vecPriority.pop_back();
            }
            else if (clearedTxs.empty()) { // add tx with next highest score
                iter = mempool.mapTx.project<0>(mi);
                mi++;
            }
            else {  // try to add a previously postponed child tx
                iter = clearedTxs.top();
                clearedTxs.pop();
            }

            if (inBlock.count(iter)) {
                LogPrintf("skip, due to exist!\n");
                continue; // could have been added to the priorityBlock
            }

            const CTransaction& tx = iter->GetTx();
            LogPrintf("Trying to add tx=%s\n", tx.GetHash().ToString());

            if (!tx_ids.empty() && std::find(tx_ids.begin(), tx_ids.end(), tx.GetHash()) == tx_ids.end()) {
                continue; // Skip because we were asked to include only transactions in tx_ids.
            }

            bool fOrphan = false;
            BOOST_FOREACH(CTxMemPool::txiter parent, mempool.GetMemPoolParents(iter))
            {
                if (!inBlock.count(parent)) {
                    fOrphan = true;
                    break;
                }
            }
            if (fOrphan) {
                if (priorityTx)
                    waitPriMap.insert(std::make_pair(iter,actualPriority));
                else waitSet.insert(iter);
                LogPrintf("skip tx=%s, due to fOrphan=%s\n", tx.GetHash().ToString(), fOrphan);
                continue;
            }

            unsigned int nTxSize = iter->GetTxSize();
            if (fPriorityBlock &&
                (nBlockSize + nTxSize >= nBlockPrioritySize || !AllowFree(actualPriority))) {
                fPriorityBlock = false;
                waitPriMap.clear();
            }
//            if (!priorityTx && (iter->GetModifiedFee() < ::minRelayTxFee.GetFee(nTxSize) && nBlockSize >= nBlockMinSize)) {
//                LogPrintf("skip tx=%s\n", tx.GetHash().ToString());
//                LogPrintf("iter->GetModifiedFee()=%s\n", iter->GetModifiedFee());
//                LogPrintf("::minRelayTxFee.GetFee(nTxSize)=%s\n", ::minRelayTxFee.GetFee(nTxSize));
//                LogPrintf("nBlockSize=%s\n", nBlockSize);
//                LogPrintf("nBlockMinSize=%s\n", nBlockMinSize);
//                LogPrintf("***********************************");
//                break;
//            }
            if (nBlockSize + nTxSize >= nBlockMaxSize) {
                if (nBlockSize >  nBlockMaxSize - 100 || lastFewTxs > 50) {
                    LogPrintf("stop due to size overweight", tx.GetHash().ToString());
                    LogPrintf("nBlockSize=%s\n", nBlockSize);
                    LogPrintf("nBlockMaxSize=%s\n", nBlockMaxSize);
                    break;
                }
                // Once we're within 1000 bytes of a full block, only look at 50 more txs
                // to try to fill the remaining space.
                if (nBlockSize > nBlockMaxSize - 1000) {
                    lastFewTxs++;
                }
                LogPrintf("skip tx=%s\n", tx.GetHash().ToString());
                LogPrintf("nBlockSize=%s\n", nBlockSize);
                LogPrintf("nBlockMaxSize=%s\n", nBlockMaxSize);
                continue;
            }
            if (tx.IsCoinBase()) {
                LogPrintf("skip tx=%s, coinbase tx\n", tx.GetHash().ToString());
                continue;
            }

            if (!IsFinalTx(tx, nHeight, nLockTimeCutoff)) {
                LogPrintf("skip tx=%s, not IsFinalTx\n", tx.GetHash().ToString());
                continue;
            }

            if (tx.IsSigmaMint() || tx.IsSigmaSpend()) {
                sigma::CSigmaState * sigmaState = sigma::CSigmaState::GetState();
                if(sigmaState->IsSurgeConditionDetected())
                    continue;
            }

            // temporarily disable zerocoin. Re-enable after sigma release
            // Make exception for regtest network (for remint tests)
            if (!chainparams.GetConsensus().IsRegtest() && (tx.IsZerocoinSpend() || tx.IsZerocoinMint()))
                continue;

            if(tx.IsSigmaSpend() && nHeight >= chainparams.GetConsensus().nDisableUnpaddedSigmaBlock && nHeight < chainparams.GetConsensus().nSigmaPaddingBlock)
                continue;

            if (tx.IsSigmaSpend() || tx.IsZerocoinRemint()) {
                // Sigma spend and zerocoin->sigma remint are subject to the same limits
                CAmount spendAmount = tx.IsSigmaSpend() ? sigma::GetSpendAmount(tx) : sigma::CoinRemintToV3::GetAmount(tx);

                if (tx.vin.size() > params.nMaxSigmaInputPerTransaction ||
                    spendAmount > params.nMaxValueSigmaSpendPerTransaction) {
                    continue;
                }
                if (tx.vin.size() + nSigmaSpend > params.nMaxSigmaInputPerBlock) {
                    continue;
                }
                if (spendAmount + nValueSigmaSpend > params.nMaxValueSigmaSpendPerBlock) {
                    continue;
                }

                //mempool.countZCSpend--;
                // Size limits
                unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

                LogPrintf("\n\n######################################\n");
                LogPrintf("nBlockMaxSize = %d\n", nBlockMaxSize);
                LogPrintf("nBlockSize = %d\n", nBlockSize);
                LogPrintf("nTxSize = %d\n", nTxSize);
                LogPrintf("nBlockSize + nTxSize  = %d\n", nBlockSize + nTxSize);
                LogPrintf("nBlockSigOpsCost  = %d\n", nBlockSigOpsCost);
                LogPrintf("GetLegacySigOpCount  = %d\n", GetLegacySigOpCount(tx));
                LogPrintf("######################################\n\n\n");

                if (nBlockSize + nTxSize >= nBlockMaxSize) {
                    LogPrintf("failed by sized\n");
                    continue;
                }

                // Legacy limits on sigOps:
                unsigned int nTxSigOps = GetLegacySigOpCount(tx);
                if (nBlockSigOpsCost + nTxSigOps >= MAX_BLOCK_SIGOPS_COST) {
                    LogPrintf("failed by sized\n");
                    continue;
                }

                CAmount nTxFees = iter->GetFee();

                pblock->vtx.push_back(tx);
                pblocktemplate->vTxFees.push_back(nTxFees);
                pblocktemplate->vTxSigOpsCost.push_back(nTxSigOps);
                nBlockSize += nTxSize;
                ++nBlockTx;
                nBlockSigOpsCost += nTxSigOps;
                nFees += nTxFees;
                nSigmaSpend += tx.vin.size();
                nValueSigmaSpend += spendAmount;
                inBlock.insert(iter);
                continue;
            }


            unsigned int nTxSigOps = iter->GetSigOpCost();
            LogPrintf("nTxSigOps=%s\n", nTxSigOps);
            LogPrintf("nBlockSigOps=%s\n", nBlockSigOps);
            LogPrintf("MAX_BLOCK_SIGOPS_COST=%s\n", MAX_BLOCK_SIGOPS_COST);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS_COST) {
                if (nBlockSigOps > MAX_BLOCK_SIGOPS_COST - 2) {
                    LogPrintf("stop due to cross fee\n", tx.GetHash().ToString());
                    break;
                }
                LogPrintf("skip tx=%s, nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS_COST\n", tx.GetHash().ToString());
                continue;
            }
            CAmount nTxFees = iter->GetFee();
            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOpsCost.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;
            LogPrintf("added to block=%s\n", tx.GetHash().ToString());
            if (fPrintPriority)
            {
                double dPriority = iter->GetPriority(nHeight);
                CAmount dummy;
                mempool.ApplyDeltas(tx.GetHash(), dPriority, dummy);
                LogPrintf("priority %.1f fee %s txid %s\n",
                          dPriority , CFeeRate(iter->GetModifiedFee(), nTxSize).ToString(), tx.GetHash().ToString());
            }

            inBlock.insert(iter);

            // Add transactions that depend on this one to the priority queue
            BOOST_FOREACH(CTxMemPool::txiter child, mempool.GetMemPoolChildren(iter))
            {
                if (fPriorityBlock) {
                    waitPriIter wpiter = waitPriMap.find(child);
                    if (wpiter != waitPriMap.end()) {
                        vecPriority.push_back(TxCoinAgePriority(wpiter->second,child));
                        std::push_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
                        waitPriMap.erase(wpiter);
                    }
                }
                else {
                    if (waitSet.count(child)) {
                        clearedTxs.push(child);
                        waitSet.erase(child);
                    }
                }
            }
        }
        CAmount blockReward = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus(), nBlockTime);
        // Update coinbase transaction with additional info about indexnode and governance payments,
        // get some info back to pass to getblocktemplate
        bool fPayIDXNode = nHeight >= chainparams.GetConsensus().nIndexnodePaymentsStartBlock && !fProofOfStake;
        CAmount indexnodePayment = 0;
        if (fPayIDXNode) {
            const Consensus::Params &params = chainparams.GetConsensus();
            indexnodePayment = GetIndexnodePayment(chainparams.GetConsensus(),false,nHeight);
            FillBlockPayments(coinbaseTx, nHeight, indexnodePayment, pblock->txoutIndexnode, pblock->voutSuperblock);
        }
        //Only take out idx payment if a indexnode is actually filled in txoutindexnode and indexnodepayment is not 0
        if(pblock->txoutIndexnode != CTxOut() && indexnodePayment != 0)
            coinbaseTx.vout[0].nValue -= indexnodePayment;

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        LogPrintf("CreateNewBlock(): total size %u txs: %u fees: %ld sigops %d\n", nBlockSize, nBlockTx, nFees, nBlockSigOps);

        // Compute final coinbase transaction.
        if(!fProofOfStake)//Only Set vout of coinbasetx as blockreward in PoW Blocks
            coinbaseTx.vout[0].nValue += blockReward;
        coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
        pblock->vtx[0] = coinbaseTx;
        pblocktemplate->vTxFees[0] = -nFees;

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        if(!fProofOfStake){
            UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
        }
        pblock->nBits  = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus(),fProofOfStake);
        pblock->nNonce = fProofOfStake ? 0 : 1;
        pblocktemplate->vTxSigOpsCost[0] = GetLegacySigOpCount(pblock->vtx[0]);

        //LogPrintf("CreateNewBlock(): AFTER pblocktemplate->vTxSigOpsCost[0] = GetLegacySigOpCount(pblock->vtx[0])\n");

        CValidationState state;
        //LogPrintf("CreateNewBlock(): BEFORE TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)\n");
        if ( !fProofOfStake && !TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
            throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
        }
        //LogPrintf("CreateNewBlock(): AFTER TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)\n");
    }
    //LogPrintf("CreateNewBlock(): pblocktemplate.release()\n");
    return pblocktemplate.release();
}


CBlockTemplate* BlockAssembler::CreateNewBlockWithKey(CReserveKey &reservekey) {
    LogPrintf("CreateNewBlockWithKey()\n");
    CPubKey pubkey;
    if (!reservekey.GetReservedKey(pubkey))
        return NULL;

    CScript scriptPubKey = CScript() << pubkey << OP_CHECKSIG;
//    CScript scriptPubKey = GetScriptForDestination(pubkey.GetID());;
    return CreateNewBlock(scriptPubKey, {},false);
}

bool BlockAssembler::isStillDependent(CTxMemPool::txiter iter)
{
    BOOST_FOREACH(CTxMemPool::txiter parent, mempool.GetMemPoolParents(iter))
    {
        if (!inBlock.count(parent)) {
            return true;
        }
    }
    return false;
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost)
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST)
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
// - serialized size (in case -blockmaxsize is in use)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package)
{
    uint64_t nPotentialBlockSize = nBlockSize; // only used with fNeedSizeAccounting
    BOOST_FOREACH (const CTxMemPool::txiter it, package) {
        if (!IsFinalTx(it->GetTx(), nHeight, nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && !it->GetTx().wit.IsNull())
            return false;
        if (fNeedSizeAccounting) {
            uint64_t nTxSize = ::GetSerializeSize(it->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
            if (nPotentialBlockSize + nTxSize >= nBlockMaxSize) {
                return false;
            }
            nPotentialBlockSize += nTxSize;
        }
    }
    return true;
}

bool BlockAssembler::TestForBlock(CTxMemPool::txiter iter)
{
    LogPrintf("\nTestForBlock ######################################\n");
    LogPrintf("nBlockMaxSize = %d\n", nBlockMaxSize);
    LogPrintf("nBlockSize = %d\n", nBlockSize);
    int nTxSize = ::GetSerializeSize(iter->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
    LogPrintf("nTxSize = %d\n", nTxSize);
    LogPrintf("nBlockSize + nTxSize  = %d\n", nBlockSize + nTxSize);
    LogPrintf("######################################\n\n\n");
    LogPrintf("nBlockWeight = %d\n", nBlockWeight);
    LogPrintf("iter->GetTxWeight() = %d\n", iter->GetTxWeight());
    LogPrintf("nBlockWeight = %d\n", nBlockWeight);
    LogPrintf("lastFewTxs = %d\n", lastFewTxs);
    LogPrintf("######################################\n\n\n");

    if (nBlockWeight + iter->GetTxWeight() >= nBlockMaxWeight) {
        // If the block is so close to full that no more txs will fit
        // or if we've tried more than 50 times to fill remaining space
        // then flag that the block is finished
        if (nBlockWeight >  nBlockMaxWeight - 400 || lastFewTxs > 50) {
             blockFinished = true;
             LogPrintf("\nTestForBlock -> FAIL: blockFinished = true\n");
             return false;
        }
        // Once we're within 4000 weight of a full block, only look at 50 more txs
        // to try to fill the remaining space.
        if (nBlockWeight > nBlockMaxWeight - 4000) {
            lastFewTxs++;
        }
        LogPrintf("\nTestForBlock -> FAIL: nBlockWeight + iter->GetTxWeight() >= nBlockMaxWeight\n");
        return false;
    }

    if (fNeedSizeAccounting) {
        if (nBlockSize + ::GetSerializeSize(iter->GetTx(), SER_NETWORK, PROTOCOL_VERSION) >= nBlockMaxSize) {
            if (nBlockSize >  nBlockMaxSize - 100 || lastFewTxs > 50) {
                 blockFinished = true;
                 LogPrintf("\nTestForBlock -> FAIL: fNeedSizeAccounting: blockFinished = true\n");
                 return false;
            }
            if (nBlockSize > nBlockMaxSize - 1000) {
                lastFewTxs++;
            }
            LogPrintf("\nTestForBlock -> FAIL: fNeedSizeAccounting\n");
            return false;
        }
    }

    if (nBlockSigOpsCost + iter->GetSigOpCost() >= MAX_BLOCK_SIGOPS_COST) {
        // If the block has room for no more sig ops then
        // flag that the block is finished
        if (nBlockSigOpsCost > MAX_BLOCK_SIGOPS_COST - 8) {
            blockFinished = true;
            LogPrintf("\nTestForBlock -> FAIL: nBlockSigOpsCost: blockFinished = true\n");
            return false;
        }
        // Otherwise attempt to find another tx with fewer sigops
        // to put in the block.
        LogPrintf("\nTestForBlock -> FAIL: nBlockSigOpsCost\n");
        return false;
    }

    // Must check that lock times are still valid
    // This can be removed once MTP is always enforced
    // as long as reorgs keep the mempool consistent.
    if (!IsFinalTx(iter->GetTx(), nHeight, nLockTimeCutoff)) {
        LogPrintf("\nTestForBlock -> FAIL: !IsFinalTx()\n");
        return false;
    }

    LogPrintf("\nTestForBlock -> OK\n");
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.push_back(iter->GetTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    if (fNeedSizeAccounting) {
        nBlockSize += ::GetSerializeSize(iter->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
    }
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        double dPriority = iter->GetPriority(nHeight);
        CAmount dummy;
        mempool.ApplyDeltas(iter->GetTx().GetHash(), dPriority, dummy);
        LogPrintf("priority %.1f fee %s txid %s\n",
                  dPriority,
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

void BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    BOOST_FOREACH(const CTxMemPool::txiter it, alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        BOOST_FOREACH(CTxMemPool::txiter desc, descendants) {
            if (alreadyAdded.count(desc))
                continue;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert (it != mempool.mapTx.end());
    if (mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it))
        return true;
    return false;
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, CTxMemPool::txiter entry, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs()
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;
    while (mi != mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty())
    {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score>().end() &&
                SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareModifiedEntry()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < ::minRelayTxFee.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // Package can be added. Sort the entries in a valid order.
        vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, iter, sortedEntries);

        for (size_t i=0; i<sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        // Update transactions that depend on each of these
        UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void BlockAssembler::addPriorityTxs()
{
    // Largest block you're willing to create:
    nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SERIALIZED_SIZE - 1000), nBlockMaxSize));
    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

//    if (nBlockPrioritySize == 0) {
//        return;
//    }

    bool fSizeAccounting = fNeedSizeAccounting;
    fNeedSizeAccounting = true;

    // This vector will be sorted into a priority queue:
    vector<TxCoinAgePriority> vecPriority;
    TxCoinAgePriorityCompare pricomparer;
    std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash> waitPriMap;
    typedef std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash>::iterator waitPriIter;
    double actualPriority = -1;

    vecPriority.reserve(mempool.mapTx.size());
    for (CTxMemPool::indexed_transaction_set::iterator mi = mempool.mapTx.begin();
         mi != mempool.mapTx.end(); ++mi)
    {
        double dPriority = mi->GetPriority(nHeight);
        CAmount dummy;
        CTransaction tx = mi->GetTx();
        mempool.ApplyDeltas(tx.GetHash(), dPriority, dummy);
        vecPriority.push_back(TxCoinAgePriority(dPriority, mi));
        //add Index validation
        if (tx.IsCoinBase() || !CheckFinalTx(tx))
            continue;
        if (tx.IsZerocoinSpend() || tx.IsSigmaSpend() || tx.IsZerocoinRemint()) {
            //mempool.countZCSpend--;
            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

            LogPrintf("\n\n######################################\n");
            LogPrintf("nBlockMaxSize = %d\n", nBlockMaxSize);
            LogPrintf("nBlockSize = %d\n", nBlockSize);
            LogPrintf("nTxSize = %d\n", nTxSize);
            LogPrintf("nBlockSize + nTxSize  = %d\n", nBlockSize + nTxSize);
            LogPrintf("######################################\n\n\n");

            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOpsCost + nTxSigOps >= MAX_BLOCK_SIGOPS_COST)
                continue;

            CAmount nTxFees(0);
            if (tx.IsSigmaSpend())
                nTxFees = mi->GetFee();

            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOpsCost.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOpsCost += nTxSigOps;
            nFees += nTxFees;
            continue;
        }
    }
    std::make_heap(vecPriority.begin(), vecPriority.end(), pricomparer);

    CTxMemPool::txiter iter;
    while (!vecPriority.empty() && !blockFinished) { // add a tx from priority queue to fill the blockprioritysize
        iter = vecPriority.front().second;
        actualPriority = vecPriority.front().first;
        std::pop_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
        vecPriority.pop_back();

        // If tx already in block, skip
        if (inBlock.count(iter)) {
            assert(false); // shouldn't happen for priority txs
            continue;
        }

        // cannot accept witness transactions into a non-witness block
        if (!fIncludeWitness && !iter->GetTx().wit.IsNull())
            continue;

        // If tx is dependent on other mempool txs which haven't yet been included
        // then put it in the waitSet
        if (isStillDependent(iter)) {
            waitPriMap.insert(std::make_pair(iter, actualPriority));
            continue;
        }

        // If this tx fits in the block add it, otherwise keep looping
        if (TestForBlock(iter)) {
            AddToBlock(iter);

            // If now that this txs is added we've surpassed our desired priority size
            // or have dropped below the AllowFreeThreshold, then we're done adding priority txs
            if (nBlockSize >= nBlockPrioritySize || !AllowFree(actualPriority)) {
                break;
            }

            // This tx was successfully added, so
            // add transactions that depend on this one to the priority queue to try again
            BOOST_FOREACH(CTxMemPool::txiter child, mempool.GetMemPoolChildren(iter))
            {
                waitPriIter wpiter = waitPriMap.find(child);
                if (wpiter != waitPriMap.end()) {
                    vecPriority.push_back(TxCoinAgePriority(wpiter->second,child));
                    std::push_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
                    waitPriMap.erase(wpiter);
                }
            }
        }
    }
    fNeedSizeAccounting = fSizeAccounting;
}

//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

//
// ScanHash scans nonces looking for a hash with at least some zero bits.
// The nonce is usually preserved between calls, but periodically or if the
// nonce is 0xffff0000 or above, the block is rebuilt and nNonce starts over at
// zero.
//
//bool static ScanHash(const CBlockHeader *pblock, uint32_t& nNonce, uint256 *phash)
//{
//    // Write the first 76 bytes of the block header to a double-SHA256 state.
//    CHash256 hasher;
//    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
//    ss << *pblock;
//    assert(ss.size() == 80);
//    hasher.Write((unsigned char*)&ss[0], 76);
//
//    while (true) {
//        nNonce++;
//
//        // Write the last 4 bytes of the block header (the nonce) to a copy of
//        // the double-SHA256 state, and compute the result.
//        CHash256(hasher).Write((unsigned char*)&nNonce, 4).Finalize((unsigned char*)phash);
//
//        // Return the nonce if the hash has at least some zero bits,
//        // caller will check if it has enough to reach the target
//        if (((uint16_t*)phash)[15] == 0)
//            return true;
//
//        // If nothing found after trying for a while, return -1
//        if ((nNonce & 0xfff) == 0)
//            return false;
//    }
//}

static bool ProcessBlockFound(const CBlock* pblock, const CChainParams& chainparams)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("BitcoinMiner: generated block is stale");
    }

    // Inform about the new block
    GetMainSignals().BlockFound(pblock->GetHash());

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(state, chainparams, NULL, pblock, true, NULL, false))
        return error("ZcoinMiner: ProcessNewBlock, block not accepted");

    return true;
}

void static ZcoinMiner(const CChainParams &chainparams) {
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("index-miner");

    unsigned int nExtraNonce = 0;

    boost::shared_ptr<CReserveScript> coinbaseScript;
    GetMainSignals().ScriptForMining(coinbaseScript);
    bool fTestNet = chainparams.GetConsensus().IsTestnet();
    try {
        // Throw an error if no script was provided.  This can happen
        // due to some internal error but also if the keypool is empty.
        // In the latter case, already the pointer is NULL.
        if (!coinbaseScript || coinbaseScript->reserveScript.empty()) {
            LogPrintf("ZcoinMiner stop here coinbaseScript=%s, coinbaseScript->reserveScript.empty()=%s\n", coinbaseScript, coinbaseScript->reserveScript.empty());
            throw std::runtime_error("No coinbase script available (mining requires a wallet)");
        }

        while (true) {
            if (chainparams.MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.

                // Also try to wait for indexnode winners unless we're on regtest chain
                do {
                    bool fvNodesEmpty;
                    bool fHasIndexnodesWinnerForNextBlock;
                    const Consensus::Params &params = chainparams.GetConsensus();
                    {
                        LOCK(cs_vNodes);
                        fvNodesEmpty = vNodes.empty();
                    }
                    {
                        LOCK2(cs_main, mempool.cs);
                        int nCount = 0;
                        fHasIndexnodesWinnerForNextBlock =
                                params.IsRegtest() ||
                                chainActive.Height() < params.nIndexnodePaymentsStartBlock ||
                                mnodeman.GetNextIndexnodeInQueueForPayment(chainActive.Height(), true, nCount);
                    }
                    if (!fvNodesEmpty && fHasIndexnodesWinnerForNextBlock && !IsInitialBlockDownload()) {
                        break;
                    }
                    MilliSleep(1000);
                } while (true);
            }
            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex *pindexPrev = chainActive.Tip();
            if (pindexPrev) {
                LogPrintf("loop pindexPrev->nHeight=%s\n", pindexPrev->nHeight);
            }
            unique_ptr <CBlockTemplate> pblocktemplate(BlockAssembler(Params()).CreateNewBlock(
                coinbaseScript->reserveScript, {}));
            if (!pblocktemplate.get()) {
                LogPrintf("Error in ZcoinMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            LogPrintf("Running ZcoinMiner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
                      ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);
            LogPrintf("hashTarget: %s\n", hashTarget.ToString());
            LogPrintf("fTestnet: %d\n", fTestNet);
            LogPrintf("pindexPrev->nHeight: %s\n", pindexPrev->nHeight);
            LogPrintf("pblock: %s\n", pblock->ToString());
            LogPrintf("pblock->nVersion: %s\n", pblock->nVersion);
            LogPrintf("pblock->nTime: %s\n", pblock->nTime);
            LogPrintf("pblock->nNonce: %s\n", &pblock->nNonce);
            LogPrintf("powLimit: %s\n", Params().GetConsensus().powLimit.ToString());

            while (true) {
                // Check if something found
                uint256 thash;
                   ///change to x116rv3
                while (true) {
                    thash = pblock->GetPoWHash();

                    //LogPrintf("*****\nhash   : %s  \ntarget : %s\n", UintToArith256(thash).ToString(), hashTarget.ToString());

                    if (UintToArith256(thash) <= hashTarget) {
                        // Found a solution
                        LogPrintf("Found a solution. Hash: %s", UintToArith256(thash).ToString());
                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
//                        CheckWork(pblock, *pwallet, reservekey);
                        LogPrintf("ZcoinMiner:\n");
                        LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", UintToArith256(thash).ToString(), hashTarget.ToString());

                        if(!ProcessBlockFound(pblock, chainparams)){
                            break;
                            throw boost::thread_interrupted();
                        }
                        SetThreadPriority(THREAD_PRIORITY_LOWEST);
                        coinbaseScript->KeepScript();
                        // In regression test mode, stop mining after a block is found.
                        if (chainparams.MineBlocksOnDemand())
                            throw boost::thread_interrupted();
                        break;
                    }
                    pblock->nNonce += 1;
                    if ((pblock->nNonce & 0xFF) == 0)
                        break;
                }
                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();
                // Regtest mode doesn't require peers
                if (vNodes.empty() && chainparams.MiningRequiresPeers())
                    break;
                if (pblock->nNonce >= 0xffff0000)
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;
                if (pindexPrev != chainActive.Tip())
                    break;

                // Update nTime every few seconds
                if (UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev) < 0)
                    break; // Recreate the block if the clock has run backwards,
                // so that we can use the correct time.
                if (chainparams.GetConsensus().fPowAllowMinDifficultyBlocks) {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch (const boost::thread_interrupted &) {
        LogPrintf("ZcoinMiner terminated\n");
        throw;
    }
    catch (const std::runtime_error &e) {
        LogPrintf("ZcoinMiner runtime error: %s\n", e.what());
        return;
    }
}

void GenerateBitcoins(bool fGenerate, int nThreads, const CChainParams& chainparams)
{
    static boost::thread_group* minerThreads = NULL;

    if (nThreads < 0)
        nThreads = GetNumCores();

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(boost::bind(&ZcoinMiner, boost::cref(chainparams)));
}
	void ThreadStakeMiner(CWallet *pwallet, const CChainParams& chainparams)
{
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    LogPrintf("Staking started\n");

    // Make this thread recognisable as the mining thread
    RenameThread("index-staker");

    CReserveKey reservekey(pwallet);

    bool fTestNet = (Params().NetworkIDString() == CBaseChainParams::TESTNET);
    bool fTryToSync = true;
    while (true)
    {
        CBlockIndex* pindexPrev = chainActive.Tip();
        const int nHeight = pindexPrev->nHeight + 1;            
        if (nHeight >= Params().GetConsensus().nFirstPOSBlock)
        {
            while (pwallet->IsLocked())
            {
                nLastCoinStakeSearchInterval = 0;
                MilliSleep(10000);
            }
            while (vNodes.empty() || IsInitialBlockDownload() || !indexnodeSync.IsSynced())
            {
                nLastCoinStakeSearchInterval = 0;
                fTryToSync = true;
                MilliSleep(1000);
            }
            if (fTryToSync)
            {
                fTryToSync = false;
                if (vNodes.size() < 2 || pindexBestHeader->GetBlockTime() < GetTime() - 10 * 60)
                {
                    MilliSleep(6000);
                    continue;
                }
            }

            //
            // Create new block
            //
            if (pwallet->HaveAvailableCoinsForStaking()) {
                int64_t nFees = 0;
                // First just create an empty block. No need to process transactions until we know we can create a block
                std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(Params()).CreateNewBlock(reservekey.reserveScript, {},true));
                if (!pblocktemplate.get()) {
                    LogPrintf("ThreadStakeMiner(): Could not get Blocktemplate\n");
                    return;
                }

                CBlock *pblock = &pblocktemplate->block;
                // Trying to sign a block
                if (SignBlock(*pblock, *pwallet, nFees, pblocktemplate.get()))
                {
                    // increase priority
                    SetThreadPriority(THREAD_PRIORITY_ABOVE_NORMAL);
                     // Check if stake check passes and process the new block
                    CheckStake(pblock, *pwallet, chainparams);
                    // return back to low priority
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    MilliSleep(5000);
                }
            }
            MilliSleep(nMinerSleep);
        }
        MilliSleep(10000);
    }
}
void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}
