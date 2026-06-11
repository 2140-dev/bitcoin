// Copyright (c) 2019-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/mining.h>

#include <block_validation.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <interfaces/mining.h>
#include <key_io.h>
#include <node/context.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/script.h>
#include <uint256.h>
#include <util/check.h>
#include <util/result.h>
#include <chainstate.h>
#include <validationinterface.h>
#include <versionbits.h>

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

using node::NodeContext;

namespace {
template <typename Result, typename Start>
Result WaitForAsync(Start&& start)
{
    std::promise<Result> promise;
    auto future{promise.get_future()};
    auto handler{start([&promise](Result result) mutable {
        promise.set_value(std::move(result));
    })};
    (void)handler;
    return future.get();
}
} // namespace

std::unique_ptr<interfaces::BlockTemplate> CreateNewBlock(interfaces::Mining& mining,
                                                          const node::BlockCreateOptions& options,
                                                          bool cooldown)
{
    auto result{WaitForAsync<interfaces::Mining::CreateBlockResult>([&](interfaces::Mining::CreateBlockFn fn) {
        return mining.createNewBlockAsync(options, cooldown, std::move(fn));
    })};
    if (!result) throw std::runtime_error{util::ErrorString(result).original};
    return std::move(*result);
}

std::unique_ptr<interfaces::BlockTemplate> WaitNext(interfaces::BlockTemplate& block_template,
                                                    node::BlockWaitOptions options)
{
    return WaitForAsync<std::unique_ptr<interfaces::BlockTemplate>>([&](interfaces::BlockTemplate::NextTemplateFn fn) {
        return block_template.watchNext(options, std::move(fn));
    });
}

bool SubmitSolution(interfaces::BlockTemplate& block_template,
                    uint32_t version,
                    uint32_t timestamp,
                    uint32_t nonce,
                    CTransactionRef coinbase)
{
    return WaitForAsync<bool>([&](interfaces::BlockTemplate::SubmitSolutionFn fn) {
        return block_template.submitSolutionAsync(version, timestamp, nonce, std::move(coinbase), std::move(fn));
    });
}

BlockTestResult CheckBlock(interfaces::Mining& mining, CBlock block, node::BlockCheckOptions options)
{
    return WaitForAsync<BlockTestResult>([&](std::function<void(BlockTestResult)> done) {
        return mining.checkBlockAsync(std::move(block), options, [done = std::move(done)](bool valid, std::string reason, std::string debug) mutable {
            done({valid, std::move(reason), std::move(debug)});
        });
    });
}

BlockTestResult SubmitBlock(interfaces::Mining& mining, CBlock block)
{
    return WaitForAsync<BlockTestResult>([&](std::function<void(BlockTestResult)> done) {
        return mining.submitBlockAsync(std::move(block), [done = std::move(done)](bool accepted, std::string reason, std::string debug) mutable {
            done({accepted, std::move(reason), std::move(debug)});
        });
    });
}

std::optional<interfaces::BlockRef> WaitTipChanged(interfaces::Mining& mining,
                                                   uint256 current_tip,
                                                   MillisecondsDouble timeout)
{
    return WaitForAsync<std::optional<interfaces::BlockRef>>([&](interfaces::Mining::TipChangedFn fn) {
        return mining.watchTip(current_tip, timeout, std::move(fn));
    });
}

COutPoint generatetoaddress(NodeContext& node, const std::string& address)
{
    const auto dest = DecodeDestination(address);
    assert(IsValidDestination(dest));
    return MineBlock(node, {
        .coinbase_output_script = GetScriptForDestination(dest),
    });
}

