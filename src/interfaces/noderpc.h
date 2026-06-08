// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_NODERPC_H
#define BITCOIN_INTERFACES_NODERPC_H

#include <primitives/block.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace node {
struct NodeContext;
} // namespace node

namespace interfaces {

struct BlockchainPruneInfo {
    int height{0};
    bool automatic{false};
    uint64_t target_size{0};
};

struct BlockchainInfo {
    std::string chain;
    int blocks{0};
    int headers{0};
    std::string bestblockhash;
    std::string bits;
    std::string target;
    double difficulty{0};
    int64_t time{0};
    int64_t mediantime{0};
    double verificationprogress{0};
    bool initialblockdownload{false};
    std::string chainwork;
    uint64_t size_on_disk{0};
    bool pruned{false};
    std::optional<BlockchainPruneInfo> prune;
    std::optional<std::string> signet_challenge;
    std::vector<std::string> warnings;
};

struct BlockHeaderInfo {
    std::string hash;
    int confirmations{0};
    int height{0};
    int version{0};
    std::string version_hex;
    std::string merkleroot;
    int64_t time{0};
    int64_t mediantime{0};
    uint32_t nonce{0};
    std::string bits;
    std::string target;
    double difficulty{0};
    std::string chainwork;
    int n_tx{0};
    std::optional<std::string> previousblockhash;
    std::optional<std::string> nextblockhash;
};

struct SmartFeeEstimate {
    int64_t feerate{0};
    std::vector<std::string> errors;
    int blocks{0};
};

struct TxOutScriptPubKey {
    std::string script_asm;
    std::string desc;
    std::string hex;
    std::string type;
    std::optional<std::string> address;
};

struct TxOutInfo {
    std::string bestblock;
    int confirmations{0};
    int64_t value{0};
    TxOutScriptPubKey script_pub_key;
    bool coinbase{false};
};

struct NetworkInfoNetwork {
    std::string name;
    bool limited{false};
    bool reachable{false};
    std::string proxy;
    bool proxy_randomize_credentials{false};
};

struct NetworkInfoLocalAddress {
    std::string address;
    uint16_t port{0};
    int score{0};
};

struct NetworkInfo {
    int version{0};
    std::string subversion;
    int protocolversion{0};
    std::string localservices;
    std::vector<std::string> localservicesnames;
    bool localrelay{false};
    int64_t timeoffset{0};
    size_t connections{0};
    size_t connections_in{0};
    size_t connections_out{0};
    bool networkactive{false};
    std::vector<NetworkInfoNetwork> networks;
    int64_t relayfee{0};
    int64_t incrementalfee{0};
    std::vector<NetworkInfoLocalAddress> localaddresses;
    std::vector<std::string> warnings;
};

struct BIP9Statistics {
    int period{0};
    int threshold{0};
    int elapsed{0};
    int count{0};
    bool possible{false};
};

struct BIP9DeploymentInfo {
    int bit{-1};
    int64_t start_time{0};
    int64_t timeout{0};
    int min_activation_height{0};
    std::string status;
    int since{0};
    std::string status_next;
    std::optional<BIP9Statistics> statistics;
    std::string signalling;
};

struct DeploymentInfo {
    std::string name;
    std::string type;
    bool active{false};
    int height{-1};
    std::optional<BIP9DeploymentInfo> bip9;
};

struct DeploymentsInfo {
    std::string hash;
    int height{-1};
    std::vector<std::string> script_flags;
    std::vector<DeploymentInfo> deployments;
};

class NodeRpc
{
public:
    virtual ~NodeRpc() = default;

    virtual BlockchainInfo getBlockchainInfo() = 0;

    //! Hash of the active chain tip, hex-encoded. Mirrors getbestblockhash.
    virtual std::string getBestBlockHash() = 0;

    //! Hash of the active-chain block at the given height, hex-encoded. Throws
    //! if the height is out of range. Mirrors getblockhash.
    virtual std::string getBlockHash(int height) = 0;

    //! Verbose header info for the given block. Throws if the block is not
    //! found. Mirrors the verbose getblockheader.
    virtual BlockHeaderInfo getBlockHeader(const uint256& block_hash) = 0;

    //! Full block for the given hash, read from disk. Throws if the block is
    //! not found or its data is unavailable (pruned). Mirrors getblock with
    //! verbosity 0 (the block is serialized over the wire).
    virtual CBlock getBlock(const uint256& block_hash) = 0;

    //! Details about an unspent transaction output, or nullopt if the output is
    //! not found (spent or never existed). When include_mempool is set, outputs
    //! spent in the mempool are treated as unavailable. The value is in
    //! satoshis. Mirrors gettxout.
    virtual std::optional<TxOutInfo> getTxOut(const uint256& txid, uint32_t n, bool include_mempool) = 0;

    //! Estimate the fee rate (sat/kvB) needed for confirmation within
    //! conf_target blocks, mirroring the estimatesmartfee RPC (applies the
    //! mempool/relay fee floor and reports the target actually used).
    virtual SmartFeeEstimate estimateSmartFee(int conf_target, bool conservative) = 0;

    virtual NetworkInfo getNetworkInfo() = 0;

    virtual DeploymentsInfo getDeploymentInfo(const std::optional<uint256>& block_hash) = 0;
};

std::unique_ptr<NodeRpc> MakeNodeRpc(node::NodeContext& context);

} // namespace interfaces

#endif // BITCOIN_INTERFACES_NODERPC_H
