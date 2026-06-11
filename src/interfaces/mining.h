// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_MINING_H
#define BITCOIN_INTERFACES_MINING_H

#include <consensus/amount.h>
#include <interfaces/handler.h>
#include <interfaces/types.h>
#include <node/mining_types.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/result.h>
#include <util/time.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace node {
struct NodeContext;
} // namespace node

namespace interfaces {

//! Block template interface
class BlockTemplate
{
public:
    using NextTemplateFn = std::function<void(std::unique_ptr<BlockTemplate>)>;
    using SubmitSolutionFn = std::function<void(bool)>;

    virtual ~BlockTemplate() = default;

    virtual CBlockHeader getBlockHeader() = 0;
    // Block contains a dummy coinbase transaction that should not be used and
    // it may not match a transaction constructed from getCoinbaseTx().
    virtual CBlock getBlock() = 0;

    // Fees per transaction, not including coinbase transaction.
    virtual std::vector<CAmount> getTxFees() = 0;
    // Sigop cost per transaction, not including coinbase transaction.
    virtual std::vector<int64_t> getTxSigops() = 0;

    /** Return fields needed to construct a coinbase transaction */
    virtual node::CoinbaseTx getCoinbaseTx() = 0;

    /**
     * Compute merkle path to the coinbase transaction
     *
     * @return merkle path ordered from the deepest
     */
    virtual std::vector<uint256> getCoinbaseMerklePath() = 0;

    /**
     * Construct and broadcast the block. Modifies the template in place,
     * updating the fields listed below as well as the merkle root.
     *
     * @param[in] version version block header field
     * @param[in] timestamp time block header field (unix timestamp)
     * @param[in] nonce nonce block header field
     * @param[in] coinbase complete coinbase transaction (including witness)
     *
     * @note Unlike the submitblock RPC, this method does not call
     *       UpdateUncommittedBlockStructures to add a missing coinbase witness
     *       reserved value. Callers must provide a complete coinbase transaction,
     *       including the witness when a witness commitment is present.
     *
     * @note for heights <= 16, the BIP34 height push in getCoinbaseTx().script_sig_prefix
     *       is only one byte long, so the coinbase scriptSig needs at least
     *       one additional byte of data to avoid bad-cb-length.
     *
     * @returns if the block was processed, does not necessarily indicate validity.
     *
     * @note Returns true if the block is already known, which can happen if
     *       the solved block is constructed and broadcast by multiple nodes
     *       (e.g. both the miner who constructed the template and the pool).
     */
    /**
     * Construct and broadcast the block asynchronously. The returned handler
     * cancels the submission before the callback runs.
     */
    virtual std::unique_ptr<Handler> submitSolutionAsync(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase, SubmitSolutionFn fn) = 0;

    /**
     * Watches for fees in the next block to rise, a new tip or the timeout.
     *
     * @param[in] options   Control the timeout (default forever) and by how much total fees
     *                      for the next block should rise (default infinite).
     * @param[in] fn        Called with a new block template, or nullptr on
     *                      timeout or cancellation.
     *
     * @returns a handler that cancels the watch before the callback runs.
     *
     * On testnet this will additionally return a template with difficulty 1 if
     * the tip is more than 20 minutes old.
     */
    virtual std::unique_ptr<Handler> watchNext(node::BlockWaitOptions options, NextTemplateFn fn) = 0;
};

//! Interface giving clients (RPC, Stratum v2 Template Provider in the future)
//! ability to create block templates.
class Mining
{
public:
    using TipChangedFn = std::function<void(std::optional<BlockRef>)>;
    using CreateBlockResult = util::Result<std::unique_ptr<BlockTemplate>>;
    using CreateBlockFn = std::function<void(CreateBlockResult)>;
    using CheckBlockFn = std::function<void(bool, std::string, std::string)>;
    using SubmitBlockFn = std::function<void(bool, std::string, std::string)>;

    virtual ~Mining() = default;

    //! If this chain is exclusively used for testing
    virtual bool isTestChain() = 0;

    //! Returns whether IBD is still in progress.
    virtual bool isInitialBlockDownload() = 0;

    //! Returns the hash and height for the tip of this chain
    virtual std::optional<BlockRef> getTip() = 0;

    //! Watches for the connected tip to differ from current_tip. The returned
    //! handler cancels the watch before the callback runs.
    virtual std::unique_ptr<Handler> watchTip(uint256 current_tip, MillisecondsDouble timeout, TipChangedFn fn) = 0;

    /**
     * Construct a new block template asynchronously.
     *
     * @param[in] options options for creating the block
     * @param[in] cooldown wait for tip to be connected and IBD to complete.
     *                     If the best header is ahead of the tip, wait for the
     *                     tip to catch up. It's recommended to disable this on
     *                     regtest and signets with only one miner, as these
     *                     could stall.
     * @param[in] fn called with a block template result. A successful result
     *               may contain nullptr when the operation is cancelled or the
     *               node is shutting down.
     *
     * @returns a handler that cancels the operation before the callback runs.
     */
    virtual std::unique_ptr<Handler> createNewBlockAsync(const node::BlockCreateOptions& options, bool cooldown, CreateBlockFn fn) = 0;

    /**
     * Checks if a given block is valid asynchronously.
     *
     * @param[in] block       the block to check
     * @param[in] options     verification options: the proof-of-work check can be
     *                        skipped in order to verify a template generated by
     *                        external software.
     * @param[in] fn          called with validity, failure reason (BIP22), and
     *                        more detailed rejection reason.
     *
     * For signets the challenge verification is skipped when check_pow is false.
     */
    virtual std::unique_ptr<Handler> checkBlockAsync(CBlock block, node::BlockCheckOptions options, CheckBlockFn fn) = 0;

    /**
     * Process a fully assembled block asynchronously.
     *
     * Similar to the submitblock RPC. Accepts a complete block, validates it,
     * and if accepted as new, processes it into chainstate. Accepted blocks may
     * then be announced to peers through normal validation signals.
     *
     * @param[in] block the complete block to submit
     * @param[in] fn called with accepted, failure reason (BIP22), and more
     *               detailed rejection reason. Accepted is false for duplicate
     *               or inconclusive validation results.
     *
     * @note Unlike the submitblock RPC, this method does not call
     *       UpdateUncommittedBlockStructures to add a missing coinbase witness
     *       reserved value. Callers must submit a fully formed block, including
     *       the coinbase witness when a witness commitment is present.
     */
    virtual std::unique_ptr<Handler> submitBlockAsync(CBlock block, SubmitBlockFn fn) = 0;

    //! Get internal node context. Useful for RPC and testing,
    //! but not accessible across processes.
    virtual node::NodeContext* context() { return nullptr; }
};

//! Return implementation of Mining interface.
//!
//! @param[in] wait_loaded waits for chainstate data to be loaded before
//!                        returning. Used to prevent external clients from
//!                        being able to crash the node during startup.
std::unique_ptr<Mining> MakeMining(node::NodeContext& node, bool wait_loaded = true);

} // namespace interfaces

#endif // BITCOIN_INTERFACES_MINING_H
