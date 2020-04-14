#include "main.h"
#include "zerocoin.h"
#include "sigma.h"
#include "timedata.h"
#include "chainparams.h"
#include "util.h"
#include "base58.h"
#include "definition.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#include "indexnode-payments.h"
#include "indexnode-sync.h"
#include "sigma/remint.h"

#include <atomic>
#include <sstream>
#include <chrono>

#include <boost/foreach.hpp>

using namespace std;

// Settings
int64_t nTransactionFee = 0;
int64_t nMinimumInputValue = DUST_HARD_LIMIT;

// btzc: add zerocoin init
// zerocoin init
static CBigNum bnTrustedModulus(ZEROCOIN_MODULUS), bnTrustedModulusV2(ZEROCOIN_MODULUS_V2);

// Set up the Zerocoin Params object
uint32_t securityLevel = 80;
libzerocoin::Params *ZCParams = new libzerocoin::Params(bnTrustedModulus, bnTrustedModulus);
libzerocoin::Params *ZCParamsV2 = new libzerocoin::Params(bnTrustedModulusV2, bnTrustedModulus);

static CZerocoinState zerocoinState;

static bool CheckZerocoinSpendSerial(CValidationState &state, const Consensus::Params &params, CZerocoinTxInfo *zerocoinTxInfo, libzerocoin::CoinDenomination denomination, const CBigNum &serial, int nHeight, bool fConnectTip) {
    if (nHeight > params.nCheckBugFixedAtBlock) {
        // check for zerocoin transaction in this block as well
        if (zerocoinTxInfo && !zerocoinTxInfo->fInfoIsComplete && zerocoinTxInfo->spentSerials.count(serial) > 0)
            return state.DoS(0, error("CTransaction::CheckTransaction() : two or more spends with same serial in the same block"));

        // check for used serials in zerocoinState
        if (zerocoinState.IsUsedCoinSerial(serial)) {
            // Proceed with checks ONLY if we're accepting tx into the memory pool or connecting block to the existing blockchain
            if (nHeight == INT_MAX || fConnectTip) {
                if (nHeight < params.nSpendV15StartBlock)
                    LogPrintf("ZCSpend: height=%d, denomination=%d, serial=%s\n", nHeight, (int)denomination, serial.ToString());
                else
                    return state.DoS(0, error("CTransaction::CheckTransaction() : The CoinSpend serial has been used"));
            }
        }
    }

    return true;
}

CBigNum ParseZerocoinMintScript(const CScript& script)
{
    if (script.size() < 6) {
        throw std::invalid_argument("Script is not a valid Zerocoin mint");
    }

    return CBigNum(std::vector<unsigned char>(script.begin() + 6, script.end()));
}

std::pair<std::unique_ptr<libzerocoin::CoinSpend>, uint32_t> ParseZerocoinSpend(const CTxIn& in)
{
    // Check arguments.
    uint32_t groupId = in.nSequence;

    if (groupId < 1 || groupId >= INT_MAX) {
        throw CBadSequence();
    }

    if (in.scriptSig.size() < 4) {
        throw CBadTxIn();
    }

    // Determine if version 2 spend.
    bool v2 = groupId >= ZC_MODULUS_V2_BASE_ID;

    // Deserialize spend.
    CDataStream serialized(
        std::vector<unsigned char>(in.scriptSig.begin() + 4, in.scriptSig.end()),
        SER_NETWORK,
        PROTOCOL_VERSION
    );

    std::unique_ptr<libzerocoin::CoinSpend> spend(new libzerocoin::CoinSpend(v2 ? ZCParamsV2 : ZCParams, serialized));

    return std::make_pair(std::move(spend), groupId);
}

bool CheckRemintZcoinTransaction(const CTransaction &tx,
                                const Consensus::Params &params,
                                CValidationState &state,
                                uint256 hashTx,
                                bool isVerifyDB,
                                int nHeight,
                                bool isCheckWallet,
                                bool fStatefulZerocoinCheck,
                                CZerocoinTxInfo *zerocoinTxInfo) {

    // Check height
    int txHeight;
    {
        LOCK(cs_main);
        txHeight = nHeight == INT_MAX ? chainActive.Height() : nHeight;
    }

    if (txHeight < params.nSigmaStartBlock || txHeight >= params.nSigmaStartBlock + params.nZerocoinToSigmaRemintWindowSize)
        // we allow transactions of remint type only during specific window
        return false;
    
    // There should only one remint input
    if (tx.vin.size() != 1 || tx.vin[0].scriptSig.size() == 0 || tx.vin[0].scriptSig[0] != OP_ZEROCOINTOSIGMAREMINT)
        return false;

    vector<unsigned char> remintSerData(tx.vin[0].scriptSig.begin()+1, tx.vin[0].scriptSig.end());
    CDataStream inStream1(remintSerData, SER_NETWORK, PROTOCOL_VERSION);
    sigma::CoinRemintToV3 remint(inStream1);

    LogPrintf("CheckRemintZcoinTransaction: nHeight=%d, denomination=%d, serial=%s\n", 
            nHeight, remint.getDenomination(), remint.getSerialNumber().GetHex().c_str());

    if (remint.getMintVersion() != ZEROCOIN_TX_VERSION_2) {
        LogPrintf("CheckRemintZcoinTransaction: only mint of version 2 is currently supported\n");
        return false;
    }

    vector<unique_ptr<sigma::PublicCoin>> sigmaMints;
    int64_t totalAmountInSigmaMints = 0;

    if (CZerocoinState::IsPublicCoinValueBlacklisted(remint.getPublicCoinValue())) {
        LogPrintf("CheckRemintZcoinTransaction: coin is blacklisted\n");
        return false;
    }

    // All the outputs should be sigma mints
    for (const CTxOut &out: tx.vout) {
        if (out.scriptPubKey.size() == 0 || out.scriptPubKey[0] != OP_SIGMAMINT)
            return false;

        sigma::CoinDenomination d;
        if (!sigma::IntegerToDenomination(out.nValue, d, state))
            return false;

        secp_primitives::GroupElement mintPublicValue = sigma::ParseSigmaMintScript(out.scriptPubKey);
        sigma::PublicCoin *mint = new sigma::PublicCoin(mintPublicValue, d);
        if (!mint->validate()) {
            LogPrintf("CheckRemintZcoinTransaction: sigma mint validation failure\n");
            return false;
        }

        sigmaMints.emplace_back(mint);
        totalAmountInSigmaMints += out.nValue;
    }

    if (remint.getDenomination()*COIN != totalAmountInSigmaMints) {
        LogPrintf("CheckRemintZcoinTransaction: incorrect amount\n");
        return false;
    }

    // Create temporary tx, clear remint signature and get its hash
    CMutableTransaction tempTx = tx;
    CDataStream inStream2(remintSerData, SER_NETWORK, PROTOCOL_VERSION);
    sigma::CoinRemintToV3 tempRemint(inStream2);
    tempRemint.ClearSignature();

    CDataStream remintWithoutSignature(SER_NETWORK, PROTOCOL_VERSION);
    remintWithoutSignature << tempRemint;

    CScript remintScriptBeforeSignature;
    remintScriptBeforeSignature << OP_ZEROCOINTOSIGMAREMINT;
    remintScriptBeforeSignature.insert(remintScriptBeforeSignature.end(), remintWithoutSignature.begin(), remintWithoutSignature.end());

    tempTx.vin[0].scriptSig = remintScriptBeforeSignature;

    libzerocoin::SpendMetaData metadata(remint.getCoinGroupId(), tempTx.GetHash());

    if (!remint.Verify(metadata)) {
        LogPrintf("CheckRemintZcoinTransaction: remint input verification failure\n");
        return false;
    }

    if (!fStatefulZerocoinCheck)
        return true;

    CZerocoinState *zerocoinState = CZerocoinState::GetZerocoinState();

    // Check if this coin is present
    int mintId = -1;
    int mintHeight = -1;
    if ((mintHeight = zerocoinState->GetMintedCoinHeightAndId(remint.getPublicCoinValue(), (int)remint.getDenomination(), mintId) <= 0) 
                || mintId != remint.getCoinGroupId()     /* inconsistent group id in remint data */
                || mintHeight >= params.nSigmaStartBlock /* additional failsafe to ensure mint height is valid */) {
        LogPrintf("CheckRemintZcoinTransaction: no such mint\n");
        return false;
    }

    CBigNum serial = remint.getSerialNumber();
    if (!CheckZerocoinSpendSerial(state, params, zerocoinTxInfo, (libzerocoin::CoinDenomination)remint.getDenomination(), serial, nHeight, false))
        return false;

    if(!isVerifyDB && !isCheckWallet) {
        if (zerocoinTxInfo && !zerocoinTxInfo->fInfoIsComplete) {
            // add spend information to the index
            zerocoinTxInfo->spentSerials[serial] = (int)remint.getDenomination();
            zerocoinTxInfo->zcTransactions.insert(hashTx);
        }
    }

    return true;
}

