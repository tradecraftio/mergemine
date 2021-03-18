// Copyright (c) 2020-2021 The Freicoin Developers
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "stratum.h"

#include "base58.h"
#include "chainparams.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "crypto/sha256.h"
#include "httpserver.h"
#include "main.h"
#include "mergemine.h"
#include "miner.h"
#include "netbase.h"
#include "net.h"
#include "rpc/server.h"
#include "serialize.h"
#include "streams.h"
#include "sync.h"
#include "txmempool.h"
#include "uint256.h"
#include "util.h"
#include "utilstrencodings.h"

#include <univalue.h>

#include <algorithm> // for std::reverse
#include <string>
#include <vector>

// for argument-dependent lookup
using std::swap;

#include <boost/algorithm/string.hpp> // for boost::trim
#include <boost/lexical_cast.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include <errno.h>
#ifdef WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

struct StratumClient
{
    evconnlistener* m_listener;
    evutil_socket_t m_socket;
    bufferevent* m_bev;
    CService m_from;
    int m_nextid;
    uint256 m_secret;

    CService GetPeer() const
      { return m_from; }

    std::string m_client;

    bool m_authorized;
    CBitcoinAddress m_addr;
    std::map<uint256, std::pair<std::string, std::string> > m_mmauth;
    std::map<uint256, std::pair<uint64_t, std::map<uint256, AuxWork> > > m_mmwork;
    double m_mindiff;

    uint32_t m_version_rolling_mask;

    CBlockIndex* m_last_tip;
    boost::optional<std::pair<uint256, uint256> > m_last_second_stage;
    bool m_send_work;

    bool m_supports_extranonce;

    StratumClient() : m_listener(0), m_socket(0), m_bev(0), m_nextid(0), m_authorized(false), m_mindiff(0.0), m_version_rolling_mask(0x00000000), m_last_tip(0), m_send_work(false), m_supports_extranonce(false) { GenSecret(); }
    StratumClient(evconnlistener* listener, evutil_socket_t socket, bufferevent* bev, CService from) : m_listener(listener), m_socket(socket), m_bev(bev), m_nextid(0), m_from(from), m_authorized(false), m_mindiff(0.0), m_version_rolling_mask(0x00000000), m_last_tip(0), m_send_work(false), m_supports_extranonce(false) { GenSecret(); }

    void GenSecret();
    std::vector<unsigned char> ExtraNonce1(uint256 job_id) const;
};

void StratumClient::GenSecret()
{
    GetRandBytes(m_secret.begin(), 32);
}

std::vector<unsigned char> StratumClient::ExtraNonce1(uint256 job_id) const
{
    CSHA256 nonce_hasher;
    nonce_hasher.Write(m_secret.begin(), 32);
    if (m_supports_extranonce) {
        nonce_hasher.Write(job_id.begin(), 32);
    }
    uint256 job_nonce;
    nonce_hasher.Finalize(job_nonce.begin());
    return {job_nonce.begin(), job_nonce.begin()+8};
}

struct StratumWork {
    const CBlockIndex *m_prev_block_index;
    CBlockTemplate m_block_template;
    std::vector<uint256> m_cb_branch;
    bool m_is_witness_enabled;
    // The height is serialized in the coinbase string.  At the time the work is
    // customized, we have no need to keep the block chain context (pindexPrev),
    // so we store just the height value which is all we need.
    int m_height;

    StratumWork() : m_prev_block_index(0), m_is_witness_enabled(false), m_height(0) { };
    StratumWork(const CBlockIndex* prev_blocK_index, int height, const CBlockTemplate& block_template);

    CBlock& GetBlock()
      { return m_block_template.block; }
    const CBlock& GetBlock() const
      { return m_block_template.block; }
};

StratumWork::StratumWork(const CBlockIndex* prev_block_index, int height, const CBlockTemplate& block_template)
    : m_prev_block_index(prev_block_index)
    , m_block_template(block_template)
    , m_height(height)
{
    m_is_witness_enabled = IsWitnessEnabled(prev_block_index, Params().GetConsensus());
    if (!m_is_witness_enabled) {
        m_cb_branch = BlockMerkleBranch(m_block_template.block, 0);
    }
};

void UpdateSegwitCommitment(const StratumWork& current_work, CMutableTransaction& cb, CMutableTransaction& bf, std::vector<uint256>& cb_branch)
{
    CBlock block2(current_work.GetBlock());
    block2.vtx.back() = CTransaction(bf);
    block2.vtx[0] = CTransaction(cb);
    // Erase any existing commitments:
    int commitpos = -1;
    while ((commitpos = GetWitnessCommitmentIndex(block2)) != -1) {
        CMutableTransaction mtx(block2.vtx[0]);
        mtx.vout.erase(mtx.vout.begin()+commitpos);
        block2.vtx[0] = CTransaction(mtx);
    }
    // Generate new commitment:
    GenerateCoinbaseCommitment(block2, current_work.m_prev_block_index, Params().GetConsensus());
    // Save results from temporary block structure:
    cb = block2.vtx.front();
    bf = block2.vtx.back();
    cb_branch = BlockMerkleBranch(block2, 0);
}

//! Critical seciton guarding access to any of the stratum global state
static CCriticalSection cs_stratum;

//! List of subnets to allow stratum connections from
static std::vector<CSubNet> stratum_allow_subnets;

//! Bound stratum listening sockets
static std::map<evconnlistener*, CService> bound_listeners;

//! Active miners connected to us
static std::map<bufferevent*, StratumClient> subscriptions;

//! Mapping of stratum method names -> handlers
static std::map<std::string, boost::function<UniValue(StratumClient&, const UniValue&)> > stratum_method_dispatch;

//! A mapping of job_id -> work templates
static std::map<uint256, StratumWork> work_templates;

//! A mapping of job_id -> second stage work
static std::map<std::string, std::pair<uint256, SecondStageWork> > second_stages;

//! A thread to watch for new blocks and send mining notifications
static boost::thread block_watcher_thread;

std::string HexInt4(uint32_t val)
{
    std::vector<unsigned char> vch;
    vch.push_back((val >> 24) & 0xff);
    vch.push_back((val >> 16) & 0xff);
    vch.push_back((val >>  8) & 0xff);
    vch.push_back( val        & 0xff);
    return HexStr(vch);
}

uint32_t ParseHexInt4(const UniValue& hex, const std::string& name)
{
    std::vector<unsigned char> vch = ParseHexV(hex, name);
    if (vch.size() != 4) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, name+" must be exactly 4 bytes / 8 hex");
    }
    uint32_t ret = 0;
    ret |= vch[0] << 24;
    ret |= vch[1] << 16;
    ret |= vch[2] <<  8;
    ret |= vch[3];
    return ret;
}

