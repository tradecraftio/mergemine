// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_REGISTER_H
#define BITCOIN_RPC_REGISTER_H

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

/** These are in one header file to avoid creating tons of single-function
 * headers for everything under src/rpc/ */
class CRPCTable;

void RegisterBlockchainRPCCommands(CRPCTable &tableRPC);
void RegisterFeeRPCCommands(CRPCTable&);
void RegisterMempoolRPCCommands(CRPCTable&);
void RegisterMerkleProofRPCCommands(CRPCTable&);
void RegisterMiningRPCCommands(CRPCTable &tableRPC);
void RegisterNodeRPCCommands(CRPCTable&);
void RegisterNetRPCCommands(CRPCTable&);
void RegisterOutputScriptRPCCommands(CRPCTable&);
void RegisterMergeMineRPCCommands(CRPCTable&);
void RegisterRawTransactionRPCCommands(CRPCTable &tableRPC);
void RegisterSignMessageRPCCommands(CRPCTable&);
void RegisterSignerRPCCommands(CRPCTable &tableRPC);
void RegisterStratumRPCCommands(CRPCTable&);
void RegisterTxoutProofRPCCommands(CRPCTable&);

static inline void RegisterAllCoreRPCCommands(CRPCTable &t)
{
    RegisterBlockchainRPCCommands(t);
    RegisterFeeRPCCommands(t);
    RegisterMempoolRPCCommands(t);
    RegisterMerkleProofRPCCommands(t);
    RegisterMiningRPCCommands(t);
    RegisterNodeRPCCommands(t);
    RegisterNetRPCCommands(t);
    RegisterOutputScriptRPCCommands(t);
    RegisterMergeMineRPCCommands(t);
    RegisterRawTransactionRPCCommands(t);
    RegisterSignMessageRPCCommands(t);
#ifdef ENABLE_EXTERNAL_SIGNER
    RegisterSignerRPCCommands(t);
#endif // ENABLE_EXTERNAL_SIGNER
    RegisterStratumRPCCommands(t);
    RegisterTxoutProofRPCCommands(t);
}

#endif // BITCOIN_RPC_REGISTER_H