bool CheckSpendZcoinTransaction(const CTransaction &tx,
                                const Consensus::Params &params,
                                const vector<libzerocoin::CoinDenomination>& targetDenominations,
                                CValidationState &state,
                                uint256 hashTx,
                                bool isVerifyDB,
                                int nHeight,
                                bool isCheckWallet,
                                bool fStatefulZerocoinCheck,
                                CZerocoinTxInfo *zerocoinTxInfo) {

    int txHeight = chainActive.Height();
    bool hasZerocoinSpendInputs = false, hasNonZerocoinInputs = false;
    int vinIndex = -1;

    set<CBigNum> serialsUsedInThisTx;

    for (const CTxIn &txin : tx.vin) {
        std::unique_ptr<libzerocoin::CoinSpend> spend;
        uint32_t pubcoinId;

        vinIndex++;
        if (txin.scriptSig.IsZerocoinSpend()) {
            hasZerocoinSpendInputs = true;
        }
        else {
            hasNonZerocoinInputs = true;
        }

        try {
            std::tie(spend, pubcoinId) = ParseZerocoinSpend(txin);
        } catch (CBadSequence&) {
            return state.DoS(100,
                false,
                NSEQUENCE_INCORRECT,
                "CTransaction::CheckTransaction() : Error: zerocoin spend nSequence is incorrect");
        } catch (CBadTxIn&) {
            return state.DoS(100,
                false,
                REJECT_MALFORMED,
                "CheckSpendZcoinTransaction: invalid spend transaction");
        }

        bool fModulusV2 = pubcoinId >= ZC_MODULUS_V2_BASE_ID, fModulusV2InIndex = false;
        if (fModulusV2)
            pubcoinId -= ZC_MODULUS_V2_BASE_ID;
        libzerocoin::Params *zcParams = fModulusV2 ? ZCParamsV2 : ZCParams;

        int spendVersion = spend->getVersion();
        if (spendVersion != ZEROCOIN_TX_VERSION_1 &&
                spendVersion != ZEROCOIN_TX_VERSION_1_5 &&
                spendVersion != ZEROCOIN_TX_VERSION_2) {
            return state.DoS(100,
                false,
                NSEQUENCE_INCORRECT,
                "CTransaction::CheckTransaction() : Error: incorrect spend transaction verion");
        }

        if (IsZerocoinTxV2(targetDenominations[vinIndex], params, pubcoinId)) {
            // After threshold id all spends should be strictly 2.0
            if (spendVersion != ZEROCOIN_TX_VERSION_2)
                return state.DoS(100,
                    false,
                    NSEQUENCE_INCORRECT,
                    "CTransaction::CheckTransaction() : Error: zerocoin spend should be version 2.0");
            fModulusV2InIndex = true;
        }
        else {
            // old spends v2.0s are probably incorrect, force spend to version 1
            if (spendVersion == ZEROCOIN_TX_VERSION_2) {
                spendVersion = ZEROCOIN_TX_VERSION_1;
                spend->setVersion(ZEROCOIN_TX_VERSION_1);
            }
        }

        if (fModulusV2InIndex != fModulusV2 && fStatefulZerocoinCheck)
            zerocoinState.CalculateAlternativeModulusAccumulatorValues(&chainActive, (int)targetDenominations[vinIndex], pubcoinId);

        uint256 txHashForMetadata;

        if (spendVersion > ZEROCOIN_TX_VERSION_1) {
            // Obtain the hash of the transaction sans the zerocoin part
            CMutableTransaction txTemp = tx;
            BOOST_FOREACH(CTxIn &txTempIn, txTemp.vin) {
                if (txTempIn.scriptSig.IsZerocoinSpend()) {
                    txTempIn.scriptSig.clear();
                    txTempIn.prevout.SetNull();
                }
            }
            txHashForMetadata = txTemp.GetHash();
        }

        LogPrintf("CheckSpendZcoinTransaction: tx version=%d, tx metadata hash=%s, serial=%s\n", spend->getVersion(), txHashForMetadata.ToString(), spend->getCoinSerialNumber().ToString());

        int txHeight = chainActive.Height();

        if (spendVersion == ZEROCOIN_TX_VERSION_1 && nHeight == INT_MAX) {
            int allowedV1Height = params.nSpendV15StartBlock;
            if (txHeight >= allowedV1Height + ZC_V1_5_GRACEFUL_MEMPOOL_PERIOD) {
                LogPrintf("CheckSpendZcoinTransaction: cannot allow spend v1 into mempool after block %d\n",
                          allowedV1Height + ZC_V1_5_GRACEFUL_MEMPOOL_PERIOD);
                return false;
            }
        }

        // test if given modulus version is allowed at this point
        if (fModulusV2) {
            if ((nHeight == INT_MAX && txHeight < params.nModulusV2StartBlock) || nHeight < params.nModulusV2StartBlock)
                return state.DoS(100, false,
                                 NSEQUENCE_INCORRECT,
                                 "CheckSpendZcoinTransaction: cannon use modulus v2 at this point");
        }
        else {
            if ((nHeight == INT_MAX && txHeight >= params.nModulusV1MempoolStopBlock) ||
                    (nHeight != INT_MAX && nHeight >= params.nModulusV1StopBlock))
                return state.DoS(100, false,
                                 NSEQUENCE_INCORRECT,
                                 "CheckSpendZcoinTransaction: cannon use modulus v1 at this point");
        }

        if (!fStatefulZerocoinCheck)
            continue;

        CBigNum serial = spend->getCoinSerialNumber();
        // check if there are spends with the same serial within one block
        // do not check for duplicates in case we've seen exact copy of this tx in this block before
        if (nHeight >= params.nDontAllowDupTxsStartBlock || !(zerocoinTxInfo && zerocoinTxInfo->zcTransactions.count(hashTx) > 0)) {
            if (serialsUsedInThisTx.count(serial) > 0)
                return state.DoS(0, error("CTransaction::CheckTransaction() : two or more spends with same serial in the same block"));
            serialsUsedInThisTx.insert(serial);

            if (!CheckZerocoinSpendSerial(state, params, zerocoinTxInfo, spend->getDenomination(), serial, nHeight, false))
                return false;
        }

        if(!isVerifyDB && !isCheckWallet) {
            if (zerocoinTxInfo && !zerocoinTxInfo->fInfoIsComplete) {
                // add spend information to the index
                zerocoinTxInfo->spentSerials[serial] = (int)spend->getDenomination();
                zerocoinTxInfo->zcTransactions.insert(hashTx);

                if (spend->getVersion() == ZEROCOIN_TX_VERSION_1)
                    zerocoinTxInfo->fHasSpendV1 = true;
            }
        }

        libzerocoin::SpendMetaData newMetadata(txin.nSequence, txHashForMetadata);

        CZerocoinState::CoinGroupInfo coinGroup;
        if (!zerocoinState.GetCoinGroupInfo(targetDenominations[vinIndex], pubcoinId, coinGroup))
            return state.DoS(100, false, NO_MINT_ZEROCOIN, "CheckSpendZcoinTransaction: Error: no coins were minted with such parameters");

        bool passVerify = false;
        CBlockIndex *index = coinGroup.lastBlock;

        pair<int,int> denominationAndId = make_pair(targetDenominations[vinIndex], pubcoinId);

        bool spendHasBlockHash = false;

        // Zerocoin v1.5/v2 transaction can cointain block hash of the last mint tx seen at the moment of spend. It speeds
        // up verification
        if (spendVersion > ZEROCOIN_TX_VERSION_1 && !spend->getAccumulatorBlockHash().IsNull()) {
			spendHasBlockHash = true;
			uint256 accumulatorBlockHash = spend->getAccumulatorBlockHash();

			// find index for block with hash of accumulatorBlockHash or set index to the coinGroup.firstBlock if not found
			while (index != coinGroup.firstBlock && index->GetBlockHash() != accumulatorBlockHash)
				index = index->pprev;
		}

        decltype(&CBlockIndex::accumulatorChanges) accChanges = fModulusV2 == fModulusV2InIndex ?
                    &CBlockIndex::accumulatorChanges : &CBlockIndex::alternativeAccumulatorChanges;

        // Enumerate all the accumulator changes seen in the blockchain starting with the latest block
        // In most cases the latest accumulator value will be used for verification
        do {
            if ((index->*accChanges).count(denominationAndId) > 0) {
                libzerocoin::Accumulator accumulator(zcParams,
                                                     (index->*accChanges)[denominationAndId].first,
                                                     targetDenominations[vinIndex]);
                LogPrintf("CheckSpendZcoinTransaction: accumulator=%s\n", accumulator.getValue().ToString().substr(0,15));
                passVerify = spend->Verify(accumulator, newMetadata);
            }

            // if spend has block hash we don't need to look further
            if (index == coinGroup.firstBlock || spendHasBlockHash)
                break;
            else
                index = index->pprev;
        } while (!passVerify);

        // Rare case: accumulator value contains some but NOT ALL coins from one block. In this case we will
        // have to enumerate over coins manually. No optimization is really needed here because it's a rarity
        // This can't happen if spend is of version 1.5 or 2.0
        if (!passVerify && spendVersion == ZEROCOIN_TX_VERSION_1) {
            // Build vector of coins sorted by the time of mint
            index = coinGroup.lastBlock;
            vector<CBigNum> pubCoins = index->mintedPubCoins[denominationAndId];
            if (index != coinGroup.firstBlock) {
                do {
                    index = index->pprev;
                    if (index->mintedPubCoins.count(denominationAndId) > 0)
                        pubCoins.insert(pubCoins.begin(),
                                        index->mintedPubCoins[denominationAndId].cbegin(),
                                        index->mintedPubCoins[denominationAndId].cend());
                } while (index != coinGroup.firstBlock);
            }

            libzerocoin::Accumulator accumulator(zcParams, targetDenominations[vinIndex]);
            BOOST_FOREACH(const CBigNum &pubCoin, pubCoins) {
                accumulator += libzerocoin::PublicCoin(zcParams, pubCoin, (libzerocoin::CoinDenomination)targetDenominations[vinIndex]);
                LogPrintf("CheckSpendZcoinTransaction: accumulator=%s\n", accumulator.getValue().ToString().substr(0,15));
                if ((passVerify = spend->Verify(accumulator, newMetadata)) == true)
                    break;
            }

            if (!passVerify) {
                // One more time now in reverse direction. The only reason why it's required is compatibility with
                // previous client versions
                libzerocoin::Accumulator accumulator(zcParams, targetDenominations[vinIndex]);
                BOOST_REVERSE_FOREACH(const CBigNum &pubCoin, pubCoins) {
                    accumulator += libzerocoin::PublicCoin(zcParams, pubCoin, (libzerocoin::CoinDenomination)targetDenominations[vinIndex]);
                    LogPrintf("CheckSpendZcoinTransaction: accumulatorRev=%s\n", accumulator.getValue().ToString().substr(0,15));
                    if ((passVerify = spend->Verify(accumulator, newMetadata)) == true)
                        break;
                }
            }
        }

        if (!passVerify) {
            LogPrintf("CheckSpendZCoinTransaction: verification failed at block %d\n", nHeight);
            return false;
        }
    }

    if (hasZerocoinSpendInputs) {
        if (hasNonZerocoinInputs) {
            // mixing zerocoin spend input with non-zerocoin inputs is prohibited
            return state.DoS(100, false,
                            REJECT_MALFORMED,
                            "CheckSpendZcoinTransaction: can't mix zerocoin spend input with regular ones");
        }
        else if (tx.vin.size() > 1) {
            // having tx with several zerocoin spend inputs is possible since nMultipleSpendInputsInOneTxStartBlock
            if ((nHeight == INT_MAX && txHeight < params.nMultipleSpendInputsInOneTxStartBlock) ||
                    (nHeight < params.nMultipleSpendInputsInOneTxStartBlock)) {
                return state.DoS(100, false,
                             REJECT_MALFORMED,
                             "CheckSpendZcoinTransaction: can't have more than one input");
            }
        }
    }

    return true;
}