std::vector<std::shared_ptr<CBlock>> CreateBlockChain(size_t total_height, const CChainParams& params)
{
    std::vector<std::shared_ptr<CBlock>> ret{total_height};
    auto time{params.GenesisBlock().nTime};
    // NOTE: here `height` does not correspond to the block height but the block height - 1.
    for (size_t height{0}; height < total_height; ++height) {
        CBlock& block{*(ret.at(height) = std::make_shared<CBlock>())};

        CMutableTransaction coinbase_tx;
        coinbase_tx.nLockTime = static_cast<uint32_t>(height);
        coinbase_tx.vin.resize(1);
        coinbase_tx.vin[0].prevout.SetNull();
        coinbase_tx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL; // Make sure timelock is enforced.
        coinbase_tx.vout.resize(1);
        coinbase_tx.vout[0].scriptPubKey = P2WSH_OP_TRUE;
        coinbase_tx.vout[0].nValue = GetBlockSubsidy(height + 1, params.GetConsensus());
        // Always include OP_0 as a dummy extraNonce.
        coinbase_tx.vin[0].scriptSig = CScript() << (height + 1) << OP_0;
        block.vtx = {MakeTransactionRef(std::move(coinbase_tx))};

        block.nVersion = VERSIONBITS_LAST_OLD_BLOCK_VERSION;
        block.hashPrevBlock = (height >= 1 ? *ret.at(height - 1) : params.GenesisBlock()).GetHash();
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nTime = ++time;
        block.nBits = params.GenesisBlock().nBits;
        block.nNonce = 0;

        while (!CheckProofOfWork(block.GetHash(), block.nBits, params.GetConsensus())) {
            ++block.nNonce;
            assert(block.nNonce);
        }
    }
    return ret;
}

COutPoint MineBlock(NodeContext& node, const node::BlockCreateOptions& assembler_options)
{
    auto block = PrepareBlock(node, assembler_options);
    auto valid = MineBlock(node, block);
    assert(!valid.IsNull());
    return valid;
}

struct BlockValidationStateCatcher : public CValidationInterface {
    const uint256 m_hash;
    std::optional<BlockValidationState> m_state;

    BlockValidationStateCatcher(const uint256& hash)
        : m_hash{hash},
          m_state{} {}

protected:
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& state) override
    {
        if (block->GetHash() != m_hash) return;
        m_state = state;
    }
};

COutPoint MineBlock(NodeContext& node, std::shared_ptr<CBlock>& block)
{
    while (!CheckProofOfWork(block->GetHash(), block->nBits, Params().GetConsensus())) {
        ++block->nNonce;
        assert(block->nNonce);
    }

    return ProcessBlock(node, block);
}

COutPoint ProcessBlock(NodeContext& node, const std::shared_ptr<CBlock>& block)
{
    auto& chainman{*Assert(node.chainman)};
    const auto old_height = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveHeight());
    bool new_block;
    BlockValidationStateCatcher bvsc{block->GetHash()};
    node.validation_signals->RegisterValidationInterface(&bvsc);
    const bool processed{ProcessNewBlock(chainman, block, true, true, &new_block)};
    const bool duplicate{!new_block && processed};
    assert(!duplicate);
    node.validation_signals->UnregisterValidationInterface(&bvsc);
    node.validation_signals->SyncWithValidationInterfaceQueue();
    const bool was_valid{bvsc.m_state && bvsc.m_state->IsValid()};
    assert(old_height + was_valid == WITH_LOCK(chainman.GetMutex(), return chainman.ActiveHeight()));

    if (was_valid) return {block->vtx[0]->GetHash(), 0};
    return {};
}

std::shared_ptr<CBlock> PrepareBlock(NodeContext& node,
                                     const node::BlockCreateOptions& assembler_options)
{
    auto mining = interfaces::MakeMining(node);
    auto block_template = CreateNewBlock(*mining, assembler_options, /*cooldown=*/false);
    auto block = std::make_shared<CBlock>(Assert(block_template)->getBlock());

    LOCK(cs_main);
    block->nTime = Assert(node.chainman)->ActiveChain().Tip()->GetMedianTimePast() + 1;
    block->hashMerkleRoot = BlockMerkleRoot(*block);

    return block;
}