uint256 ParseUInt256(const UniValue& hex, const std::string& name)
{
    if (!hex.isStr()) {
        throw std::runtime_error(name+" must be a hexidecimal string");
    }
    std::vector<unsigned char> vch = ParseHex(hex.get_str());
    if (vch.size() != 32) {
        throw std::runtime_error(name+" must be exactly 32 bytes / 64 hex");
    }
    uint256 ret;
    std::copy(vch.begin(), vch.end(), ret.begin());
    return ret;
}

static uint256 AuxWorkMerkleRoot(const std::map<uint256, AuxWork>& mmwork)
{
    // If there is nothing to commit to, then the default zero hash is as good
    // as any other value.
    if (mmwork.empty()) {
        return uint256();
    }
    // The protocol supports an effectively limitless number of auxiliary
    // commitments under the Merkle root, however code has not yet been written
    // to generate root values and proofs for arbitrary trees.
    if (mmwork.size() != 1) {
        throw std::runtime_error("AuxWorkMerkleRoot: we do not yet support more than one merge-mining commitment");
    }
    // For now, we've hard-coded the special case of a single hash commitment:
    uint256 key = mmwork.begin()->first;
    uint256 value = mmwork.begin()->second.commit;
    uint256 ret = ComputeMerkleMapRootFromBranch(value, {}, key);
    return ret;
}

static double ClampDifficulty(const StratumClient& client, double diff)
{
    if (client.m_mindiff > 0) {
        diff = client.m_mindiff;
    }
    diff = std::max(diff, 0.001);
    return diff;
}

static std::string GetExtraNonceRequest(StratumClient& client, const uint256& job_id)
{
    std::string ret;
    if (client.m_supports_extranonce) {
        const std::string k_extranonce_req = std::string()
            + "{"
            +     "\"id\":";
        const std::string k_extranonce_req2 = std::string()
            +     ","
            +     "\"method\":\"mining.set_extranonce\","
            +     "\"params\":["
            +         "\"";
        const std::string k_extranonce_req3 = std::string()
            +            "\"," // extranonce1
            +         "4"      // extranonce2.size()
            +     "]"
            + "}"
            + "\n";

        ret = k_extranonce_req
            + strprintf("%d", client.m_nextid++)
            + k_extranonce_req2
            + HexStr(client.ExtraNonce1(job_id))
            + k_extranonce_req3;
    }
    return ret;
}