bool CheckMintZcoinTransaction(const CTxOut &txout,
                               CValidationState &state,
                               uint256 hashTx,
                               CZerocoinTxInfo *zerocoinTxInfo) {
    CBigNum pubCoin;

    LogPrintf("CheckMintZcoinTransaction txHash = %s\n", txout.GetHash().ToString());
    LogPrintf("nValue = %d\n", txout.nValue);

    try {
        pubCoin = ParseZerocoinMintScript(txout.scriptPubKey);
    } catch (std::invalid_argument&) {
        return state.DoS(100,
            false,
            PUBCOIN_NOT_VALIDATE,
            "CTransaction::CheckTransaction() : PubCoin validation failed");
    }

    bool hasCoin = zerocoinState.HasCoin(pubCoin);

    if (!hasCoin && zerocoinTxInfo && !zerocoinTxInfo->fInfoIsComplete) {
        BOOST_FOREACH(const PAIRTYPE(int,CBigNum) &mint, zerocoinTxInfo->mints) {
            if (mint.second == pubCoin) {
                hasCoin = true;
                break;
            }
        }
    }

    if (hasCoin) {
        /*return state.DoS(100,
                         false,
                         PUBCOIN_NOT_VALIDATE,
                         "CheckZerocoinTransaction: duplicate mint");*/
        LogPrintf("CheckMintZerocoinTransaction: double mint, tx=%s\n", txout.GetHash().ToString());
    }

    switch (txout.nValue) {
    default:
        return state.DoS(100,
            false,
            PUBCOIN_NOT_VALIDATE,
            "CheckZerocoinTransaction : PubCoin denomination is invalid");

    case libzerocoin::ZQ_LOVELACE*COIN:
    case libzerocoin::ZQ_GOLDWASSER*COIN:
    case libzerocoin::ZQ_RACKOFF*COIN:
    case libzerocoin::ZQ_PEDERSEN*COIN:
    case libzerocoin::ZQ_WILLIAMSON*COIN:
        libzerocoin::CoinDenomination denomination = (libzerocoin::CoinDenomination)(txout.nValue / COIN);
        libzerocoin::PublicCoin checkPubCoin(ZCParamsV2, pubCoin, denomination);
        if (!checkPubCoin.validate())
            return state.DoS(100,
                false,
                PUBCOIN_NOT_VALIDATE,
                "CheckZerocoinTransaction : PubCoin validation failed");

        if (zerocoinTxInfo != NULL && !zerocoinTxInfo->fInfoIsComplete) {
            // Update public coin list in the info
            zerocoinTxInfo->mints.push_back(make_pair(denomination, pubCoin));
            zerocoinTxInfo->zcTransactions.insert(hashTx);
        }

        break;
    }

    return true;
}

