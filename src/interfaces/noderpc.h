// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_NODERPC_H
#define BITCOIN_INTERFACES_NODERPC_H

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

    virtual NetworkInfo getNetworkInfo() = 0;

    virtual DeploymentsInfo getDeploymentInfo(const std::optional<uint256>& block_hash) = 0;
};

std::unique_ptr<NodeRpc> MakeNodeRpc(node::NodeContext& context);

} // namespace interfaces

#endif // BITCOIN_INTERFACES_NODERPC_H