std::string GetWorkUnit(StratumClient& client)
{
    LOCK(cs_main);

    if (vNodes.empty() && !Params().MineBlocksOnDemand()) {
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Bitcoin is not connected!");
    }

    if (IsInitialBlockDownload()) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Bitcoin is downloading blocks...");
    }

    if (!client.m_authorized) {
        throw JSONRPCError(RPC_INVALID_REQUEST, "Stratum client not authorized.  Use mining.authorize first, with a Bitcoin address as the username.");
    }

    auto second_stage =
        GetSecondStageWork(client.m_last_second_stage
                           ? boost::optional<uint256>(client.m_last_second_stage->first)
                           : boost::none);
    if (second_stage) {
        double diff = ClampDifficulty(client, second_stage->second.diff);

        UniValue set_difficulty(UniValue::VOBJ);
        set_difficulty.push_back(Pair("id", client.m_nextid++));
        set_difficulty.push_back(Pair("method", "mining.set_difficulty"));
        UniValue set_difficulty_params(UniValue::VARR);
        set_difficulty_params.push_back(UniValue(diff));
        set_difficulty.push_back(Pair("params", set_difficulty_params));

        std::string job_id = ":" + second_stage->second.job_id;

        UniValue mining_notify(UniValue::VOBJ);
        mining_notify.push_back(Pair("id", client.m_nextid++));
        mining_notify.push_back(Pair("method", "mining.notify"));
        UniValue mining_notify_params(UniValue::VARR);
        mining_notify_params.push_back(job_id);
        // Byte-swap the hashPrevBlock, as stratum expects.
        uint256 hashPrevBlock(second_stage->second.hashPrevBlock);
        for (int i = 0; i < 256/32; ++i) {
            ((uint32_t*)hashPrevBlock.begin())[i] = bswap_32(
            ((uint32_t*)hashPrevBlock.begin())[i]);
        }
        mining_notify_params.push_back(HexStr(hashPrevBlock.begin(),
                                              hashPrevBlock.end()));
        mining_notify_params.push_back(HexStr(second_stage->second.cb1.begin(),
                                              second_stage->second.cb1.end()));
        mining_notify_params.push_back(HexStr(second_stage->second.cb2.begin(),
                                              second_stage->second.cb2.end()));
        // Reverse the order of the hashes, because that's what stratum does.
        UniValue branch(UniValue::VARR);
        for (const uint256& hash : second_stage->second.cb_branch) {
            branch.push_back(HexStr(hash.begin(),
                                    hash.end()));
        }
        mining_notify_params.push_back(branch);
        mining_notify_params.push_back(HexInt4(second_stage->second.nVersion));
        mining_notify_params.push_back(HexInt4(second_stage->second.nBits));
        mining_notify_params.push_back(HexInt4(second_stage->second.nTime));
        if (client.m_last_second_stage && (client.m_last_second_stage->first == second_stage->first) && (client.m_last_second_stage->second == second_stage->second.hashPrevBlock)) {
            mining_notify_params.push_back(UniValue(false));
        } else {
            mining_notify_params.push_back(UniValue(true));
        }
        mining_notify.push_back(Pair("params", mining_notify_params));

        second_stages[second_stage->second.job_id] = *second_stage;

        client.m_last_second_stage =
            std::make_pair(second_stage->first,
                           second_stage->second.hashPrevBlock);

        return GetExtraNonceRequest(client, second_stage->first) // note: not job_id
             + set_difficulty.write() + "\n"
             + mining_notify.write() + "\n";
    } else {
        client.m_last_second_stage = boost::none;
        second_stages.clear();
    }

    static CBlockIndex* tip = NULL;
    static uint256 job_id;
    static unsigned int transactions_updated_last = 0;
    static int64_t last_update_time = 0;

    if (tip != chainActive.Tip() || (mempool.GetTransactionsUpdated() != transactions_updated_last && (GetTime() - last_update_time) > 5) || !work_templates.count(job_id))
    {
        CBlockIndex *tip_new = chainActive.Tip();
        const CScript script = CScript() << OP_FALSE;
        CBlockTemplate *new_work = BlockAssembler(Params()).CreateNewBlock(script);
        if (!new_work) {
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");
        }
        // So that block.GetHash() is correct
        new_work->block.hashMerkleRoot = BlockMerkleRoot(new_work->block);

        job_id = new_work->block.GetHash();
        work_templates[job_id] = StratumWork(tip_new, tip_new->nHeight + 1, *new_work);
        tip = tip_new;

        transactions_updated_last = mempool.GetTransactionsUpdated();
        last_update_time = GetTime();

        delete new_work;
        new_work = NULL;

        LogPrint("stratum", "New stratum block template (%d total): %s\n", work_templates.size(), HexStr(job_id.begin(), job_id.end()));

        // Remove any old templates
        std::vector<uint256> old_job_ids;
        boost::optional<uint256> oldest_job_id = boost::none;
        uint32_t oldest_job_nTime = last_update_time;
        for (const auto& work_template : work_templates) {
            // If, for whatever reason the new work was generated with
            // an old nTime, don't erase it!
            if (work_template.first == job_id) {
                continue;
            }
            // Build a list of outdated work units to free.
            if (work_template.second.GetBlock().nTime < (last_update_time - 900)) {
                old_job_ids.push_back(work_template.first);
            }
            // Track the oldest work unit, in case we have too much
            // recent work.
            if (work_template.second.GetBlock().nTime <= oldest_job_nTime) {
                oldest_job_id = work_template.first;
                oldest_job_nTime = work_template.second.GetBlock().nTime;
            }
        }
        // Remove all outdated work.
        for (const auto& old_job_id : old_job_ids) {
            work_templates.erase(old_job_id);
            LogPrint("stratum", "Removed outdated stratum block template (%d total): %s\n", work_templates.size(), HexStr(old_job_id.begin(), old_job_id.end()));
        }
        // Remove the oldest work unit if we're still over the maximum
        // number of stored work templates.
        if (work_templates.size() > 30 && oldest_job_id) {
            work_templates.erase(oldest_job_id.get());
            LogPrint("stratum", "Removed oldest stratum block template (%d total): %s\n", work_templates.size(), HexStr(oldest_job_id.get().begin(), oldest_job_id.get().end()));
        }

        // Do the same for merge-mining work
        std::vector<uint256> old_mmwork_ids;
        boost::optional<uint256> oldest_mmwork_id = boost::none;
        uint64_t oldest_mmwork_timestamp = static_cast<uint64_t>(last_update_time) * 1000;
        const uint64_t cutoff_timestamp = oldest_mmwork_timestamp - (900 * 1000);
        for (const auto& mmwork : client.m_mmwork) {
            // Build a list of outdated work units to free
            if (mmwork.second.first < cutoff_timestamp) {
                old_mmwork_ids.push_back(mmwork.first);
            }
            // Track the oldest work unit, in case we have too much recent work.
            if (mmwork.second.first <= oldest_mmwork_timestamp) {
                oldest_mmwork_id = mmwork.first;
                oldest_mmwork_timestamp = mmwork.second.first;
            }
        }
        // Remove outdated mmwork units.
        for (const auto& old_mmwork_id : old_mmwork_ids) {
            client.m_mmwork.erase(old_mmwork_id);
            LogPrint("mergemine", "Removed outdated merge-mining work unit for miner %s from %s (%d total): %s\n", client.m_addr.ToString(), client.GetPeer().ToString(), client.m_mmwork.size(), HexStr(old_mmwork_id.begin(), old_mmwork_id.end()));
        }
        // Remove the oldest mmwork unit if we're still over the maximum number
        // of stored mmwork templates.
        if (client.m_mmwork.size() > 30 && oldest_mmwork_id) {
            client.m_mmwork.erase(oldest_mmwork_id.get());
            LogPrint("mergemine", "Removed oldest merge-mining work unit for miner %s from %s (%d total): %s\n", client.m_addr.ToString(), client.GetPeer().ToString(), client.m_mmwork.size(), HexStr(oldest_mmwork_id.get().begin(), oldest_mmwork_id.get().end()));
        }
    }

    StratumWork& current_work = work_templates[job_id];

    CMutableTransaction cb(current_work.GetBlock().vtx[0]);
    CMutableTransaction bf(current_work.GetBlock().vtx.back());

    // Our first customization of the work template is the insert merge-mine
    // block header commitments, but we can only do that if the template has a
    // block-final transaction.
    uint32_t max_bits = current_work.GetBlock().nBits;
    bool has_merge_mining = false;
    uint256 mmroot;
    if (current_work.m_block_template.has_block_final_tx) {
        std::map<uint256, AuxWork> mmwork = GetMergeMineWork(client.m_mmauth);
        if (mmwork.empty()) {
            LogPrint("mergemine", "No auxiliary work commitments to add to block template for stratum miner %s from %s.\n", client.m_addr.ToString(), client.GetPeer().ToString());
        } else {
            mmroot = AuxWorkMerkleRoot(mmwork);
            if (!client.m_mmwork.count(mmroot)) {
                client.m_mmwork[mmroot] = std::make_pair(GetTimeMillis(), mmwork);
            }
            if (UpdateBlockFinalTransaction(bf, mmroot)) {
                LogPrint("stratum", "Updated merge-mining commitment in block-final transaction.\n");
                has_merge_mining = true;
            }
        }
    } else {
        if (!client.m_mmauth.empty()) {
            LogPrint("mergemine", "Cannot add merge-mining commitments to block template because there is no block-final transaction.\n");
        }
    }

    std::vector<uint256> cb_branch = current_work.m_cb_branch;
    if (current_work.m_is_witness_enabled) {
        UpdateSegwitCommitment(current_work, cb, bf, cb_branch);
        LogPrint("stratum", "Updated segwit commitment in coinbase.\n");
    }

    CBlockIndex tmp_index;
    tmp_index.nBits = current_work.GetBlock().nBits;
    double diff = ClampDifficulty(client, GetDifficulty(&tmp_index));

    UniValue set_difficulty(UniValue::VOBJ);
    set_difficulty.push_back(Pair("id", client.m_nextid++));
    set_difficulty.push_back(Pair("method", "mining.set_difficulty"));
    UniValue set_difficulty_params(UniValue::VARR);
    set_difficulty_params.push_back(UniValue(diff));
    set_difficulty.push_back(Pair("params", set_difficulty_params));

    auto nonce = client.ExtraNonce1(job_id);
    nonce.resize(nonce.size()+4, 0x00); // extranonce2
    cb.vin.front().scriptSig =
           CScript()
        << current_work.m_height
        << nonce;
    if (cb.vout.front().scriptPubKey == (CScript() << OP_FALSE)) {
        cb.vout.front().scriptPubKey =
            GetScriptForDestination(client.m_addr.Get());
    }

    CDataStream ds(SER_GETHASH, SERIALIZE_TRANSACTION_NO_WITNESS);
    ds << CTransaction(cb);
    if (ds.size() < (4 + 1 + 32 + 4 + 1)) {
        throw std::runtime_error("Serialized transaction is too small to be parsed.  Is this even a coinbase?");
    }
    size_t pos = 4 + 1 + 32 + 4 + 1 + ds[4+1+32+4];
    if (ds.size() < pos) {
        throw std::runtime_error("Customized coinbase transaction does not contain extranonce field at expected location.");
    }
    std::string cb1 = HexStr(&ds[0], &ds[pos-4-8]);
    std::string cb2 = HexStr(&ds[pos], &ds[ds.size()]);

    UniValue params(UniValue::VARR);
    params.push_back(HexStr(job_id.begin(), job_id.end()) + (has_merge_mining? ":" + HexStr(mmroot.begin(), mmroot.end()): ""));
    // For reasons of who-the-heck-knows-why, stratum byte-swaps each
    // 32-bit chunk of the hashPrevBlock.
    uint256 hashPrevBlock(current_work.GetBlock().hashPrevBlock);
    for (int i = 0; i < 256/32; ++i) {
        ((uint32_t*)hashPrevBlock.begin())[i] = bswap_32(
        ((uint32_t*)hashPrevBlock.begin())[i]);
    }
    params.push_back(HexStr(hashPrevBlock.begin(), hashPrevBlock.end()));
    params.push_back(cb1);
    params.push_back(cb2);

    UniValue branch(UniValue::VARR);
    for (const auto& hash : cb_branch) {
        branch.push_back(HexStr(hash.begin(), hash.end()));
    }
    params.push_back(branch);

    CBlockHeader blkhdr(current_work.GetBlock());
    int64_t delta = UpdateTime(&blkhdr, Params().GetConsensus(), tip);
    LogPrint("stratum", "Updated the timestamp of block template by %d seconds\n", delta);

    params.push_back(HexInt4(blkhdr.nVersion));
    params.push_back(HexInt4(blkhdr.nBits));
    params.push_back(HexInt4(blkhdr.nTime));
    params.push_back(UniValue(client.m_last_tip != tip));
    client.m_last_tip = tip;

    UniValue mining_notify(UniValue::VOBJ);
    mining_notify.push_back(Pair("params", params));
    mining_notify.push_back(Pair("id", client.m_nextid++));
    mining_notify.push_back(Pair("method", "mining.notify"));

    return GetExtraNonceRequest(client, job_id)
         + set_difficulty.write() + "\n"
         + mining_notify.write()  + "\n";
}

