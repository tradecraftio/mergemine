// Copyright (c) 2020-2023 The Freicoin Developers
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <sharechain.h>

#include <consensus/merkle.h> // for ComputeMerkleMapRootFromBranch, ComputeMerkleRootFromBranch
#include <hash.h> // for CHash256, CHashWriter
#include <primitives/block.h> // for CBlockHeader
#include <streams.h> // for CDataStream
#include <span.h> // for Span, MakeUCharSpan
#include <util/check.h> // for Assert

#include <array> // for std::array
#include <utility> // for std::swap

void SetupShareChainParamsOptions(ArgsManager& argsman) {
    argsman.AddArg("-sharechain=<name>", "Use the share chain <name> (default: main). Allowed values: solo or main", ArgsManager::ALLOW_ANY, OptionsCategory::STRATUM);
}

struct SoloShareChainParams : public ShareChainParams {
    SoloShareChainParams(const ArgsManager& args) {
        is_valid = false;
        network_name = SOLO;
    }
};

struct MainShareChainParams : public ShareChainParams {
    MainShareChainParams(const ArgsManager& args) {
        is_valid = true;
        network_name = MAIN;
    }
};

std::unique_ptr<const ShareChainParams> g_share_chain_params;

void SelectShareParams(const ArgsManager& args, const std::string& chain) {
    if (chain == ShareChainParams::SOLO) {
        g_share_chain_params = std::unique_ptr<const ShareChainParams>(new SoloShareChainParams(args));
    } else if (chain == ShareChainParams::MAIN) {
        g_share_chain_params = std::unique_ptr<const ShareChainParams>(new MainShareChainParams(args));
    } else {
        throw std::runtime_error(strprintf("%s: Unknown share chain %s.", __func__, chain));
    }
}

const ShareChainParams& ShareParams() {
    Assert(g_share_chain_params);
    return *g_share_chain_params;
}

void swap(ShareWitness& lhs, ShareWitness& rhs) {
    using std::swap; // for ADL
    swap(lhs.commit, rhs.commit);
    swap(lhs.cb1, rhs.cb1);
    swap(lhs.nLockTime, rhs.nLockTime);
    swap(lhs.branch, rhs.branch);
    swap(lhs.nVersion, rhs.nVersion);
    swap(lhs.hashPrevBlock, rhs.hashPrevBlock);
    swap(lhs.share_chain_path, rhs.share_chain_path);
    swap(lhs.nTime, rhs.nTime);
    swap(lhs.nBits, rhs.nBits);
    swap(lhs.nNonce, rhs.nNonce);
}

void swap(Share& lhs, Share& rhs) {
    using std::swap; // for ADL
    swap(lhs.version, rhs.version);
    swap(lhs.bits, rhs.bits);
    swap(lhs.height, rhs.height);
    swap(lhs.total_work, rhs.total_work);
    swap(lhs.prev_shares, rhs.prev_shares);
    swap(lhs.miner, rhs.miner);
    swap(lhs.wit, rhs.wit);
}

CBlockHeader Share::GetBlockHeader(bool *mutated) const {
    if (mutated) {
        *mutated = false;
    }

    // Compute the hash of the share header.
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << version;
    ss << bits;
    ss << height;
    ss << total_work;
    // When being hashed, we include only the root hash of the Merkle
    // mountain range structure, which has the advantage of making the
    // share header a fixed sized structure.
    ss << prev_shares.GetHash();
    ss << VARINT(miner.version);
    ss << VARINT(miner.length);
    ss << Span(miner.program, miner.length);
    uint256 hash = ss.GetHash();

    // Compute the commitment root hash.
    // The share chain commitment might be stored alongside other
    // commitments in the form of a Merkle hash map.  We therefore use
    // the branch proof to work our way up to the root value of the
    // Merkle hash map.
    bool invalid = false;
    hash = ComputeMerkleMapRootFromBranch(hash, wit.commit, wit.share_chain_path, &invalid);
    if (invalid && mutated) {
        *mutated = true;
    }

    // Calculate hash of coinbase transaction.
    CHash256 h;
    // Write the first part of the coinbase transaction.
    h.Write(MakeUCharSpan(wit.cb1));
    // Write the commitment root hash.
    h.Write(MakeUCharSpan(hash));
    // Write the commitment identifier.
    static const std::array<unsigned char, 4> id
        = { 0x4b, 0x4a, 0x49, 0x48 };
    h.Write(MakeUCharSpan(id));
    // Write the rest of the coinbase transaction.
    {
        CDataStream ds(SER_GETHASH, PROTOCOL_VERSION);
        ds << wit.nLockTime;
        h.Write(MakeUCharSpan(ds));
    }
    hash = ss.GetHash();

    // Calculate hashMerkleRoot for the block header.
    hash = ComputeMerkleRootFromBranch(hash, wit.branch, 0);

    // Write the block header fields.
    CBlockHeader blkhdr;
    blkhdr.nVersion = wit.nVersion;
    blkhdr.hashPrevBlock = wit.hashPrevBlock;
    blkhdr.hashMerkleRoot = hash;
    blkhdr.nTime = wit.nTime;
    blkhdr.nBits = wit.nBits;
    blkhdr.nNonce = wit.nNonce;
    return blkhdr;
}

// End of File