bool CheckZerocoinFoundersInputs(const CTransaction &tx, CValidationState &state, const Consensus::Params &params, int nHeight, bool fMTP) {
        bool fPremineBlock = nHeight == 2;
        bool found_1 = false;
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
        //Check for Blocks before start before checking for rest of blocks
        if (params.IsMain() && GetAdjustedTime() <= nStartRewardTime) {
                return state.DoS(100, false, REJECT_TRANSACTION_TOO_EARLY,
                                 "CTransaction::CheckTransaction() : transaction is too early");
        }

        if (fPremineBlock) {
            for(int i=0;i<318;i++){
                if (params.IsMain() && GetAdjustedTime() > nStartRewardTime) {
                    SNAPSHOT_PAYEE_SCRIPT = GetScriptForDestination(CBitcoinAddress(snapshotaddrs[i]).Get());
                }
                BOOST_FOREACH(const CTxOut &output, tx.vout) {
                    if (output.scriptPubKey == SNAPSHOT_PAYEE_SCRIPT && output.nValue == (int64_t)(addrbalances[i])) {
                        found_1 = true;
                    }
                }
                if (!found_1) {
                    return state.DoS(100, false, REJECT_PREMINE_REWARD_MISSING,
                                "CTransaction::CheckTransaction() : snapshot payee reward missing");
                }
            }
        }

        int total_payment_tx = 0; // no more than 1 output for payment
        if (nHeight >= params.nIndexnodePaymentsStartBlock) {
            CAmount indexnodePayment = GetIndexnodePayment(params, fMTP,nHeight);
            BOOST_FOREACH(const CTxOut &output, tx.vout) {
                if (indexnodePayment == output.nValue) {
                    total_payment_tx = total_payment_tx + 1;
                }
            }

            bool validIndexnodePayment;

            if (nHeight > params.nIndexnodePaymentsBugFixedAtBlock) {
                if (!indexnodeSync.IsSynced()) {
                    validIndexnodePayment = true;
                } else {
                    validIndexnodePayment = mnpayments.IsTransactionValid(tx, nHeight, fMTP);
                }
            } else {
                validIndexnodePayment = total_payment_tx <= 1;
            }

            if (!validIndexnodePayment) {
                return state.DoS(100, false, REJECT_INVALID_INDEXNODE_PAYMENT,
                                 "CTransaction::CheckTransaction() : invalid indexnode payment");
            }
        
    }

    return true;
}
bool CheckZerocoinTransaction(const CTransaction &tx,
                              CValidationState &state,
                              const Consensus::Params &params,
                              uint256 hashTx,
                              bool isVerifyDB,
                              int nHeight,
                              bool isCheckWallet,
                              bool fStatefulZerocoinCheck,
                              CZerocoinTxInfo *zerocoinTxInfo)
{
    if (tx.IsZerocoinSpend() || tx.IsZerocoinMint()) {
        if ((nHeight != INT_MAX && nHeight >= params.nDisableZerocoinStartBlock)    // transaction is a part of block: disable after specific block number
                    || (nHeight == INT_MAX && !params.IsRegtest() && !isVerifyDB))  // transaction is accepted to the memory pool: always disable except if regtest chain (need remint tests)
            return state.DoS(1, error("Zerocoin is disabled at this point"));
    }

    bool const isWalletCheck = (isVerifyDB && nHeight == INT_MAX);

    // nHeight have special mode which value is INT_MAX so we need this.
    int realHeight = 0;

    if(!(isWalletCheck)) {
        LOCK(cs_main);
        realHeight = chainActive.Height();
    }

    // Check Mint Zerocoin Transaction
    for (const CTxOut &txout : tx.vout) {
        if (!txout.scriptPubKey.empty() && txout.scriptPubKey.IsZerocoinMint()) {
            if (!isWalletCheck && realHeight > params.nSigmaStartBlock + params.nZerocoinV2MintGracefulPeriod)
                return state.DoS(100, false, REJECT_OBSOLETE, "bad-txns-mint-obsolete");

            if (!CheckMintZcoinTransaction(txout, state, hashTx, zerocoinTxInfo))
                return false;
        }
    }

    // Check Spend Zerocoin Transaction
    vector<libzerocoin::CoinDenomination> denominations;
    if (tx.IsZerocoinSpend()) {
        if (!isWalletCheck && realHeight > params.nSigmaStartBlock + params.nZerocoinV2SpendGracefulPeriod)
            return state.DoS(100, false, REJECT_OBSOLETE, "bad-txns-spend-obsolete");

        if (tx.vout.size() > 1) {
            // TODO: enable such spends after some block number
            return state.DoS(100, error("Zerocoin spend with more than 1 output"));
        }

        // First check number of inputs does not exceed transaction limit
        if(tx.vin.size() > ZC_SPEND_LIMIT){
            return false;
        }

        // Check for any non spend inputs and fail if so
        int64_t totalValue = 0;
        BOOST_FOREACH(const CTxIn &txin, tx.vin){
            if(!txin.scriptSig.IsZerocoinSpend()) {
                return state.DoS(100, false,
                                REJECT_MALFORMED,
                                "CheckSpendZcoinTransaction: can't mix zerocoin spend input with regular ones");
            }
            // Get the CoinDenomination value of each vin for the CheckSpendZcoinTransaction function
            uint32_t pubcoinId = txin.nSequence;
            if (pubcoinId < 1 || pubcoinId >= INT_MAX) {
                 // coin id should be positive integer
                return false;
            }
            libzerocoin::Params *zcParams = (pubcoinId >= ZC_MODULUS_V2_BASE_ID) ? ZCParamsV2 : ZCParams;

            CDataStream serializedCoinSpend((const char *)&*(txin.scriptSig.begin() + 4),
                                    (const char *)&*txin.scriptSig.end(),
                                    SER_NETWORK, PROTOCOL_VERSION);
            libzerocoin::CoinSpend newSpend(zcParams, serializedCoinSpend);
            denominations.push_back(newSpend.getDenomination());
            totalValue += newSpend.getDenomination();
        }

        // Check vOut
        // Only one loop, we checked on the format before enter this case
        BOOST_FOREACH(const CTxOut &txout, tx.vout)
        {
            if(!isVerifyDB) {
                if (txout.nValue == totalValue * COIN) {
                    if(!CheckSpendZcoinTransaction(tx, params, denominations, state, hashTx, isVerifyDB, nHeight, isCheckWallet, fStatefulZerocoinCheck, zerocoinTxInfo)){
                        return false;
                    }
                }
                else {
                    return state.DoS(100, error("CheckZerocoinTransaction : invalid spending txout value"));
                }
            }
        }
    }

    if (tx.IsZerocoinRemint())
        return CheckRemintZcoinTransaction(tx, params, state, hashTx, isVerifyDB, nHeight, isCheckWallet, fStatefulZerocoinCheck, zerocoinTxInfo);

    return true;
}