bool SubmitBlock(StratumClient& client, const uint256& job_id, const uint256& mmroot, const StratumWork& current_work, std::vector<unsigned char> extranonce2, uint32_t nTime, uint32_t nNonce, uint32_t nVersion)
{
    if (current_work.GetBlock().vtx.empty()) {
        const std::string msg("SubmitBlock: no transactions in block template; unable to submit work");
        LogPrint("stratum", "%s\n", msg);
        throw std::runtime_error(msg);
    }
    CMutableTransaction cb(current_work.GetBlock().vtx.front());
    if (cb.vin.size() != 1) {
        const std::string msg("SubmitBlock: unexpected number of inputs; is this even a coinbase transaction?");
        LogPrint("stratum", "%s\n", msg);
        throw std::runtime_error(msg);
    }
    auto nonce = client.ExtraNonce1(job_id);
    if ((nonce.size() + extranonce2.size()) != 12) {
        const std::string msg = strprintf("SubmitBlock: unexpected combined nonce length: extranonce1(%d) + extranonce2(%d) != 12; unable to submit work", nonce.size(), extranonce2.size());
        LogPrint("stratum", "%s\n", msg);
        throw std::runtime_error(msg);
    }
    nonce.insert(nonce.end(), extranonce2.begin(),
                              extranonce2.end());
    if (cb.vin.empty()) {
        const std::string msg("SubmitBlock: first transaction is missing coinbase input; unable to customize work to miner");
        LogPrint("stratum", "%s\n", msg);
        throw std::runtime_error(msg);
    }
    cb.vin.front().scriptSig =
           CScript()
        << current_work.m_height
        << nonce;
    if (cb.vout.empty()) {
        const std::string msg("SubmitBlock: coinbase transaction is missing outputs; unable to customize work to miner");
        LogPrint("stratum", "%s\n", msg);
        throw std::runtime_error(msg);
    }
    if (cb.vout.front().scriptPubKey == (CScript() << OP_FALSE)) {
        cb.vout.front().scriptPubKey =
            GetScriptForDestination(client.m_addr.Get());
    }

    CMutableTransaction bf(current_work.GetBlock().vtx.back());
    if (current_work.m_block_template.has_block_final_tx) {
        if (UpdateBlockFinalTransaction(bf, mmroot)) {
            LogPrint("stratum", "Updated merge-mining commitment in block-final transaction.\n");
        }
    }

    std::vector<uint256> cb_branch = current_work.m_cb_branch;
    if (current_work.m_is_witness_enabled) {
        UpdateSegwitCommitment(current_work, cb, bf, cb_branch);
        LogPrint("stratum", "Updated segwit commitment in coinbase.\n");
    }

    CBlockHeader blkhdr(current_work.GetBlock());
    blkhdr.hashMerkleRoot = ComputeMerkleRootFromBranch(cb.GetHash(), cb_branch, 0);
    blkhdr.nTime = nTime;
    blkhdr.nNonce = nNonce;
    blkhdr.nVersion = nVersion;

    bool res = false;
    uint256 hash = blkhdr.GetHash();
    if (CheckProofOfWork(hash, blkhdr.nBits, 0, Params().GetConsensus())) {
        LogPrintf("GOT BLOCK!!! by %s: %s\n", client.m_addr.ToString(), hash.ToString());
        CBlock block(current_work.GetBlock());
        block.vtx.front() = CTransaction(cb);
        if (current_work.m_is_witness_enabled) {
            block.vtx.back() = CTransaction(bf);
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nTime = nTime;
        block.nNonce = nNonce;
        block.nVersion = nVersion;
        CValidationState state;
        res = ProcessNewBlock(state, Params(), NULL, &block, true, NULL, false);
    } else {
        LogPrintf("NEW SHARE!!! by %s: %s\n", client.m_addr.ToString(), hash.ToString());
    }

    // Now we check if the work meets any of the auxiliary header requirements,
    // and if so submit them.
    //client.m_mmwork[mmroot] = std::make_pair(GetTimeMillis(), mmwork);
    if (current_work.m_is_witness_enabled && current_work.m_block_template.has_block_final_tx && client.m_mmwork.count(mmroot)) {
        AuxProof auxproof;
        CDataStream ds(SER_GETHASH, PROTOCOL_VERSION);
        ds << bf;
        ds.resize(ds.size() - 40);
        auxproof.midstate_buffer.resize(ds.size() % 64);
        uint64_t tmp = 0;
        CSHA256()
            .Write((unsigned char*)&ds[0], ds.size())
            .Midstate(auxproof.midstate_hash.begin(),
                      auxproof.midstate_buffer.data(),
                     &tmp);
        auxproof.midstate_length = static_cast<uint32_t>(tmp / 8);
        auxproof.lock_time = bf.nLockTime;
        std::vector<uint256> leaves;
        leaves.reserve(current_work.GetBlock().vtx.size());
        for (const auto& tx : current_work.GetBlock().vtx) {
            leaves.push_back(tx.GetHash());
        }
        leaves.front() = cb.GetHash();
        leaves.back() = bf.GetHash();
        auxproof.aux_branch = ComputeStableMerkleBranch(leaves, leaves.size()-1).first;
        auxproof.num_txns = leaves.size();
        auxproof.nVersion = blkhdr.nVersion;
        auxproof.hashPrevBlock = blkhdr.hashPrevBlock;
        auxproof.nTime = blkhdr.nTime;
        auxproof.nBits = blkhdr.nBits;
        auxproof.nNonce = blkhdr.nNonce;
        for (const auto& item : client.m_mmwork[mmroot].second) {
            const uint256& chainid = item.first;
            const AuxWork& auxwork = item.second;
            if (!client.m_mmauth.count(chainid)) {
                LogPrint("mergemine", "Got share for chain we aren't authorized for; unable to submit work.\n");
                continue;
            }
            const std::string& username = client.m_mmauth[chainid].first;
            SubmitAuxChainShare(chainid, username, auxwork, auxproof);
            // FIXME: Change to our own consensus params with no powlimit
            if (CheckProofOfWork(hash, auxwork.bits, auxwork.bias, Params().GetConsensus())) {
                LogPrintf("GOT AUX CHAIN BLOCK!!! 0x%s by %s: %s %s\n", HexStr(chainid.begin(), chainid.end()), username, auxwork.commit.ToString(), hash.ToString());
            } else {
                LogPrintf("NEW AUX CHAIN SHARE!!! 0x%s by %s: %s %s\n", HexStr(chainid.begin(), chainid.end()), username, auxwork.commit.ToString(), hash.ToString());
            }
        }
    }

    if (res) {
        client.m_send_work = true;
    }

    return res;
}

bool SubmitSecondStage(StratumClient& client, const uint256& chainid, const SecondStageWork& work, std::vector<unsigned char> extranonce2, uint32_t nTime, uint32_t nNonce, uint32_t nVersion)
{
    if (!client.m_mmauth.count(chainid)) {
        LogPrint("mergemine", "Got second stage share for chain we aren't authorized for; unable to submit work.\n");
        return false;
    }
    const std::string& username = client.m_mmauth[chainid].first;

    std::vector<unsigned char> extranonce1 = client.ExtraNonce1(chainid);

    SubmitSecondStageShare(chainid, username, work, SecondStageProof(extranonce1, extranonce2, nVersion, nTime, nNonce));

    uint256 hash;
    CSHA256()
        .Write(&work.cb1[0], work.cb1.size())
        .Write(&extranonce1[0], extranonce1.size())
        .Write(&extranonce2[0], extranonce2.size())
        .Write(&work.cb2[0], work.cb2.size())
        .Finalize(hash.begin());
    CSHA256()
        .Write(hash.begin(), 32)
        .Finalize(hash.begin());

    CBlockHeader blkhdr;
    blkhdr.nVersion = nVersion;
    blkhdr.hashPrevBlock = work.hashPrevBlock;
    blkhdr.hashMerkleRoot = ComputeMerkleRootFromBranch(hash, work.cb_branch, 0);
    blkhdr.nTime = nTime;
    blkhdr.nBits = work.nBits;
    blkhdr.nNonce = nNonce;
    hash = blkhdr.GetHash();

    bool res = false;
    // FIXME: Change to our own consensus params with no powlimit
    if ((res = CheckProofOfWork(hash, work.nBits, 0, Params().GetConsensus()))) {
        LogPrintf("GOT AUX CHAIN SECOND STAGE BLOCK!!! 0x%s by %s: %s\n", HexStr(chainid.begin(), chainid.end()), username, hash.ToString());
    } else {
        LogPrintf("NEW AUX CHAIN SECOND STAGE SHARE!!! 0x%s by %s: %s\n", HexStr(chainid.begin(), chainid.end()), username, hash.ToString());
    }

    if (res) {
        client.m_send_work = true;
    }

    return res;
}

void BoundParams(const std::string& method, const UniValue& params, size_t min, size_t max)
{
    if (params.size() < min) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s expects at least %d parameters; received %d", method, min, params.size()));
    }

    if (params.size() > max) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s receives no more than %d parameters; got %d", method, max, params.size()));
    }
}

