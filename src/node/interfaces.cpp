// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <banman.h>
#include <block_validation.h>
#include <btcsignals.h>
#include <chain.h>
#include <chainparams.h>
#include <chainstate.h>
#include <coins.h>
#include <common/args.h>
#include <common/settings.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <init.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/types.h>
#include <kernel/context.h>
#include <key.h>
#include <logging.h>
#include <mapport.h>
#include <net.h>
#include <net_processing.h>
#include <net_types.h>
#include <netaddress.h>
#include <netbase.h>
#include <node/blockstorage.h>
#include <node/coin.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <node/kernel_notifications.h>
#include <node/miner.h>
#include <node/mini_miner.h>
#include <node/mining_args.h>
#include <node/mining_types.h>
#include <node/transaction.h>
#include <node/types.h>
#include <node/warnings.h>
#include <policy/feerate.h>
#include <policy/fees/block_policy_estimator.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <scheduler.h>
#include <sync.h>
#include <txmempool.h>
#include <uint256.h>
#include <univalue.h>
#include <util/check.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <validationinterface.h>

#include <algorithm>
#include <atomic>
#include <compare>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using interfaces::BlockRef;
using interfaces::BlockTemplate;
using interfaces::BlockTip;
using interfaces::Chain;
using interfaces::FoundBlock;
using interfaces::Handler;
using interfaces::MakeCleanupHandler;
using interfaces::MakeSignalHandler;
using interfaces::Mining;
using interfaces::Node;
using node::BlockAssembler;
using node::BlockCreateOptions;
using node::BlockWaitOptions;
using node::CoinbaseTx;
using util::Join;