void DisconnectTipZC(CBlock & /*block*/, CBlockIndex *pindexDelete) {
    zerocoinState.RemoveBlock(pindexDelete);
}

CBigNum ZerocoinGetSpendSerialNumber(const CTransaction &tx, const CTxIn &txin) {
    if (!txin.IsZerocoinSpend())
        return CBigNum(0);
    try {
        CDataStream serializedCoinSpend((const char *)&*(txin.scriptSig.begin() + 4),
                                    (const char *)&*txin.scriptSig.end(),
                                    SER_NETWORK, PROTOCOL_VERSION);
        libzerocoin::CoinSpend spend(txin.nSequence >= ZC_MODULUS_V2_BASE_ID ? ZCParamsV2 : ZCParams, serializedCoinSpend);
        return spend.getCoinSerialNumber();
    }
    catch (const std::runtime_error &) {
        return CBigNum(0);
    }
}

/**
 * Connect a new ZCblock to chainActive. pblock is either NULL or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 */
bool ConnectBlockZC(CValidationState &state, const CChainParams &chainParams, CBlockIndex *pindexNew, const CBlock *pblock, bool fJustCheck) {

    // Add zerocoin transaction information to index
    if (pblock && pblock->zerocoinTxInfo) {
        if (pblock->zerocoinTxInfo->fHasSpendV1) {
            // Don't allow spend v1s after some point of time
            int allowV1Height = chainParams.GetConsensus().nSpendV15StartBlock;
            if (pindexNew->nHeight >= allowV1Height + ZC_V1_5_GRACEFUL_PERIOD) {
                LogPrintf("ConnectTipZC: spend v1 is not allowed after block %d\n", allowV1Height);
                return false;
            }
        }

	    if (!fJustCheck) {
            // clear the state
			pindexNew->spentSerials.clear();
            pindexNew->mintedPubCoins.clear();
            pindexNew->accumulatorChanges.clear();
            pindexNew->alternativeAccumulatorChanges.clear();
        }

        if (pindexNew->nHeight > chainParams.GetConsensus().nCheckBugFixedAtBlock) {
            BOOST_FOREACH(const PAIRTYPE(CBigNum,int) &serial, pblock->zerocoinTxInfo->spentSerials) {
                if (!CheckZerocoinSpendSerial(state, chainParams.GetConsensus(), pblock->zerocoinTxInfo.get(), (libzerocoin::CoinDenomination)serial.second, serial.first, pindexNew->nHeight, true))
                    return false;

                if (!fJustCheck) {
                    pindexNew->spentSerials.insert(serial.first);
                    zerocoinState.AddSpend(serial.first);
                }

            }
        }

        if (fJustCheck)
            return true;

        // Update minted values and accumulators
        BOOST_FOREACH(const PAIRTYPE(int,CBigNum) &mint, pblock->zerocoinTxInfo->mints) {
            CBigNum oldAccValue(0);
            int denomination = mint.first;
            int mintId = zerocoinState.AddMint(pindexNew, denomination, mint.second, oldAccValue);

            libzerocoin::Params *zcParams = IsZerocoinTxV2((libzerocoin::CoinDenomination)denomination,
                                                chainParams.GetConsensus(), mintId) ? ZCParamsV2 : ZCParams;

            if (!oldAccValue)
                oldAccValue = zcParams->accumulatorParams.accumulatorBase;

            LogPrintf("ConnectTipZC: mint added denomination=%d, id=%d\n", denomination, mintId);
            pair<int,int> denomAndId = make_pair(denomination, mintId);

            pindexNew->mintedPubCoins[denomAndId].push_back(mint.second);

            CZerocoinState::CoinGroupInfo coinGroupInfo;
            zerocoinState.GetCoinGroupInfo(denomination, mintId, coinGroupInfo);

            libzerocoin::PublicCoin pubCoin(zcParams, mint.second, (libzerocoin::CoinDenomination)denomination);
            libzerocoin::Accumulator accumulator(zcParams,
                                                 oldAccValue,
                                                 (libzerocoin::CoinDenomination)denomination);
            accumulator += pubCoin;

            if (pindexNew->accumulatorChanges.count(denomAndId) > 0) {
                pair<CBigNum,int> &accChange = pindexNew->accumulatorChanges[denomAndId];
                accChange.first = accumulator.getValue();
                accChange.second++;
            }
            else {
                pindexNew->accumulatorChanges[denomAndId] = make_pair(accumulator.getValue(), 1);
            }
            // invalidate alternative accumulator value for this denomination and id
            pindexNew->alternativeAccumulatorChanges.erase(denomAndId);
        }
    }
    else if (!fJustCheck) {
        zerocoinState.AddBlock(pindexNew, chainParams.GetConsensus());
    }

    return true;
}