UniValue stratum_mining_subscribe(StratumClient& client, const UniValue& params)
{
    const std::string method("mining.subscribe");
    BoundParams(method, params, 0, 2);

    if (params.size() >= 1) {
        client.m_client = params[0].get_str();
        LogPrint("stratum", "Received subscription from client %s\n", client.m_client);
    }

    // params[1] is the subscription ID for reconnect, which we
    // currently do not support.

    UniValue msg(UniValue::VARR);

    // Some mining proxies (e.g. Nicehash) reject connections that don't send
    // a reasonable difficulty on first connection.  The actual value will be
    // overridden when the miner is authorized and work is delivered.  Oh, and
    // for reasons unknown it is sent in serialized float format rather than
    // as a numeric value...
    UniValue set_difficulty(UniValue::VARR);
    set_difficulty.push_back("mining.set_difficulty");
    set_difficulty.push_back("1e+06"); // Will be overriden by later
    msg.push_back(set_difficulty);     // work delivery messages.

    UniValue notify(UniValue::VARR);
    notify.push_back("mining.notify");
    notify.push_back("ae6812eb4cd7735a302a8a9dd95cf71f");
    msg.push_back(notify);

    UniValue ret(UniValue::VARR);
    ret.push_back(msg);
    // client.m_supports_extranonce is false, so the job_id isn't used.
    ret.push_back(HexStr(client.ExtraNonce1(uint256())));
    ret.push_back(UniValue(4)); // sizeof(extranonce2)

    return ret;
}