namespace node {
// All members of the classes in this namespace are intentionally public, as the
// classes themselves are private.
namespace {
class NodeImpl : public Node
{
public:
    explicit NodeImpl(NodeContext& context) { setContext(&context); }
    void initLogging() override { InitLogging(args()); }
    void initParameterInteraction() override { InitParameterInteraction(args()); }
    bilingual_str getWarnings() override { return Join(Assert(m_context->warnings)->GetMessages(), Untranslated("<hr />")); }
    int getExitStatus() override { return Assert(m_context)->exit_status.load(); }
    BCLog::CategoryMask getLogCategories() override { return LogInstance().GetCategoryMask(); }
    bool baseInitialize() override
    {
        if (!AppInitBasicSetup(args(), Assert(context())->exit_status)) return false;
        if (!AppInitParameterInteraction(args())) return false;

        m_context->warnings = std::make_unique<node::Warnings>();
        m_context->kernel = std::make_unique<kernel::Context>();
        m_context->ecc_context = std::make_unique<ECC_Context>();
        if (!AppInitSanityChecks(*m_context->kernel)) return false;

        if (!AppInitLockDirectories()) return false;
        if (!AppInitInterfaces(*m_context)) return false;

        return true;
    }
    bool appInitMain(interfaces::BlockAndHeaderTipInfo* tip_info) override
    {
        if (AppInitMain(*m_context, tip_info)) return true;
        // Error during initialization, set exit status before continue
        m_context->exit_status.store(EXIT_FAILURE);
        return false;
    }
    void appShutdown() override
    {
        Shutdown(*m_context);
    }
    void startShutdown() override
    {
        NodeContext& ctx{*Assert(m_context)};
        if (!(Assert(ctx.shutdown_request))()) {
            LogError("Failed to send shutdown signal\n");
        }
        Interrupt(*m_context);
    }
    bool shutdownRequested() override { return ShutdownRequested(*Assert(m_context)); };
    bool isSettingIgnored(const std::string& name) override
    {
        bool ignored = false;
        args().LockSettings([&](common::Settings& settings) {
            if (auto* options = common::FindKey(settings.command_line_options, name)) {
                ignored = !options->empty();
            }
        });
        return ignored;
    }
    common::SettingsValue getPersistentSetting(const std::string& name) override { return args().GetPersistentSetting(name); }
    void updateRwSetting(const std::string& name, const common::SettingsValue& value) override
    {
        args().LockSettings([&](common::Settings& settings) {
            if (value.isNull()) {
                settings.rw_settings.erase(name);
            } else {
                settings.rw_settings[name] = value;
            }
        });
        args().WriteSettingsFile();
    }
    void forceSetting(const std::string& name, const common::SettingsValue& value) override
    {
        args().LockSettings([&](common::Settings& settings) {
            if (value.isNull()) {
                settings.forced_settings.erase(name);
            } else {
                settings.forced_settings[name] = value;
            }
        });
    }
    void resetSettings() override
    {
        args().WriteSettingsFile(/*errors=*/nullptr, /*backup=*/true);
        args().LockSettings([&](common::Settings& settings) {
            settings.rw_settings.clear();
        });
        args().WriteSettingsFile();
    }
    void mapPort(bool enable) override { StartMapPort(enable); }
    std::optional<Proxy> getProxy(Network net) override { return GetProxy(net); }
    size_t getNodeCount(ConnectionDirection flags) override
    {
        return m_context->connman ? m_context->connman->GetNodeCount(flags) : 0;
    }
    bool getNodesStats(NodesStats& stats) override
    {
        stats.clear();

        if (m_context->connman) {
            std::vector<CNodeStats> stats_temp;
            m_context->connman->GetNodeStats(stats_temp);

            stats.reserve(stats_temp.size());
            for (auto& node_stats_temp : stats_temp) {
                stats.emplace_back(std::move(node_stats_temp), false, CNodeStateStats());
            }

            // Try to retrieve the CNodeStateStats for each node.
            if (m_context->peerman) {
                TRY_LOCK(::cs_main, lockMain);
                if (lockMain) {
                    for (auto& node_stats : stats) {
                        std::get<1>(node_stats) =
                            m_context->peerman->GetNodeStateStats(std::get<0>(node_stats).nodeid, std::get<2>(node_stats));
                    }
                }
            }
            return true;
        }
        return false;
    }
    bool getBanned(banmap_t& banmap) override
    {
        if (m_context->banman) {
            m_context->banman->GetBanned(banmap);
            return true;
        }
        return false;
    }
    bool ban(const CNetAddr& net_addr, int64_t ban_time_offset) override
    {
        if (m_context->banman) {
            m_context->banman->Ban(net_addr, ban_time_offset);
            return true;
        }
        return false;
    }
    bool unban(const CSubNet& ip) override
    {
        if (m_context->banman) {
            m_context->banman->Unban(ip);
            return true;
        }
        return false;
    }
    bool disconnectByAddress(const CNetAddr& net_addr) override
    {
        if (m_context->connman) {
            return m_context->connman->DisconnectNode(net_addr);
        }
        return false;
    }
    bool disconnectById(NodeId id) override
    {
        if (m_context->connman) {
            return m_context->connman->DisconnectNode(id);
        }
        return false;
    }
    int64_t getTotalBytesRecv() override { return m_context->connman ? m_context->connman->GetTotalBytesRecv() : 0; }
    int64_t getTotalBytesSent() override { return m_context->connman ? m_context->connman->GetTotalBytesSent() : 0; }
    size_t getMempoolSize() override { return m_context->mempool ? m_context->mempool->size() : 0; }
    size_t getMempoolDynamicUsage() override { return m_context->mempool ? m_context->mempool->DynamicMemoryUsage() : 0; }
    size_t getMempoolMaxUsage() override { return m_context->mempool ? m_context->mempool->m_opts.max_size_bytes : 0; }
    bool getHeaderTip(int& height, int64_t& block_time) override
    {
        LOCK(::cs_main);
        auto best_header = chainman().m_best_header;
        if (best_header) {
            height = best_header->nHeight;
            block_time = best_header->GetBlockTime();
            return true;
        }
        return false;
    }
    std::map<CNetAddr, LocalServiceInfo> getNetLocalAddresses() override
    {
        if (m_context->connman)
            return m_context->connman->getNetLocalAddresses();
        else
            return {};
    }
    int getNumBlocks() override
    {
        LOCK(::cs_main);
        return chainman().ActiveChain().Height();
    }
    uint256 getBestBlockHash() override
    {
        const CBlockIndex* tip = WITH_LOCK(::cs_main, return chainman().ActiveChain().Tip());
        return tip ? tip->GetBlockHash() : chainman().GetParams().GenesisBlock().GetHash();
    }
    int64_t getLastBlockTime() override
    {
        LOCK(::cs_main);
        if (chainman().ActiveChain().Tip()) {
            return chainman().ActiveChain().Tip()->GetBlockTime();
        }
        return chainman().GetParams().GenesisBlock().GetBlockTime(); // Genesis block's time of current network
    }
    double getVerificationProgress() override
    {
        LOCK(chainman().GetMutex());
        return chainman().GuessVerificationProgress(chainman().ActiveTip());
    }
    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }
    bool isLoadingBlocks() override { return chainman().m_blockman.LoadingBlocks(); }
    void setNetworkActive(bool active) override
    {
        if (m_context->connman) {
            m_context->connman->SetNetworkActive(active);
        }
    }
    bool getNetworkActive() override { return m_context->connman && m_context->connman->GetNetworkActive(); }
    CFeeRate getDustRelayFee() override
    {
        if (!m_context->mempool) return CFeeRate{DUST_RELAY_TX_FEE};
        return m_context->mempool->m_opts.dust_relay_feerate;
    }
    std::optional<Coin> getUnspentOutput(const COutPoint& output) override
    {
        LOCK(::cs_main);
        return chainman().ActiveChainstate().CoinsTip().GetCoin(output);
    }
    TransactionError broadcastTransaction(CTransactionRef tx, CAmount max_tx_fee, std::string& err_string) override
    {
        return BroadcastTransaction(*m_context,
                                    std::move(tx),
                                    err_string,
                                    max_tx_fee,
                                    TxBroadcast::MEMPOOL_AND_BROADCAST_TO_ALL,
                                    /*wait_callback=*/false);
    }
    std::unique_ptr<Handler> handleInitMessage(InitMessageFn fn) override
    {
        return MakeSignalHandler(::uiInterface.InitMessage_connect(fn));
    }
    std::unique_ptr<Handler> handleMessageBox(MessageBoxFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ThreadSafeMessageBox_connect(fn));
    }
    std::unique_ptr<Handler> handleQuestion(QuestionFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ThreadSafeQuestion_connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ShowProgress_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyNumConnectionsChanged(NotifyNumConnectionsChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyNumConnectionsChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyNetworkActiveChanged(NotifyNetworkActiveChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyNetworkActiveChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyAlertChanged(NotifyAlertChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyAlertChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleBannedListChanged(BannedListChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.BannedListChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyBlockTip(NotifyBlockTipFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyBlockTip_connect([fn](SynchronizationState sync_state, const CBlockIndex& block, double verification_progress) {
            fn(sync_state, BlockTip{block.nHeight, block.GetBlockTime(), block.GetBlockHash()}, verification_progress);
        }));
    }
    std::unique_ptr<Handler> handleNotifyHeaderTip(NotifyHeaderTipFn fn) override
    {
        return MakeSignalHandler(
            ::uiInterface.NotifyHeaderTip_connect([fn](SynchronizationState sync_state, int64_t height, int64_t timestamp, bool presync) {
                fn(sync_state, BlockTip{(int)height, timestamp, uint256{}}, presync);
            }));
    }
    NodeContext* context() override { return m_context; }
    void setContext(NodeContext* context) override
    {
        m_context = context;
    }
    ArgsManager& args() { return *Assert(Assert(m_context)->args); }
    ChainstateManager& chainman() { return *Assert(m_context->chainman); }
    NodeContext* m_context{nullptr};
};

// NOLINTNEXTLINE(misc-no-recursion)
bool FillBlock(const CBlockIndex* index, const FoundBlock& block, UniqueLock<RecursiveMutex>& lock, const CChain& active, const BlockManager& blockman) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (!index) return false;
    if (block.m_hash) *block.m_hash = index->GetBlockHash();
    if (block.m_height) *block.m_height = index->nHeight;
    if (block.m_time) *block.m_time = index->GetBlockTime();
    if (block.m_max_time) *block.m_max_time = index->GetBlockTimeMax();
    if (block.m_mtp_time) *block.m_mtp_time = index->GetMedianTimePast();
    if (block.m_in_active_chain) *block.m_in_active_chain = active[index->nHeight] == index;
    if (block.m_locator) { *block.m_locator = GetLocator(index); }
    if (block.m_next_block) FillBlock(active[index->nHeight] == index ? active[index->nHeight + 1] : nullptr, *block.m_next_block, lock, active, blockman);
    if (block.m_data) {
        REVERSE_LOCK(lock, cs_main);
        if (!blockman.ReadBlock(*block.m_data, *index)) block.m_data->SetNull();
    }
    block.found = true;
    return true;
}

class NotificationsProxy : public CValidationInterface
{
public:
    explicit NotificationsProxy(std::shared_ptr<Chain::Notifications> notifications)
        : m_notifications(std::move(notifications)) {}
    virtual ~NotificationsProxy() = default;
    void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t mempool_sequence) override
    {
        m_notifications->transactionAddedToMempool(tx.info.m_tx);
    }
    void TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason, uint64_t mempool_sequence) override
    {
        m_notifications->transactionRemovedFromMempool(tx, reason);
    }
    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* index) override
    {
        m_notifications->blockConnected(kernel::MakeBlockInfo(index, block.get()));
    }
    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* index) override
    {
        m_notifications->blockDisconnected(kernel::MakeBlockInfo(index, block.get()));
    }
    void UpdatedBlockTip(const CBlockIndex* index, const CBlockIndex* fork_index, bool is_ibd) override
    {
        m_notifications->updatedBlockTip();
    }
    void ChainStateFlushed(const CBlockLocator& locator) override
    {
        m_notifications->chainStateFlushed(locator);
    }
    std::shared_ptr<Chain::Notifications> m_notifications;
};