int ZerocoinGetNHeight(const CBlockHeader &block) {
    CBlockIndex *pindexPrev = NULL;
    int nHeight = 0;
    BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
    if (mi != mapBlockIndex.end()) {
        pindexPrev = (*mi).second;
        nHeight = pindexPrev->nHeight + 1;
    }
    return nHeight;
}


bool ZerocoinBuildStateFromIndex(CChain *chain, set<CBlockIndex *> &changes) {
    auto params = Params().GetConsensus();

    zerocoinState.Reset();
    for (CBlockIndex *blockIndex = chain->Genesis(); blockIndex; blockIndex=chain->Next(blockIndex))
        zerocoinState.AddBlock(blockIndex, params);

    changes = zerocoinState.RecalculateAccumulators(chain);

    // DEBUG
    LogPrintf("Latest IDs are %d, %d, %d, %d, %d\n",
              zerocoinState.latestCoinIds[1],
               zerocoinState.latestCoinIds[10],
            zerocoinState.latestCoinIds[25],
            zerocoinState.latestCoinIds[50],
            zerocoinState.latestCoinIds[100]);
    return true;
}

// CZerocoinTxInfo

void CZerocoinTxInfo::Complete() {
    // We need to sort mints lexicographically by serialized value of pubCoin. That's the way old code
    // works, we need to stick to it. Denomination doesn't matter but we will sort by it as well
    sort(mints.begin(), mints.end(),
         [](decltype(mints)::const_reference m1, decltype(mints)::const_reference m2)->bool {
            CDataStream ds1(SER_DISK, CLIENT_VERSION), ds2(SER_DISK, CLIENT_VERSION);
            ds1 << m1.second;
            ds2 << m2.second;
            return (m1.first < m2.first) || ((m1.first == m2.first) && (ds1.str() < ds2.str()));
         });

    // Mark this info as complete
    fInfoIsComplete = true;
}

// CZerocoinState::CBigNumHash

std::size_t CZerocoinState::CBigNumHash::operator ()(const CBigNum &bn) const noexcept {
    // we are operating on almost random big numbers and least significant bytes (save for few last bytes) give us a good hash
    vector<unsigned char> bnData = bn.ToBytes();
    if (bnData.size() < sizeof(size_t)*3)
        // rare case, put ones like that into one hash bin
        return 0;
    else
        return ((size_t*)bnData.data())[1];
}

// CZerocoinState

CZerocoinState::CZerocoinState() {
}

int CZerocoinState::AddMint(CBlockIndex *index, int denomination, const CBigNum &pubCoin, CBigNum &previousAccValue) {

    int mintId = 1;

    if (latestCoinIds[denomination] < 1)
        latestCoinIds[denomination] = 1;
    else
        mintId = latestCoinIds[denomination];

    // There is a limit of 10 coins per group but mints belonging to the same block must have the same id thus going
    // beyond 10
    CoinGroupInfo &coinGroup = coinGroups[make_pair(denomination, mintId)];
    int coinsPerId = IsZerocoinTxV2((libzerocoin::CoinDenomination)denomination,
                        Params().GetConsensus(), mintId) ? ZC_SPEND_V2_COINSPERID : ZC_SPEND_V1_COINSPERID;
    if (coinGroup.nCoins < coinsPerId || coinGroup.lastBlock == index) {
        if (coinGroup.nCoins++ == 0) {
            // first group of coins for given denomination
            coinGroup.firstBlock = coinGroup.lastBlock = index;
        }
        else {
            previousAccValue = coinGroup.lastBlock->accumulatorChanges[make_pair(denomination,mintId)].first;
            coinGroup.lastBlock = index;
        }
    }
    else {
        latestCoinIds[denomination] = ++mintId;
        CoinGroupInfo &newCoinGroup = coinGroups[make_pair(denomination, mintId)];
        newCoinGroup.firstBlock = newCoinGroup.lastBlock = index;
        newCoinGroup.nCoins = 1;
    }

    CMintedCoinInfo coinInfo;
    coinInfo.denomination = denomination;
    coinInfo.id = mintId;
    coinInfo.nHeight = index->nHeight;
    mintedPubCoins.insert(pair<CBigNum,CMintedCoinInfo>(pubCoin, coinInfo));

    return mintId;
}

void CZerocoinState::AddSpend(const CBigNum &serial) {
    usedCoinSerials.insert(serial);
}

void CZerocoinState::AddBlock(CBlockIndex *index, const Consensus::Params &params) {
    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int), PAIRTYPE(CBigNum,int)) &accUpdate, index->accumulatorChanges)
    {
        CoinGroupInfo   &coinGroup = coinGroups[accUpdate.first];

        if (coinGroup.firstBlock == NULL)
            coinGroup.firstBlock = index;
        coinGroup.lastBlock = index;
        coinGroup.nCoins += accUpdate.second.second;
    }

    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int),vector<CBigNum>) &pubCoins, index->mintedPubCoins) {
        latestCoinIds[pubCoins.first.first] = pubCoins.first.second;
        BOOST_FOREACH(const CBigNum &coin, pubCoins.second) {
            CMintedCoinInfo coinInfo;
            coinInfo.denomination = pubCoins.first.first;
            coinInfo.id = pubCoins.first.second;
            coinInfo.nHeight = index->nHeight;
            mintedPubCoins.insert(pair<CBigNum,CMintedCoinInfo>(coin, coinInfo));
        }
    }

    if (index->nHeight > params.nCheckBugFixedAtBlock) {
        BOOST_FOREACH(const CBigNum &serial, index->spentSerials) {
            usedCoinSerials.insert(serial);
        }
    }
}

void CZerocoinState::RemoveBlock(CBlockIndex *index) {
    // roll back accumulator updates
    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int), PAIRTYPE(CBigNum,int)) &accUpdate, index->accumulatorChanges)
    {
        CoinGroupInfo   &coinGroup = coinGroups[accUpdate.first];
        int  nMintsToForget = accUpdate.second.second;

        assert(coinGroup.nCoins >= nMintsToForget);

        if ((coinGroup.nCoins -= nMintsToForget) == 0) {
            // all the coins of this group have been erased, remove the group altogether
            coinGroups.erase(accUpdate.first);
            // decrease pubcoin id for this denomination
            latestCoinIds[accUpdate.first.first]--;
        }
        else {
            // roll back lastBlock to previous position
            do {
                assert(coinGroup.lastBlock != coinGroup.firstBlock);
                coinGroup.lastBlock = coinGroup.lastBlock->pprev;
            } while (coinGroup.lastBlock->accumulatorChanges.count(accUpdate.first) == 0);
        }
    }

    // roll back mints
    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int),vector<CBigNum>) &pubCoins, index->mintedPubCoins) {
        BOOST_FOREACH(const CBigNum &coin, pubCoins.second) {
            auto coins = mintedPubCoins.equal_range(coin);
            auto coinIt = find_if(coins.first, coins.second, [=](const decltype(mintedPubCoins)::value_type &v) {
                return v.second.denomination == pubCoins.first.first &&
                        v.second.id == pubCoins.first.second;
            });
            assert(coinIt != coins.second);
            mintedPubCoins.erase(coinIt);
        }
    }

    // roll back spends
    BOOST_FOREACH(const CBigNum &serial, index->spentSerials) {
        usedCoinSerials.erase(serial);
    }
}