UniValue stratum_mining_authorize(StratumClient& client, const UniValue& params)
{
    const std::string method("mining.authorize");
    BoundParams(method, params, 1, 2);

    std::string username = params[0].get_str();
    boost::trim(username);

    // params[1] is the client-provided password.  We do not perform
    // user authorization, but we instead allow the password field to
    // be used to specify merge-mining parameters.
    std::string password = params[1].get_str();
    boost::trim(password);

    size_t start = 0;
    size_t pos = 0;
    std::vector<std::string> opts;
    while ((pos = password.find(',', start)) != std::string::npos) {
        std::string opt(password, start, pos);
        boost::trim(opt);
        if (opt.empty()) {
            continue;
        }
        opts.push_back(opt);
        start = pos + 1;
    }
    std::string opt(password, start);
    boost::trim(opt);
    if (!opt.empty()) {
        opts.push_back(opt);
    }

    std::map<uint256, std::pair<std::string, std::string> > mmauth;
    for (const std::string& opt : opts) {
        if ((pos = opt.find('=')) != std::string::npos) {
            std::string key(opt, 0, pos); // chain name or ID
            boost::trim_right(key);
            std::string value(opt, pos+1); // pass-through to chain server
            boost::trim_left(value);
            std::string username(value);
            std::string password;
            if ((pos = value.find(':')) != std::string::npos) {
                username.resize(pos);
                password = value.substr(pos+1);
            }
            if (chain_names.count(key)) {
                const uint256& chainid = chain_names[key];
                LogPrint("mergemine", "Merge-mine chain \"%s\" (0x%s) with username \"%s\" and password \"%s\"\n", key, HexStr(chainid.begin(), chainid.end()), username, password);
                mmauth[chainid] = std::make_pair(username, password);
            } else {
                uint256 chainid = ParseUInt256(key, "chainid");
                std::vector<unsigned char> zero(24, 0x00);
                if (memcmp(chainid.begin()+8, zero.data(), 24) == 0) {
                    // At least 24 bytes are empty. Gonna go out on a limb and
                    // say this wasn't a hex-encoded aux_pow_path.
                    LogPrint("mergemine", "Skipping unrecognized stratum password keyword option \"%s=%s\"\n", key, value);
                } else {
                    if (mmauth.count(chainid)) {
                        LogPrint("mergemine", "Duplicate chain 0x%s; skipping\n");
                        continue;
                    }
                    LogPrint("mergemine", "Merge-mine chain 0x%s with username \"%s\" and password \"%s\"\n", HexStr(chainid.begin(), chainid.end()), username, password);
                    mmauth[chainid] = std::make_pair(username, password);
                }
            }
        } else {
            CBitcoinAddress addr(opt);
            if (addr.IsValid()) {
                const uint256& chainid = Params().DefaultAuxPowPath();
                if (mmauth.count(chainid)) {
                    LogPrint("mergemine", "Duplicate chain 0x%s (default); skipping\n");
                    continue;
                }
                std::string username(addr.ToString());
                std::string password("x");
                LogPrint("mergemine", "Merge-mine chain 0x%s with username \"%s\" and password \"%s\"\n", HexStr(chainid.begin(), chainid.end()), username, password);
                mmauth[chainid] = std::make_pair(username, password);
            } else {
                LogPrint("mergemine", "Skipping unrecognized stratum password option \"%s\"\n", opt);
            }
        }
    }

    double mindiff = 0.0;
    pos = username.find('+');
    if (pos != std::string::npos) {
        // Extract the suffix and trim it
        std::string suffix(username, pos+1);
        boost::trim_left(suffix);
        // Extract the minimum difficulty request
        mindiff = boost::lexical_cast<double>(suffix);
        // Remove the '+' and everything after
        username.resize(pos);
        boost::trim_right(username);
    }

    CBitcoinAddress addr(username);

    if (!addr.IsValid()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid Bitcoin address: %s", username));
    }

    client.m_addr = addr;
    swap(client.m_mmauth, mmauth);
    for (const auto& item : client.m_mmauth) {
        RegisterMergeMineClient(item.first, item.second.first, item.second.second);
    }
    client.m_mindiff = mindiff;
    client.m_authorized = true;

    client.m_send_work = true;

    LogPrintf("Authorized stratum miner %s from %s, mindiff=%f\n", addr.ToString(), client.GetPeer().ToString(), mindiff);

    return true;
}

UniValue stratum_mining_configure(StratumClient& client, const UniValue& params)
{
    const std::string method("mining.configure");
    BoundParams(method, params, 2, 2);

    UniValue res(UniValue::VOBJ);

    UniValue extensions = params[0].get_array();
    UniValue config = params[1].get_obj();
    for (int i = 0; i < extensions.size(); ++i) {
        std::string name = extensions[i].get_str();

        if ("version-rolling" == name) {
            uint32_t mask = ParseHexInt4(find_value(config, "version-rolling.mask"), "version-rolling.mask");
            size_t min_bit_count = find_value(config, "version-rolling.min-bit-count").get_int();
            client.m_version_rolling_mask = mask & 0x1fffe000;
            res.push_back(Pair("version-rolling", true));
            res.push_back(Pair("version-rolling.mask", HexInt4(client.m_version_rolling_mask)));
            LogPrint("stratum", "Received version rolling request from %s\n", client.GetPeer().ToString());
        }

        else {
            LogPrint("stratum", "Unrecognized stratum extension '%s' sent by %s\n", name, client.GetPeer().ToString());
        }
    }

    return res;
}

UniValue stratum_mining_submit(StratumClient& client, const UniValue& params)
{
    const std::string method("mining.submit");
    BoundParams(method, params, 5, 6);
    // First parameter is the client username, which is ignored.

    std::string id = params[1].get_str();

    std::vector<unsigned char> extranonce2 = ParseHexV(params[2], "extranonce2");
    if (extranonce2.size() != 4) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("extranonce2 is wrong length (received %d bytes; expected %d bytes", extranonce2.size(), 4));
    }
    uint32_t nTime = ParseHexInt4(params[3], "nTime");
    uint32_t nNonce = ParseHexInt4(params[4], "nNonce");

    if (id[0] == ':') {
        // Second stage work unit
        std::string job_id(id, 1);
        if (!second_stages.count(job_id)) {
            LogPrint("stratum", "Received completed share for unknown second stage work : %s\n", id);
            client.m_send_work = true;
            return false;
        }
        const auto& item = second_stages[job_id];
        const uint256& aux_pow_path = item.first;
        const SecondStageWork& second_stage = item.second;

        uint32_t nVersion = second_stage.nVersion;
        if (params.size() > 5) {
            uint32_t bits = ParseHexInt4(params[5], "nVersion");
            nVersion = (nVersion & ~client.m_version_rolling_mask)
                     | (bits & client.m_version_rolling_mask);
        }

        SubmitSecondStage(client, aux_pow_path, second_stage, extranonce2, nTime, nNonce, nVersion);
    } else {
        uint256 job_id, mmroot;
        if ((pos = id.find(':', 0)) != std::string::npos) {
            mmroot = ParseUInt256(std::string(id, pos+1), "mmroot");
            id.resize(pos);
        }
        job_id = ParseUInt256(id, "job_id");

        if (!work_templates.count(job_id)) {
            LogPrint("stratum", "Received completed share for unknown job_id : %s\n", HexStr(job_id.begin(), job_id.end()));
            client.m_send_work = true;
            return false;
        }
        StratumWork &current_work = work_templates[job_id];

        uint32_t nVersion = current_work.GetBlock().nVersion;
        if (params.size() > 5) {
            uint32_t bits = ParseHexInt4(params[5], "nVersion");
            nVersion = (nVersion & ~client.m_version_rolling_mask)
                     | (bits & client.m_version_rolling_mask);
        }

        SubmitBlock(client, job_id, mmroot, current_work, extranonce2, nTime, nNonce, nVersion);
    }

    return true;
}