class NotificationsHandlerImpl : public Handler
{
public:
    explicit NotificationsHandlerImpl(ValidationSignals& signals, std::shared_ptr<Chain::Notifications> notifications)
        : m_signals{signals}, m_proxy{std::make_shared<NotificationsProxy>(std::move(notifications))}
    {
        m_signals.RegisterSharedValidationInterface(m_proxy);
    }
    ~NotificationsHandlerImpl() override { disconnect(); }
    void disconnect() override
    {
        if (m_proxy) {
            m_signals.UnregisterSharedValidationInterface(m_proxy);
            m_proxy.reset();
        }
    }
    ValidationSignals& m_signals;
    std::shared_ptr<NotificationsProxy> m_proxy;
};

class ChainImpl : public Chain
{
public:
    explicit ChainImpl(NodeContext& node) : m_node(node) {}
    std::optional<int> getHeight() override
    {
        const int height{WITH_LOCK(::cs_main, return chainman().ActiveChain().Height())};
        return height >= 0 ? std::optional{height} : std::nullopt;
    }
    uint256 getBlockHash(int height) override
    {
        LOCK(::cs_main);
        return Assert(chainman().ActiveChain()[height])->GetBlockHash();
    }
    bool haveBlockOnDisk(int height) override
    {
        LOCK(::cs_main);
        const CBlockIndex* block{chainman().ActiveChain()[height]};
        return block && ((block->nStatus & BLOCK_HAVE_DATA) != 0) && block->nTx > 0;
    }
    std::optional<int> findLocatorFork(const CBlockLocator& locator) override
    {
        LOCK(::cs_main);
        if (const CBlockIndex* fork = chainman().ActiveChainstate().FindForkInGlobalIndex(locator)) {
            return fork->nHeight;
        }
        return std::nullopt;
    }
    bool findBlock(const uint256& hash, const FoundBlock& block) override
    {
        WAIT_LOCK(cs_main, lock);
        return FillBlock(chainman().m_blockman.LookupBlockIndex(hash), block, lock, chainman().ActiveChain(), chainman().m_blockman);
    }
    bool findFirstBlockWithTimeAndHeight(int64_t min_time, int min_height, const FoundBlock& block) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        return FillBlock(active.FindEarliestAtLeast(min_time, min_height), block, lock, active, chainman().m_blockman);
    }
    bool findAncestorByHeight(const uint256& block_hash, int ancestor_height, const FoundBlock& ancestor_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        if (const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash)) {
            if (const CBlockIndex* ancestor = block->GetAncestor(ancestor_height)) {
                return FillBlock(ancestor, ancestor_out, lock, active, chainman().m_blockman);
            }
        }
        return FillBlock(nullptr, ancestor_out, lock, active, chainman().m_blockman);
    }
    bool findAncestorByHash(const uint256& block_hash, const uint256& ancestor_hash, const FoundBlock& ancestor_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash);
        const CBlockIndex* ancestor = chainman().m_blockman.LookupBlockIndex(ancestor_hash);
        if (block && ancestor && block->GetAncestor(ancestor->nHeight) != ancestor) ancestor = nullptr;
        return FillBlock(ancestor, ancestor_out, lock, chainman().ActiveChain(), chainman().m_blockman);
    }
    bool findCommonAncestor(const uint256& block_hash1, const uint256& block_hash2, const FoundBlock& ancestor_out, const FoundBlock& block1_out, const FoundBlock& block2_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        const CBlockIndex* block1 = chainman().m_blockman.LookupBlockIndex(block_hash1);
        const CBlockIndex* block2 = chainman().m_blockman.LookupBlockIndex(block_hash2);
        const CBlockIndex* ancestor = block1 && block2 ? LastCommonAncestor(block1, block2) : nullptr;
        // Using & instead of && below to avoid short circuiting and leaving
        // output uninitialized. Cast bool to int to avoid -Wbitwise-instead-of-logical
        // compiler warnings.
        return int{FillBlock(ancestor, ancestor_out, lock, active, chainman().m_blockman)} &
               int{FillBlock(block1, block1_out, lock, active, chainman().m_blockman)} &
               int{FillBlock(block2, block2_out, lock, active, chainman().m_blockman)};
    }
    void findCoins(std::map<COutPoint, Coin>& coins) override { return FindCoins(m_node, coins); }
    double guessVerificationProgress(const uint256& block_hash) override
    {
        LOCK(chainman().GetMutex());
        return chainman().GuessVerificationProgress(chainman().m_blockman.LookupBlockIndex(block_hash));
    }
    bool hasBlocks(const uint256& block_hash, int min_height, std::optional<int> max_height) override
    {
        // hasBlocks returns true if all ancestors of block_hash in specified
        // range have block data (are not pruned), false if any ancestors in
        // specified range are missing data.
        //
        // For simplicity and robustness, min_height and max_height are only
        // used to limit the range, and passing min_height that's too low or
        // max_height that's too high will not crash or change the result.
        LOCK(::cs_main);
        if (const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash)) {
            if (max_height && block->nHeight >= *max_height) block = block->GetAncestor(*max_height);
            for (; block->nStatus & BLOCK_HAVE_DATA; block = block->pprev) {
                // Check pprev to not segfault if min_height is too low
                if (block->nHeight <= min_height || !block->pprev) return true;
            }
        }
        return false;
    }
    RBFTransactionState isRBFOptIn(const CTransaction& tx) override
    {
        if (!m_node.mempool) return IsRBFOptInEmptyMempool(tx);
        LOCK(m_node.mempool->cs);
        return IsRBFOptIn(tx, *m_node.mempool);
    }
    bool isInMempool(const Txid& txid) override
    {
        if (!m_node.mempool) return false;
        return m_node.mempool->exists(txid);
    }
    bool hasDescendantsInMempool(const Txid& txid) override
    {
        if (!m_node.mempool) return false;
        return m_node.mempool->HasDescendants(txid);
    }
    bool broadcastTransaction(const CTransactionRef& tx,
        const CAmount& max_tx_fee,
        TxBroadcast broadcast_method,
        std::string& err_string) override
    {
        const TransactionError err = BroadcastTransaction(m_node, tx, err_string, max_tx_fee, broadcast_method, /*wait_callback=*/false);
        // Chain clients only care about failures to accept the tx to the mempool. Disregard non-mempool related failures.
        // Note: this will need to be updated if BroadcastTransactions() is updated to return other non-mempool failures
        // that Chain clients do not need to know about.
        return TransactionError::OK == err;
    }
    void getTransactionAncestry(const Txid& txid, size_t& ancestors, size_t& cluster_count, size_t* ancestorsize, CAmount* ancestorfees) override
    {
        ancestors = cluster_count = 0;
        if (!m_node.mempool) return;
        m_node.mempool->GetTransactionAncestry(txid, ancestors, cluster_count, ancestorsize, ancestorfees);
    }

    std::map<COutPoint, CAmount> calculateIndividualBumpFees(const std::vector<COutPoint>& outpoints, const CFeeRate& target_feerate) override
    {
        if (!m_node.mempool) {
            std::map<COutPoint, CAmount> bump_fees;
            for (const auto& outpoint : outpoints) {
                bump_fees.emplace(outpoint, 0);
            }
            return bump_fees;
        }
        return MiniMiner(*m_node.mempool, outpoints).CalculateBumpFees(target_feerate);
    }

    std::optional<CAmount> calculateCombinedBumpFee(const std::vector<COutPoint>& outpoints, const CFeeRate& target_feerate) override
    {
        if (!m_node.mempool) {
            return 0;
        }
        return MiniMiner(*m_node.mempool, outpoints).CalculateTotalBumpFees(target_feerate);
    }
    void getPackageLimits(unsigned int& limit_ancestor_count, unsigned int& limit_descendant_count) override
    {
        const CTxMemPool::Limits default_limits{};

        const CTxMemPool::Limits& limits{m_node.mempool ? m_node.mempool->m_opts.limits : default_limits};

        limit_ancestor_count = limits.ancestor_count;
        limit_descendant_count = limits.descendant_count;
    }
    util::Result<void> checkChainLimits(const CTransactionRef& tx) override
    {
        if (!m_node.mempool) return {};
        if (!m_node.mempool->CheckPolicyLimits(tx)) {
            return util::Error{Untranslated("too many unconfirmed transactions in cluster")};
        }
        return {};
    }
    CFeeRate estimateSmartFee(int num_blocks, bool conservative, FeeCalculation* calc) override
    {
        if (!m_node.fee_estimator) return {};
        return m_node.fee_estimator->estimateSmartFee(num_blocks, calc, conservative);
    }
    unsigned int estimateMaxBlocks() override
    {
        if (!m_node.fee_estimator) return 0;
        return m_node.fee_estimator->HighestTargetTracked(FeeEstimateHorizon::LONG_HALFLIFE);
    }
    CFeeRate mempoolMinFee() override
    {
        if (!m_node.mempool) return {};
        return m_node.mempool->GetMinFee();
    }
    CFeeRate relayMinFee() override
    {
        if (!m_node.mempool) return CFeeRate{DEFAULT_MIN_RELAY_TX_FEE};
        return m_node.mempool->m_opts.min_relay_feerate;
    }
    CFeeRate relayIncrementalFee() override
    {
        if (!m_node.mempool) return CFeeRate{DEFAULT_INCREMENTAL_RELAY_FEE};
        return m_node.mempool->m_opts.incremental_relay_feerate;
    }
    CFeeRate relayDustFee() override
    {
        if (!m_node.mempool) return CFeeRate{DUST_RELAY_TX_FEE};
        return m_node.mempool->m_opts.dust_relay_feerate;
    }
    bool havePruned() override
    {
        LOCK(::cs_main);
        return chainman().m_blockman.m_have_pruned;
    }
    std::optional<int> getPruneHeight() override
    {
        LOCK(chainman().GetMutex());
        const CChain& chain{chainman().ActiveChain()};
        const CBlockIndex* first_block{chain[1]};
        const CBlockIndex* chain_tip{chain.Tip()};
        if (!first_block || !chain_tip) return std::nullopt;
        if ((chain_tip->nStatus & BLOCK_HAVE_MASK) != BLOCK_HAVE_MASK) return chain_tip->nHeight;
        const auto& first_unpruned{chainman().m_blockman.GetFirstBlock(*chain_tip, BLOCK_HAVE_MASK, first_block)};
        if (&first_unpruned == first_block) return std::nullopt;
        return CHECK_NONFATAL(first_unpruned.pprev)->nHeight;
    }
    bool isReadyToBroadcast() override { return !chainman().m_blockman.LoadingBlocks() && !isInitialBlockDownload(); }
    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }
    bool shutdownRequested() override { return ShutdownRequested(m_node); }
    void initMessage(const std::string& message) override { ::uiInterface.InitMessage(message); }
    void initWarning(const bilingual_str& message) override { InitWarning(message); }
    void initError(const bilingual_str& message) override { InitError(message); }
    void showProgress(const std::string& title, int progress, bool resume_possible) override
    {
        ::uiInterface.ShowProgress(title, progress, resume_possible);
    }
    std::unique_ptr<Handler> handleNotifications(std::shared_ptr<Notifications> notifications) override
    {
        return std::make_unique<NotificationsHandlerImpl>(validation_signals(), std::move(notifications));
    }
    void waitForNotificationsIfTipChanged(const uint256& old_tip) override
    {
        if (!old_tip.IsNull() && old_tip == WITH_LOCK(::cs_main, return chainman().ActiveChain().Tip()->GetBlockHash())) return;
        validation_signals().SyncWithValidationInterfaceQueue();
    }
    void waitForNotifications() override
    {
        validation_signals().SyncWithValidationInterfaceQueue();
    }
    common::SettingsValue getSetting(const std::string& name) override
    {
        return args().GetSetting(name);
    }
    std::vector<common::SettingsValue> getSettingsList(const std::string& name) override
    {
        return args().GetSettingsList(name);
    }
    common::SettingsValue getRwSetting(const std::string& name) override
    {
        common::SettingsValue result;
        args().LockSettings([&](const common::Settings& settings) {
            if (const common::SettingsValue* value = common::FindKey(settings.rw_settings, name)) {
                result = *value;
            }
        });
        return result;
    }
    bool updateRwSetting(const std::string& name,
                         const interfaces::SettingsUpdate& update_settings_func) override
    {
        std::optional<interfaces::SettingsAction> action;
        args().LockSettings([&](common::Settings& settings) {
            if (auto* value = common::FindKey(settings.rw_settings, name)) {
                action = update_settings_func(*value);
                if (value->isNull()) settings.rw_settings.erase(name);
            } else {
                UniValue new_value;
                action = update_settings_func(new_value);
                if (!new_value.isNull()) settings.rw_settings[name] = std::move(new_value);
            }
        });
        if (!action) return false;
        // Now dump value to disk if requested
        return *action != interfaces::SettingsAction::WRITE || args().WriteSettingsFile();
    }
    bool overwriteRwSetting(const std::string& name, common::SettingsValue value, interfaces::SettingsAction action) override
    {
        return updateRwSetting(name, [&](common::SettingsValue& settings) {
            settings = std::move(value);
            return action;
        });
    }
    bool deleteRwSettings(const std::string& name, interfaces::SettingsAction action) override
    {
        return overwriteRwSetting(name, {}, action);
    }
    void requestMempoolTransactions(Notifications& notifications) override
    {
        if (!m_node.mempool) return;
        LOCK2(::cs_main, m_node.mempool->cs);
        for (const CTxMemPoolEntry& entry : m_node.mempool->entryAll()) {
            notifications.transactionAddedToMempool(entry.GetSharedTx());
        }
    }
    NodeContext* context() override { return &m_node; }
    ArgsManager& args() { return *Assert(m_node.args); }
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    ValidationSignals& validation_signals() { return *Assert(m_node.validation_signals); }
    NodeContext& m_node;
};

