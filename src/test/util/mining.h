// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_MINING_H
#define BITCOIN_TEST_UTIL_MINING_H

#include <interfaces/mining.h>
#include <node/mining_types.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/time.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CChainParams;
class COutPoint;
namespace node {
struct NodeContext;
} // namespace node

struct BlockTestResult {
    bool valid{false};
    std::string reason;
    std::string debug;
};

std::unique_ptr<interfaces::BlockTemplate> CreateNewBlock(interfaces::Mining& mining,
                                                          const node::BlockCreateOptions& options = {},
                                                          bool cooldown = false);
std::unique_ptr<interfaces::BlockTemplate> WaitNext(interfaces::BlockTemplate& block_template,
                                                    node::BlockWaitOptions options = {});
bool SubmitSolution(interfaces::BlockTemplate& block_template,
                    uint32_t version,
                    uint32_t timestamp,
                    uint32_t nonce,
                    CTransactionRef coinbase);
BlockTestResult CheckBlock(interfaces::Mining& mining, CBlock block, node::BlockCheckOptions options);
BlockTestResult SubmitBlock(interfaces::Mining& mining, CBlock block);
std::optional<interfaces::BlockRef> WaitTipChanged(interfaces::Mining& mining,
                                                   uint256 current_tip,
                                                   MillisecondsDouble timeout = MillisecondsDouble::max());

/** Create a blockchain, starting from genesis */
std::vector<std::shared_ptr<CBlock>> CreateBlockChain(size_t total_height, const CChainParams& params);

/** Returns the generated coin */
COutPoint MineBlock(node::NodeContext&,
                    const node::BlockCreateOptions& assembler_options);

/**
 * Returns the generated coin (or Null if the block was invalid).
 * It is recommended to call RegenerateCommitments before mining the block to avoid merkle tree mismatches.
 **/
COutPoint MineBlock(node::NodeContext&, std::shared_ptr<CBlock>& block);

/**
 * Returns the generated coin (or Null if the block was invalid).
 */
COutPoint ProcessBlock(node::NodeContext&, const std::shared_ptr<CBlock>& block);

/** Prepare a block to be mined */
std::shared_ptr<CBlock> PrepareBlock(node::NodeContext& node,
                                     const node::BlockCreateOptions& assembler_options);

/** RPC-like helper function, returns the generated coin */
COutPoint generatetoaddress(node::NodeContext&, const std::string& address);

#endif // BITCOIN_TEST_UTIL_MINING_H