UniValue stratum_mining_extranonce_subscribe(StratumClient& client, const UniValue& params)
{
    const std::string method("mining.extranonce.subscribe");
    BoundParams(method, params, 0, 0);

    client.m_supports_extranonce = true;

    return true;
}

/** Callback to read from a stratum connection. */
static void stratum_read_cb(bufferevent *bev, void *ctx)
{
    evconnlistener *listener = (evconnlistener*)ctx;
    LOCK(cs_stratum);
    // Lookup the client record for this connection
    if (!subscriptions.count(bev)) {
        LogPrint("stratum", "Received read notification for unknown stratum connection 0x%x\n", (size_t)bev);
        return;
    }
    StratumClient& client = subscriptions[bev];
    // Get links to the input and output buffers
    evbuffer *input = bufferevent_get_input(bev);
    evbuffer *output = bufferevent_get_output(bev);
    // Process each line of input that we have received
    char *cstr = 0;
    size_t len = 0;
    while ((cstr = evbuffer_readln(input, &len, EVBUFFER_EOL_CRLF))) {
        std::string line(cstr, len);
        free(cstr);
        LogPrint("stratum", "Received stratum request from %s : %s\n", client.GetPeer().ToString(), line);

        JSONRequest jreq;
        std::string reply;
        try {
            // Parse request
            UniValue valRequest;
            if (!valRequest.read(line)) {
                // Not JSON; is this even a stratum miner?
                throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");
            }
            if (!valRequest.isObject()) {
                // Not a JSON object; don't know what to do.
                throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");
            }
            if (valRequest.exists("result")) {
                // JSON-RPC reply.  Ignore.
                LogPrint("stratum", "Ignoring JSON-RPC response\n");
                continue;
            }
            jreq.parse(valRequest);

            // Dispatch to method handler
            UniValue result = NullUniValue;
            if (stratum_method_dispatch.count(jreq.strMethod)) {
                result = stratum_method_dispatch[jreq.strMethod](client, jreq.params);
            } else {
                throw JSONRPCError(RPC_METHOD_NOT_FOUND, strprintf("Method '%s' not found", jreq.strMethod));
            }

            // Compose reply
            reply = JSONRPCReply(result, NullUniValue, jreq.id);
        } catch (const UniValue& objError) {
            reply = JSONRPCReply(NullUniValue, objError, jreq.id);
        } catch (const std::exception& e) {
            reply = JSONRPCReply(NullUniValue, JSONRPCError(RPC_INTERNAL_ERROR, e.what()), jreq.id);
        }

        LogPrint("stratum", "Sending stratum response to %s : %s", client.GetPeer().ToString(), reply);
        if (evbuffer_add(output, reply.data(), reply.size())) {
            LogPrint("stratum", "Sending stratum response failed. (Reason: %d, '%s')\n", errno, evutil_socket_error_to_string(errno));
        }
    }

    // If required, send new work to the client.
    if (client.m_send_work) {
        std::string data;
        try {
            data = GetWorkUnit(client);
        } catch (const UniValue& objError) {
            data = JSONRPCReply(NullUniValue, objError, NullUniValue);
        } catch (const std::exception& e) {
            data = JSONRPCReply(NullUniValue, JSONRPCError(RPC_INTERNAL_ERROR, e.what()), NullUniValue);
        }

        LogPrint("stratum", "Sending requested stratum work unit to %s : %s", client.GetPeer().ToString(), data);
        if (evbuffer_add(output, data.data(), data.size())) {
            LogPrint("stratum", "Sending stratum work unit failed. (Reason: %d, '%s')\n", errno, evutil_socket_error_to_string(errno));
        }

        client.m_send_work = false;
    }
}

/** Callback to handle unrecoverable errors in a stratum link. */
static void stratum_event_cb(bufferevent *bev, short what, void *ctx)
{
    evconnlistener *listener = (evconnlistener*)ctx;
    LOCK(cs_stratum);
    // Fetch the return address for this connection, for the debug log.
    std::string from("UNKNOWN");
    if (!subscriptions.count(bev)) {
        LogPrint("stratum", "Received event notification for unknown stratum connection 0x%x\n", (size_t)bev);
        return;
    } else {
        from = subscriptions[bev].GetPeer().ToString();
    }
    // Report the reason why we are closing the connection.
    if (what & BEV_EVENT_ERROR) {
        LogPrint("stratum", "Error detected on stratum connection from %s\n", from);
    }
    if (what & BEV_EVENT_EOF) {
        LogPrint("stratum", "Remote disconnect received on stratum connection from %s\n", from);
    }
    // Remove the connection from our records, and tell libevent to
    // disconnect and free its resources.
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        LogPrint("stratum", "Closing stratum connection from %s\n", from);
        subscriptions.erase(bev);
        if (bev) {
            bufferevent_free(bev);
            bev = NULL;
        }
    }
}

/** Callback to accept a stratum connection. */
static void stratum_accept_conn_cb(evconnlistener *listener, evutil_socket_t fd, sockaddr *address, int socklen, void *ctx)
{
    LOCK(cs_stratum);
    // Parse the return address
    CService from;
    from.SetSockAddr(address);
    // Early address-based allow check
    if (!ClientAllowed(stratum_allow_subnets, from)) {
        evconnlistener_free(listener);
        LogPrint("stratum", "Rejected connection from disallowed subnet: %s\n", from.ToString());
        return;
    }
    // Should be the same as EventBase(), but let's get it the
    // official way.
    event_base *base = evconnlistener_get_base(listener);
    // Create a buffer for sending/receiving from this connection.
    bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    // Disable Nagle's algorithm, so that TCP packets are sent
    // immediately, even if it results in a small packet.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
    // Setup the read and event callbacks to handle receiving requests
    // from the miner and error handling.  A write callback isn't
    // needed because we're not sending enough data to fill buffers.
    bufferevent_setcb(bev, stratum_read_cb, NULL, stratum_event_cb, (void*)listener);
    // Enable bidirectional communication on the connection.
    bufferevent_enable(bev, EV_READ|EV_WRITE);
    // Record the connection state
    subscriptions[bev] = StratumClient(listener, fd, bev, from);
    // Log the connection.
    LogPrint("stratum", "Accepted stratum connection from %s\n", from.ToString());
}