class MiningTaskRunner
{
    struct TaskState {
        std::atomic_bool cancelled{false};
        std::function<void()> cancel;
    };

    struct Task {
        std::shared_ptr<TaskState> state;
        std::function<void(std::atomic_bool& cancelled)> task;
    };

public:
    explicit MiningTaskRunner(size_t worker_count = 1)
    {
        worker_count = std::max<size_t>(1, worker_count);
        m_workers.reserve(worker_count);
        for (size_t i{0}; i < worker_count; ++i) {
            m_workers.emplace_back([this] { WorkLoop(); });
        }
    }

    ~MiningTaskRunner()
    {
        std::vector<std::shared_ptr<TaskState>> states;
        {
            std::lock_guard lock{m_mutex};
            m_stop = true;
            states = m_states;
        }
        for (const auto& state : states) {
            Cancel(*state);
        }
        m_cv.notify_all();
        for (auto& worker : m_workers) {
            if (worker.joinable()) worker.join();
        }
    }

    std::unique_ptr<Handler> schedule(std::function<void(std::atomic_bool& cancelled)> task, std::function<void()> cancel = {})
    {
        auto state{std::make_shared<TaskState>()};
        state->cancel = std::move(cancel);
        {
            std::lock_guard lock{m_mutex};
            if (m_stop) {
                state->cancelled.store(true, std::memory_order_release);
            } else {
                m_states.push_back(state);
                m_tasks.push_back({state, std::move(task)});
            }
        }
        m_cv.notify_one();
        return MakeCleanupHandler([state] {
            Cancel(*state);
        });
    }

private:
    static void Cancel(TaskState& state) noexcept
    {
        if (state.cancelled.exchange(true, std::memory_order_acq_rel) || !state.cancel) return;
        try {
            state.cancel();
        } catch (const std::exception& e) {
            LogError("Mining task cancellation failed: %s\n", e.what());
        } catch (...) {
            LogError("Mining task cancellation failed with an unknown exception\n");
        }
    }