bool CZerocoinState::GetCoinGroupInfo(int denomination, int id, CoinGroupInfo &result) {
    pair<int,int>   key = make_pair(denomination, id);
    if (coinGroups.count(key) == 0)
        return false;

    result = coinGroups[key];
    return true;
}

bool CZerocoinState::IsUsedCoinSerial(const CBigNum &coinSerial) {
    return usedCoinSerials.count(coinSerial) != 0;
}

bool CZerocoinState::HasCoin(const CBigNum &pubCoin) {
    return mintedPubCoins.count(pubCoin) != 0;
}

int CZerocoinState::GetAccumulatorValueForSpend(CChain *chain, int maxHeight, int denomination, int id,
                                                CBigNum &accumulator, uint256 &blockHash, bool useModulusV2) {

    pair<int, int> denomAndId = pair<int, int>(denomination, id);

    if (coinGroups.count(denomAndId) == 0)
        return 0;

    CoinGroupInfo coinGroup = coinGroups[denomAndId];
    CBlockIndex *lastBlock = coinGroup.lastBlock;

    assert(lastBlock->accumulatorChanges.count(denomAndId) > 0);
    assert(coinGroup.firstBlock->accumulatorChanges.count(denomAndId) > 0);

    // is native modulus for denomination and id v2?
    bool nativeModulusIsV2 = IsZerocoinTxV2((libzerocoin::CoinDenomination)denomination, Params().GetConsensus(), id);
    // field in the block index structure for accesing accumulator changes
    decltype(&CBlockIndex::accumulatorChanges) accChangeField;
    if (nativeModulusIsV2 != useModulusV2) {
        CalculateAlternativeModulusAccumulatorValues(chain, denomination, id);
        accChangeField = &CBlockIndex::alternativeAccumulatorChanges;
    }
    else {
        accChangeField = &CBlockIndex::accumulatorChanges;
    }

    int numberOfCoins = 0;
    for (;;) {
        map<pair<int,int>, pair<CBigNum,int>> &accumulatorChanges = lastBlock->*accChangeField;
        if (accumulatorChanges.count(denomAndId) > 0) {
            if (lastBlock->nHeight <= maxHeight) {
                if (numberOfCoins == 0) {
                    // latest block satisfying given conditions
                    // remember accumulator value and block hash
                    accumulator = accumulatorChanges[denomAndId].first;
                    blockHash = lastBlock->GetBlockHash();
                }
                numberOfCoins += accumulatorChanges[denomAndId].second;
            }
        }

        if (lastBlock == coinGroup.firstBlock)
            break;
        else
            lastBlock = lastBlock->pprev;
    }

    return numberOfCoins;
}

libzerocoin::AccumulatorWitness CZerocoinState::GetWitnessForSpend(CChain *chain, int maxHeight, int denomination,
                                                                   int id, const CBigNum &pubCoin, bool useModulusV2) {

    libzerocoin::CoinDenomination d = (libzerocoin::CoinDenomination)denomination;
    pair<int, int> denomAndId = pair<int, int>(denomination, id);

    assert(coinGroups.count(denomAndId) > 0);

    CoinGroupInfo coinGroup = coinGroups[denomAndId];

    int coinId;
    int mintHeight = GetMintedCoinHeightAndId(pubCoin, denomination, coinId);

    assert(coinId == id);

    libzerocoin::Params *zcParams = useModulusV2 ? ZCParamsV2 : ZCParams;
    bool nativeModulusIsV2 = IsZerocoinTxV2((libzerocoin::CoinDenomination)denomination, Params().GetConsensus(), id);
    decltype(&CBlockIndex::accumulatorChanges) accChangeField;
    if (nativeModulusIsV2 != useModulusV2) {
        CalculateAlternativeModulusAccumulatorValues(chain, denomination, id);
        accChangeField = &CBlockIndex::alternativeAccumulatorChanges;
    }
    else {
        accChangeField = &CBlockIndex::accumulatorChanges;
    }

    // Find accumulator value preceding mint operation
    CBlockIndex *mintBlock = (*chain)[mintHeight];
    CBlockIndex *block = mintBlock;
    libzerocoin::Accumulator accumulator(zcParams, d);
    if (block != coinGroup.firstBlock) {
        do {
            block = block->pprev;
        } while ((block->*accChangeField).count(denomAndId) == 0);
        accumulator = libzerocoin::Accumulator(zcParams, (block->*accChangeField)[denomAndId].first, d);
    }

    // Now add to the accumulator every coin minted since that moment except pubCoin
    block = coinGroup.lastBlock;
    for (;;) {
        if (block->nHeight <= maxHeight && block->mintedPubCoins.count(denomAndId) > 0) {
            vector<CBigNum> &pubCoins = block->mintedPubCoins[denomAndId];
            for (const CBigNum &coin: pubCoins) {
                if (block != mintBlock || coin != pubCoin)
                    accumulator += libzerocoin::PublicCoin(zcParams, coin, d);
            }
        }
        if (block != mintBlock)
            block = block->pprev;
        else
            break;
    }

    return libzerocoin::AccumulatorWitness(zcParams, accumulator, libzerocoin::PublicCoin(zcParams, pubCoin, d));
}

int CZerocoinState::GetMintedCoinHeightAndId(const CBigNum &pubCoin, int denomination, int &id) {
    auto coins = mintedPubCoins.equal_range(pubCoin);
    auto coinIt = find_if(coins.first, coins.second,
                          [=](const decltype(mintedPubCoins)::value_type &v) { return v.second.denomination == denomination; });

    if (coinIt != coins.second) {
        id = coinIt->second.id;
        return coinIt->second.nHeight;
    }
    else
        return -1;
}

void CZerocoinState::CalculateAlternativeModulusAccumulatorValues(CChain *chain, int denomination, int id) {
    libzerocoin::CoinDenomination d = (libzerocoin::CoinDenomination)denomination;
    pair<int, int> denomAndId = pair<int, int>(denomination, id);
    libzerocoin::Params *altParams = IsZerocoinTxV2(d, Params().GetConsensus(), id) ? ZCParams : ZCParamsV2;
    libzerocoin::Accumulator accumulator(altParams, d);

    if (coinGroups.count(denomAndId) == 0) {
        // Can happen when verification is done prior to syncing with network
        return;
    }

    CoinGroupInfo coinGroup = coinGroups[denomAndId];

    CBlockIndex *block = coinGroup.firstBlock;
    for (;;) {
        if (block->accumulatorChanges.count(denomAndId) > 0) {
            if (block->alternativeAccumulatorChanges.count(denomAndId) > 0)
                // already calculated, update accumulator with cached value
                accumulator = libzerocoin::Accumulator(altParams, block->alternativeAccumulatorChanges[denomAndId].first, d);
            else {
                // re-create accumulator changes with alternative params
                assert(block->mintedPubCoins.count(denomAndId) > 0);
                const vector<CBigNum> &mintedCoins = block->mintedPubCoins[denomAndId];
                BOOST_FOREACH(const CBigNum &c, mintedCoins) {
                    accumulator += libzerocoin::PublicCoin(altParams, c, d);
                }
                block->alternativeAccumulatorChanges[denomAndId] = make_pair(accumulator.getValue(), (int)mintedCoins.size());
            }
        }

        if (block != coinGroup.lastBlock)
            block = (*chain)[block->nHeight+1];
        else
            break;
    }
}