/** Setup the stratum connection listening services */
static bool StratumBindAddresses(event_base* base)
{
    int defaultPort = GetArg("-stratumport", BaseParams().StratumPort());
    std::vector<std::pair<std::string, uint16_t> > endpoints;

    // Determine what addresses to bind to
    if (!InitEndpointList("stratum", defaultPort, endpoints))
        return false;

    // Bind each addresses
    for (const auto& endpoint : endpoints) {
        LogPrint("stratum", "Binding stratum on address %s port %i\n", endpoint.first, endpoint.second);
        // Use CService to translate string -> sockaddr
        CService socket(CNetAddr(endpoint.first), endpoint.second);
        union {
            sockaddr     ipv4;
            sockaddr_in6 ipv6;
        } addr;
        socklen_t len = sizeof(addr);
        socket.GetSockAddr((sockaddr*)&addr, &len);
        // Setup an event listener for the endpoint
        evconnlistener *listener = evconnlistener_new_bind(base, stratum_accept_conn_cb, NULL, LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE, -1, (sockaddr*)&addr, len);
        // Only record successful binds
        if (listener) {
            bound_listeners[listener] = socket;
        } else {
            LogPrintf("Binding stratum on address %s port %i failed. (Reason: %d, '%s')\n", endpoint.first, endpoint.second, errno, evutil_socket_error_to_string(errno));
        }
    }

    return !bound_listeners.empty();
}

/** Watches for new blocks and send updated work to miners. */
static bool g_shutdown = false;
void BlockWatcher()
{
    boost::unique_lock<boost::mutex> lock(csBestBlock);
    boost::system_time checktxtime = boost::get_system_time();
    unsigned int txns_updated_last = 0;
    while (true) {
        checktxtime += boost::posix_time::seconds(15);
        if (!cvBlockChange.timed_wait(lock, checktxtime)) {
            // Attempt to re-establish any connections that have been dropped.
            ReconnectToMergeMineEndpoints();

            // Timeout: Check to see if mempool was updated.
            unsigned int txns_updated_next = mempool.GetTransactionsUpdated();
            if (txns_updated_last == txns_updated_next)
                continue;
            txns_updated_last = txns_updated_next;
        }

        // Attempt to re-establish any connections that have been dropped.
        ReconnectToMergeMineEndpoints();

        LOCK(cs_stratum);

        if (g_shutdown) {
            break;
        }

        // Either new block, updated transactions, or updated merge-mining
        // commitments.  Either way, send updated work to miners.
        for (auto& subscription : subscriptions) {
            bufferevent* bev = subscription.first;
            evbuffer *output = bufferevent_get_output(bev);
            StratumClient& client = subscription.second;
            // Ignore clients that aren't authorized yet.
            if (!client.m_authorized) {
                continue;
            }
            // Ignore clients that are already working on the current second
            // stage work unit.
            auto second_stage =
                GetSecondStageWork(client.m_last_second_stage
                                 ? boost::optional<uint256>(client.m_last_second_stage->first)
                                 : boost::none);
            if (second_stage && client.m_last_second_stage && (client.m_last_second_stage.get() == std::make_pair(second_stage.get().first, second_stage.get().second.hashPrevBlock))) {
                continue;
            }
            // Ignore clients that are already working on the new block.
            // Typically this is just the miner that found the block, who was
            // immediately sent a work update.  This check avoids sending that
            // work notification again, moments later.  Due to race conditions
            // there could be more than one miner that have already received an
            // update, however.
            if (!second_stage) {
                std::map<uint256, AuxWork> mmwork = GetMergeMineWork(client.m_mmauth);
                uint256 mmroot = AuxWorkMerkleRoot(mmwork);
                if ((client.m_last_tip == chainActive.Tip()) && client.m_mmwork.count(mmroot)) {
                    continue;
                }
            }
            // Get new work
            std::string data;
            try {
                data = GetWorkUnit(client);
            } catch (const UniValue& objError) {
                data = JSONRPCReply(NullUniValue, objError, NullUniValue);
            } catch (const std::exception& e) {
                // Some sort of error.  Ignore.
                std::string msg = strprintf("Error generating updated work for stratum client: %s", e.what());
                LogPrint("stratum", "%s\n", msg);
                data = JSONRPCReply(NullUniValue, JSONRPCError(RPC_INTERNAL_ERROR, msg), NullUniValue);
            }
            // Send the new work to the client
            LogPrint("stratum", "Sending updated stratum work unit to %s : %s", client.GetPeer().ToString(), data);
            if (evbuffer_add(output, data.data(), data.size())) {
                LogPrint("stratum", "Sending stratum work unit failed. (Reason: %d, '%s')\n", errno, evutil_socket_error_to_string(errno));
            }
        }
    }
}

/** Configure the stratum server */
bool InitStratumServer()
{
    LOCK(cs_stratum);

    if (!InitSubnetAllowList("stratum", stratum_allow_subnets)) {
        LogPrint("stratum", "Unable to bind stratum server to an endpoint.\n");
        return false;
    }

    std::string strAllowed;
    for (const auto& subnet : stratum_allow_subnets) {
        strAllowed += subnet.ToString() + " ";
    }
    LogPrint("stratum", "Allowing stratum connections from: %s\n", strAllowed);

    event_base* base = EventBase();
    if (!base) {
        LogPrint("stratum", "No event_base object, cannot setup stratum server.\n");
        return false;
    }

    if (!StratumBindAddresses(base)) {
        LogPrintf("Unable to bind any endpoint for stratum server\n");
    } else {
        LogPrint("stratum", "Initialized stratum server\n");
    }

    stratum_method_dispatch["mining.subscribe"] = stratum_mining_subscribe;
    stratum_method_dispatch["mining.authorize"] = stratum_mining_authorize;
    stratum_method_dispatch["mining.configure"] = stratum_mining_configure;
    stratum_method_dispatch["mining.submit"]    = stratum_mining_submit;
    stratum_method_dispatch["mining.extranonce.subscribe"] =
        stratum_mining_extranonce_subscribe;

    // Start thread to wait for block notifications and send updated
    // work to miners.
    block_watcher_thread = boost::thread(BlockWatcher);

    return true;
}

/** Interrupt the stratum server connections */
void InterruptStratumServer()
{
    LOCK(cs_stratum);
    // Stop listening for connections on stratum sockets
    for (const auto& binding : bound_listeners) {
        LogPrint("stratum", "Interrupting stratum service on %s\n", binding.second.ToString());
        evconnlistener_disable(binding.first);
    }
    // Tell the block watching thread to stop
    g_shutdown = true;
}

/** Cleanup stratum server network connections and free resources. */
void StopStratumServer()
{
    LOCK(cs_stratum);
    /* Tear-down active connections. */
    for (const auto& subscription : subscriptions) {
        LogPrint("stratum", "Closing stratum server connection to %s due to process termination\n", subscription.second.GetPeer().ToString());
        bufferevent_free(subscription.first);
    }
    subscriptions.clear();
    /* Un-bind our listeners from their network interfaces. */
    for (const auto& binding : bound_listeners) {
        LogPrint("stratum", "Removing stratum server binding on %s\n", binding.second.ToString());
        evconnlistener_free(binding.first);
    }
    bound_listeners.clear();
    /* Free any allocated block templates. */
    work_templates.clear();
}

// End of File