    void WorkLoop()
    {
        while (true) {
            Task task;
            {
                std::unique_lock lock{m_mutex};
                m_cv.wait(lock, [&] { return m_stop || !m_tasks.empty(); });
                if (m_stop && m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            if (!task.state->cancelled.load(std::memory_order_acquire)) {
                try {
                    task.task(task.state->cancelled);
                } catch (const std::exception& e) {
                    LogError("Mining task failed: %s\n", e.what());
                } catch (...) {
                    LogError("Mining task failed with an unknown exception\n");
                }
            }
            {
                std::lock_guard lock{m_mutex};
                std::erase(m_states, task.state);
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Task> m_tasks;
    std::vector<std::shared_ptr<TaskState>> m_states;
    std::vector<std::thread> m_workers;
    bool m_stop{false};
};

CAmount TotalTemplateFees(const CBlockTemplate& block_template)
{
    return std::accumulate(block_template.vTxFees.begin(), block_template.vTxFees.end(), CAmount{0});
}

bool ShouldCreateMinDifficultyTemplate(ChainstateManager& chainman)
{
    if (!chainman.GetParams().GetConsensus().fPowAllowMinDifficultyBlocks) return false;
    LOCK(::cs_main);
    const CBlockIndex* tip{chainman.ActiveChain().Tip()};
    return tip && NodeClock::now() > NodeClock::time_point{std::chrono::seconds{tip->GetBlockTime()}} + std::chrono::minutes{20};
}

class BlockTemplateImpl : public BlockTemplate
{
public:
    explicit BlockTemplateImpl(BlockCreateOptions create_options,
                               std::unique_ptr<CBlockTemplate> block_template,
                               NodeContext& node,
                               std::shared_ptr<MiningTaskRunner> task_runner) : m_create_options(std::move(create_options)),
                                                                                m_block_template(std::move(block_template)),
                                                                                m_task_runner(std::move(task_runner)),
                                                                                m_node(node)
    {
        assert(m_block_template);
    }

    CBlockHeader getBlockHeader() override
    {
        return m_block_template->block;
    }

    CBlock getBlock() override
    {
        return m_block_template->block;
    }

    std::vector<CAmount> getTxFees() override
    {
        return m_block_template->vTxFees;
    }

    std::vector<int64_t> getTxSigops() override
    {
        return m_block_template->vTxSigOpsCost;
    }

    CoinbaseTx getCoinbaseTx() override
    {
        return m_block_template->m_coinbase_tx;
    }

    std::vector<uint256> getCoinbaseMerklePath() override
    {
        return TransactionMerklePath(m_block_template->block, 0);
    }

    std::unique_ptr<Handler> submitSolutionAsync(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase, SubmitSolutionFn fn) override
    {
        auto block{m_block_template->block};
        return m_task_runner->schedule([node = &m_node, block = std::move(block), version, timestamp, nonce, coinbase = std::move(coinbase), fn = std::move(fn)](std::atomic_bool& cancelled) mutable {
            if (cancelled.load(std::memory_order_acquire)) return;
            AddMerkleRootAndCoinbase(block, std::move(coinbase), version, timestamp, nonce);
            std::string reason;
            std::string debug;
            const bool accepted{SubmitBlock(*Assert(node->chainman), std::make_shared<const CBlock>(block), /*new_block=*/nullptr, reason, debug)};
            if (!cancelled.load(std::memory_order_acquire)) fn(accepted);
        });
    }

    std::unique_ptr<Handler> watchNext(BlockWaitOptions options, NextTemplateFn fn) override
    {
        struct WatchState : std::enable_shared_from_this<WatchState> {
            std::atomic_bool done{false};
            btcsignals::connection tip_connection;
            std::shared_ptr<MiningTaskRunner> task_runner;
            std::mutex task_mutex;
            std::vector<std::shared_ptr<std::unique_ptr<Handler>>> tasks;
            NodeContext* node{nullptr};
            BlockCreateOptions create_options;
            uint256 current_prev_hash;
            CAmount current_fees{0};
            BlockWaitOptions options;
            NextTemplateFn fn;

            void complete(std::unique_ptr<CBlockTemplate> block_template)
            {
                if (done.exchange(true, std::memory_order_acq_rel)) return;
                tip_connection.disconnect();
                {
                    std::vector<std::shared_ptr<std::unique_ptr<Handler>>> tasks;
                    {
                        std::lock_guard lock{task_mutex};
                        tasks.swap(this->tasks);
                    }
                    tasks.clear();
                }
                if (!fn) return;
                if (block_template) {
                    fn(std::make_unique<BlockTemplateImpl>(create_options, std::move(block_template), *node, task_runner));
                } else {
                    fn(nullptr);
                }
            }

            bool should_refresh(bool tip_changed) const
            {
                return tip_changed || ShouldCreateMinDifficultyTemplate(*Assert(node->chainman));
            }

            void check(bool tip_changed, bool complete_on_miss = false)
            {
                if (done.load(std::memory_order_acquire)) return;
                if (!tip_changed && options.fee_threshold == MAX_MONEY && !should_refresh(/*tip_changed=*/false)) {
                    if (complete_on_miss) complete(nullptr);
                    return;
                }

                auto holder{std::make_shared<std::unique_ptr<Handler>>()};
                auto handler{task_runner->schedule([state = shared_from_this(), holder, tip_changed, complete_on_miss](std::atomic_bool& cancelled) {
                    auto cleanup{[holder] {
                        holder->reset();
                    }};
                    if (cancelled.load(std::memory_order_acquire) || state->done.load(std::memory_order_acquire)) return cleanup();
                    const bool refresh{state->should_refresh(tip_changed)};
                    auto new_template{BlockAssembler{
                        Assert(state->node->chainman)->ActiveChainstate(),
                        state->node->mempool.get(),
                        state->create_options}
                                          .CreateNewBlock()};
                    if (cancelled.load(std::memory_order_acquire) || state->done.load(std::memory_order_acquire)) return cleanup();
                    if (refresh) {
                        state->complete(std::move(new_template));
                        return cleanup();
                    }

                    const CAmount new_fees{TotalTemplateFees(*new_template)};
                    if (new_fees >= state->current_fees + state->options.fee_threshold) {
                        state->complete(std::move(new_template));
                    } else if (complete_on_miss) {
                        state->complete(nullptr);
                    }
                    cleanup();
                })};
                *holder = std::move(handler);
                if (done.load(std::memory_order_acquire)) {
                    holder->reset();
                    return;
                }
                std::lock_guard lock{task_mutex};
                std::erase_if(tasks, [](const auto& task) { return !*task; });
                if (done.load(std::memory_order_acquire)) {
                    holder->reset();
                } else {
                    tasks.push_back(std::move(holder));
                }
            }

            void schedule_fee_tick()
            {
                if (done.load(std::memory_order_acquire) || options.fee_threshold == MAX_MONEY) return;
                Assert(node->scheduler)->schedule([state = shared_from_this()] {
                    if (state->done.load(std::memory_order_acquire)) return;
                    state->check(/*tip_changed=*/false);
                    state->schedule_fee_tick();
                },
                                                  std::chrono::steady_clock::now() + std::chrono::seconds{1});
            }

            void schedule_timeout()
            {
                if (options.timeout >= std::chrono::years{100}) return;
                Assert(node->scheduler)->schedule([state = shared_from_this()] {
                    if (state->options.fee_threshold < MAX_MONEY || state->should_refresh(/*tip_changed=*/false)) {
                        state->check(/*tip_changed=*/false, /*complete_on_miss=*/true);
                    } else {
                        state->complete(nullptr);
                    }
                },
                                                  std::chrono::time_point_cast<std::chrono::steady_clock::duration>(std::chrono::steady_clock::now() + options.timeout));
            }
        };

        auto state{std::make_shared<WatchState>()};
        state->task_runner = m_task_runner;
        state->node = &m_node;
        state->create_options = m_create_options;
        state->current_prev_hash = m_block_template->block.hashPrevBlock;
        state->current_fees = TotalTemplateFees(*m_block_template);
        state->options = options;
        state->fn = std::move(fn);
        state->tip_connection = ::uiInterface.NotifyBlockTip_connect([state](SynchronizationState, const CBlockIndex& block, double) {
            if (block.GetBlockHash() != state->current_prev_hash) state->check(/*tip_changed=*/true);
        });
        if (const auto tip{GetTip(*Assert(m_node.chainman))}; tip && tip->hash != state->current_prev_hash) {
            state->check(/*tip_changed=*/true);
        }
        state->check(/*tip_changed=*/false, /*complete_on_miss=*/options.timeout <= MillisecondsDouble{0});
        state->schedule_fee_tick();
        if (options.timeout > MillisecondsDouble{0}) state->schedule_timeout();
        return MakeCleanupHandler([state] {
            state->done.store(true, std::memory_order_release);
            state->tip_connection.disconnect();
            std::vector<std::shared_ptr<std::unique_ptr<Handler>>> tasks;
            {
                std::lock_guard lock{state->task_mutex};
                tasks.swap(state->tasks);
            }
        });
    }

    const BlockCreateOptions m_create_options;

    const std::unique_ptr<CBlockTemplate> m_block_template;

    std::shared_ptr<MiningTaskRunner> m_task_runner;
    NodeContext& m_node;
};

class MinerImpl : public Mining
{
public:
    explicit MinerImpl(NodeContext& node) : m_task_runner(std::make_shared<MiningTaskRunner>()), m_node(node) {}

    bool isTestChain() override
    {
        return chainman().GetParams().IsTestChain();
    }

    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }

    std::optional<BlockRef> getTip() override
    {
        return GetTip(chainman());
    }

    std::unique_ptr<Handler> watchTip(uint256 current_tip, MillisecondsDouble timeout, TipChangedFn fn) override
    {
        struct WatchState {
            std::atomic_bool done{false};
            btcsignals::connection tip_connection;
            NodeContext* node{nullptr};
            uint256 current_tip;
            TipChangedFn fn;

            void complete(std::optional<BlockRef> tip)
            {
                if (done.exchange(true, std::memory_order_acq_rel)) return;
                tip_connection.disconnect();
                if (fn) fn(std::move(tip));
            }
        };

        auto state{std::make_shared<WatchState>()};
        state->node = &m_node;
        state->current_tip = current_tip;
        state->fn = std::move(fn);

        if (auto tip{getTip()}; tip && tip->hash != current_tip) {
            Assert(m_node.scheduler)->schedule([state, tip] {
                state->complete(tip);
            },
                                               std::chrono::steady_clock::now());
        } else {
            state->tip_connection = ::uiInterface.NotifyBlockTip_connect([state](SynchronizationState, const CBlockIndex& block, double) {
                const BlockRef tip{block.GetBlockHash(), block.nHeight};
                if (tip.hash != state->current_tip) state->complete(tip);
            });
            if (timeout < std::chrono::years{100}) {
                Assert(m_node.scheduler)->schedule([state] {
                    state->complete(GetTip(*Assert(state->node->chainman)));
                },
                                                   std::chrono::time_point_cast<std::chrono::steady_clock::duration>(std::chrono::steady_clock::now() + timeout));
            }
        }

        return MakeCleanupHandler([state] {
            state->done.store(true, std::memory_order_release);
            state->tip_connection.disconnect();
        });
    }

    std::unique_ptr<Handler> createNewBlockAsync(const BlockCreateOptions& options, bool cooldown, CreateBlockFn fn) override
    {
        return m_task_runner->schedule([this, options, cooldown, fn = std::move(fn)](std::atomic_bool& cancelled) mutable {
            try {
                auto block_template{CreateNewBlock(options, cooldown, cancelled)};
                if (!cancelled.load(std::memory_order_acquire)) fn(CreateBlockResult{std::move(block_template)});
            } catch (const std::exception& e) {
                LogError("CreateNewBlock failed: %s\n", e.what());
                if (!cancelled.load(std::memory_order_acquire)) fn(util::Error{Untranslated(e.what())});
            } catch (...) {
                LogError("CreateNewBlock failed with an unknown exception\n");
                if (!cancelled.load(std::memory_order_acquire)) fn(util::Error{Untranslated("unknown CreateNewBlock failure")});
            }
        }, [this] {
            auto& notifications{*Assert(m_node.notifications)};
            LOCK(notifications.m_tip_block_mutex);
            notifications.m_tip_block_cv.notify_all();
        });
    }

    std::unique_ptr<BlockTemplate> CreateNewBlock(const BlockCreateOptions& options, bool cooldown, std::atomic_bool& cancelled)
    {
        // Ensure m_tip_block is set so consumers of BlockTemplate can rely on that.
        MillisecondsDouble startup_timeout{MillisecondsDouble::max()};
        std::optional<BlockRef> maybe_tip{WaitTipChanged(chainman(), notifications(), uint256::ZERO, startup_timeout, cancelled)};

        if (!maybe_tip) return {};

        if (cooldown) {
            // Do not return a template during IBD, because it can have long
            // pauses and sometimes takes a while to get started. Although this
            // is useful in general, it's gated behind the cooldown argument,
            // because on regtest and single miner signets this would wait
            // forever if no block was mined in the past day.
            while (chainman().IsInitialBlockDownload()) {
                MillisecondsDouble timeout{1000};
                maybe_tip = WaitTipChanged(chainman(), notifications(), maybe_tip->hash, timeout, cancelled);
                if (!maybe_tip || chainman().m_interrupt || cancelled.load(std::memory_order_acquire)) return {};
            }

            // Also wait during the final catch-up moments after IBD.
            if (!CooldownIfHeadersAhead(chainman(), notifications(), *maybe_tip, cancelled)) return {};
        }
        const BlockCreateOptions create_options{MergeMiningOptions(options, m_node.mining_args)};
        if (cancelled.load(std::memory_order_acquire)) return {};
        return std::make_unique<BlockTemplateImpl>(create_options,
                                                   BlockAssembler{
                                                       chainman().ActiveChainstate(),
                                                       m_node.mempool.get(),
                                                       create_options,
                                                   }.CreateNewBlock(),
                                                   m_node,
                                                   m_task_runner);
    }

    std::unique_ptr<Handler> checkBlockAsync(CBlock block, node::BlockCheckOptions options, CheckBlockFn fn) override
    {
        return m_task_runner->schedule([this, block = std::move(block), options, fn = std::move(fn)](std::atomic_bool& cancelled) mutable {
            if (cancelled.load(std::memory_order_acquire)) return;
            LOCK(chainman().GetMutex());
            BlockValidationState state{TestBlockValidity(chainman().ActiveChainstate(), block, /*check_pow=*/options.check_pow, /*check_merkle_root=*/options.check_merkle_root)};
            if (!cancelled.load(std::memory_order_acquire)) fn(state.IsValid(), state.GetRejectReason(), state.GetDebugMessage());
        });
    }

    std::unique_ptr<Handler> submitBlockAsync(CBlock block_in, SubmitBlockFn fn) override
    {
        return m_task_runner->schedule([this, block_in = std::move(block_in), fn = std::move(fn)](std::atomic_bool& cancelled) mutable {
            if (cancelled.load(std::memory_order_acquire)) return;
            auto block{std::make_shared<const CBlock>(std::move(block_in))};
            bool new_block;
            std::string reason;
            std::string debug;
            const bool accepted{SubmitBlock(chainman(), block, &new_block, reason, debug)};
            // ProcessNewBlock() can accept and store a block before it is checked
            // for validity. Treat duplicates as errors for mining clients, and only
            // return success when validation completed without setting a reason.
            if (!cancelled.load(std::memory_order_acquire)) fn(accepted && new_block && reason.empty(), std::move(reason), std::move(debug));
        });
    }

    NodeContext* context() override { return &m_node; }
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    KernelNotifications& notifications() { return *Assert(m_node.notifications); }
    std::shared_ptr<MiningTaskRunner> m_task_runner;
    NodeContext& m_node;
};

} // namespace
} // namespace node

namespace interfaces {
std::unique_ptr<Node> MakeNode(node::NodeContext& context) { return std::make_unique<node::NodeImpl>(context); }
std::unique_ptr<Chain> MakeChain(node::NodeContext& context) { return std::make_unique<node::ChainImpl>(context); }
std::unique_ptr<Mining> MakeMining(node::NodeContext& context, bool wait_loaded)
{
    if (wait_loaded) {
        node::KernelNotifications& kernel_notifications(*Assert(context.notifications));
        util::SignalInterrupt& interrupt(*Assert(context.shutdown_signal));
        WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
        kernel_notifications.m_tip_block_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
            return kernel_notifications.m_state.chainstate_loaded || interrupt;
        });
        if (interrupt) return nullptr;
    }
    return std::make_unique<node::MinerImpl>(context);
}
} // namespace interfaces