bool CZerocoinState::TestValidity(CChain *chain) {
    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int), CoinGroupInfo) &coinGroup, coinGroups) {
        fprintf(stderr, "TestValidity[denomination=%d, id=%d]\n", coinGroup.first.first, coinGroup.first.second);

        bool fModulusV2 = IsZerocoinTxV2((libzerocoin::CoinDenomination)coinGroup.first.first, Params().GetConsensus(), coinGroup.first.second);
        libzerocoin::Params *zcParams = fModulusV2 ? ZCParamsV2 : ZCParams;

        libzerocoin::Accumulator acc(&zcParams->accumulatorParams, (libzerocoin::CoinDenomination)coinGroup.first.first);

        CBlockIndex *block = coinGroup.second.firstBlock;
        for (;;) {
            if (block->accumulatorChanges.count(coinGroup.first) > 0) {
                if (block->mintedPubCoins.count(coinGroup.first) == 0) {
                    fprintf(stderr, "  no minted coins\n");
                    return false;
                }

                BOOST_FOREACH(const CBigNum &pubCoin, block->mintedPubCoins[coinGroup.first]) {
                    acc += libzerocoin::PublicCoin(zcParams, pubCoin, (libzerocoin::CoinDenomination)coinGroup.first.first);
                }

                if (acc.getValue() != block->accumulatorChanges[coinGroup.first].first) {
                    fprintf (stderr, "  accumulator value mismatch at height %d\n", block->nHeight);
                    return false;
                }

                if (block->accumulatorChanges[coinGroup.first].second != (int)block->mintedPubCoins[coinGroup.first].size()) {
                    fprintf(stderr, "  number of minted coins mismatch at height %d\n", block->nHeight);
                    return false;
                }
            }

            if (block != coinGroup.second.lastBlock)
                block = (*chain)[block->nHeight+1];
            else
                break;
        }

        fprintf(stderr, "  verified ok\n");
    }

    return true;
}

set<CBlockIndex *> CZerocoinState::RecalculateAccumulators(CChain *chain) {
    set<CBlockIndex *> changes;

    BOOST_FOREACH(const PAIRTYPE(PAIRTYPE(int,int), CoinGroupInfo) &coinGroup, coinGroups) {
        // Skip non-modulusv2 groups
        if (!IsZerocoinTxV2((libzerocoin::CoinDenomination)coinGroup.first.first, Params().GetConsensus(), coinGroup.first.second))
            continue;

        libzerocoin::Accumulator acc(&ZCParamsV2->accumulatorParams, (libzerocoin::CoinDenomination)coinGroup.first.first);

        // Try to calculate accumulator for the first batch of mints. If it doesn't match we need to recalculate the rest of it
        CBlockIndex *block = coinGroup.second.firstBlock;
        for (;;) {
            if (block->accumulatorChanges.count(coinGroup.first) > 0) {
                BOOST_FOREACH(const CBigNum &pubCoin, block->mintedPubCoins[coinGroup.first]) {
                    acc += libzerocoin::PublicCoin(ZCParamsV2, pubCoin, (libzerocoin::CoinDenomination)coinGroup.first.first);
                }

                // First block case is special: do the check
                if (block == coinGroup.second.firstBlock) {
                    if (acc.getValue() != block->accumulatorChanges[coinGroup.first].first)
                        // recalculation is needed
                        LogPrintf("ZerocoinState: accumulator recalculation for denomination=%d, id=%d\n", coinGroup.first.first, coinGroup.first.second);
                    else
                        // everything's ok
                        break;
                }

                block->accumulatorChanges[coinGroup.first] = make_pair(acc.getValue(), (int)block->mintedPubCoins[coinGroup.first].size());
                changes.insert(block);
            }

            if (block != coinGroup.second.lastBlock)
                block = (*chain)[block->nHeight+1];
            else
                break;
        }
    }

    return changes;
}

bool CZerocoinState::AddSpendToMempool(const vector<CBigNum> &coinSerials, uint256 txHash) {
    BOOST_FOREACH(CBigNum coinSerial, coinSerials){
        if (IsUsedCoinSerial(coinSerial) || mempoolCoinSerials.count(coinSerial))
            return false;

        mempoolCoinSerials[coinSerial] = txHash;
    }

    return true;
}

bool CZerocoinState::AddSpendToMempool(const CBigNum &coinSerial, uint256 txHash) {
    if (IsUsedCoinSerial(coinSerial) || mempoolCoinSerials.count(coinSerial))
        return false;

    mempoolCoinSerials[coinSerial] = txHash;
    return true;
}

void CZerocoinState::RemoveSpendFromMempool(const CBigNum &coinSerial) {
    mempoolCoinSerials.erase(coinSerial);
}

uint256 CZerocoinState::GetMempoolConflictingTxHash(const CBigNum &coinSerial) {
    if (mempoolCoinSerials.count(coinSerial) == 0)
        return uint256();

    return mempoolCoinSerials[coinSerial];
}

bool CZerocoinState::CanAddSpendToMempool(const CBigNum &coinSerial) {
    return !IsUsedCoinSerial(coinSerial) && mempoolCoinSerials.count(coinSerial) == 0;
}

extern const char *sigmaRemintBlacklist[];
std::unordered_set<CBigNum,CZerocoinState::CBigNumHash> CZerocoinState::sigmaRemintBlacklistSet;

bool CZerocoinState::IsPublicCoinValueBlacklisted(const CBigNum &value) {
    static bool blackListLoaded = false;

    // Check against black list
    if (!blackListLoaded) {
        AssertLockHeld(cs_main);
        // Initial build of the black list. Thread-safe as we are protected by cs_main
        for (const char **blEntry = sigmaRemintBlacklist; *blEntry; blEntry++) {
            CBigNum bn;
            bn.SetHex(*blEntry);
            sigmaRemintBlacklistSet.insert(bn);
        }
    }

    return sigmaRemintBlacklistSet.count(value) > 0;
}

void CZerocoinState::BlacklistPublicCoinValue(const CBigNum &value) {
    sigmaRemintBlacklistSet.insert(value);
}

void CZerocoinState::Reset() {
    coinGroups.clear();
    usedCoinSerials.clear();
    mintedPubCoins.clear();
    latestCoinIds.clear();
    mempoolCoinSerials.clear();
}

CZerocoinState *CZerocoinState::GetZerocoinState() {
    return &zerocoinState;
}
