// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <banman.h>
#include <block_validation.h>
#include <btcsignals.h>
#include <chain.h>
#include <chainparams.h>
#include <chainstate.h>
#include <clientversion.h>
#include <coins.h>
#include <common/args.h>
#include <common/settings.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <deploymentinfo.h>
#include <deploymentstatus.h>
#include <init.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/noderpc.h>
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
#include <node/protocol_version.h>
#include <node/transaction.h>
#include <node/types.h>
#include <node/warnings.h>
#include <policy/feerate.h>
#include <policy/fees/block_policy_estimator.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <arith_uint256.h>
#include <pow.h>
#include <primitives/block.h>
#include <protocol.h>
#include <script/interpreter.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
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
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using interfaces::BlockRef;
using interfaces::BlockTemplate;
using interfaces::BlockTip;
using interfaces::Chain;
using interfaces::FoundBlock;
using interfaces::Handler;
using interfaces::MakeSignalHandler;
using interfaces::Mining;
using interfaces::Node;
using interfaces::BIP9DeploymentInfo;
using interfaces::BIP9Statistics;
using interfaces::BlockchainInfo;
using interfaces::BlockchainPruneInfo;
using interfaces::SmartFeeEstimate;
using interfaces::DeploymentInfo;
using interfaces::DeploymentsInfo;
using interfaces::NetworkInfo;
using interfaces::NetworkInfoLocalAddress;
using interfaces::NetworkInfoNetwork;
using interfaces::NodeRpc;
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

class BlockTemplateImpl : public BlockTemplate
{
public:
    explicit BlockTemplateImpl(BlockCreateOptions create_options,
                               std::unique_ptr<CBlockTemplate> block_template,
                               const NodeContext& node) : m_create_options(std::move(create_options)),
                                                          m_block_template(std::move(block_template)),
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

    bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase) override
    {
        AddMerkleRootAndCoinbase(m_block_template->block, std::move(coinbase), version, timestamp, nonce);
        return ProcessNewBlock(chainman(), std::make_shared<const CBlock>(m_block_template->block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
    }

    std::unique_ptr<BlockTemplate> waitNext(BlockWaitOptions options) override
    {
        auto new_template = WaitAndCreateNewBlock(chainman(),
                                                  notifications(),
                                                  m_node.mempool.get(),
                                                  m_block_template,
                                                  /*wait_options=*/options,
                                                  /*create_options=*/m_create_options,
                                                  /*interrupt_wait=*/m_interrupt_wait);
        if (new_template) return std::make_unique<BlockTemplateImpl>(m_create_options, std::move(new_template), m_node);
        return nullptr;
    }

    void interruptWait() override
    {
        InterruptWait(notifications(), m_interrupt_wait);
    }

    const BlockCreateOptions m_create_options;

    const std::unique_ptr<CBlockTemplate> m_block_template;

    bool m_interrupt_wait{false};
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    KernelNotifications& notifications() { return *Assert(m_node.notifications); }
    const NodeContext& m_node;
};

class MinerImpl : public Mining
{
public:
    explicit MinerImpl(const NodeContext& node) : m_node(node) {}

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

    std::optional<BlockRef> waitTipChanged(uint256 current_tip, MillisecondsDouble timeout) override
    {
        return WaitTipChanged(chainman(), notifications(), current_tip, timeout, m_interrupt_mining);
    }

    std::unique_ptr<BlockTemplate> createNewBlock(const BlockCreateOptions& options, bool cooldown) override
    {
        // Ensure m_tip_block is set so consumers of BlockTemplate can rely on that.
        std::optional<BlockRef> maybe_tip{waitTipChanged(uint256::ZERO, MillisecondsDouble::max())};

        if (!maybe_tip) return {};

        if (cooldown) {
            // Do not return a template during IBD, because it can have long
            // pauses and sometimes takes a while to get started. Although this
            // is useful in general, it's gated behind the cooldown argument,
            // because on regtest and single miner signets this would wait
            // forever if no block was mined in the past day.
            while (chainman().IsInitialBlockDownload()) {
                maybe_tip = waitTipChanged(maybe_tip->hash, MillisecondsDouble{1000});
                if (!maybe_tip || chainman().m_interrupt || WITH_LOCK(notifications().m_tip_block_mutex, return m_interrupt_mining)) return {};
            }

            // Also wait during the final catch-up moments after IBD.
            if (!CooldownIfHeadersAhead(chainman(), notifications(), *maybe_tip, m_interrupt_mining)) return {};
        }
        const BlockCreateOptions create_options{MergeMiningOptions(options, m_node.mining_args)};
        return std::make_unique<BlockTemplateImpl>(create_options,
                                                   BlockAssembler{
                                                       chainman().ActiveChainstate(),
                                                       m_node.mempool.get(),
                                                       create_options,
                                                   }.CreateNewBlock(),
                                                   m_node);
    }

    void interrupt() override
    {
        InterruptWait(notifications(), m_interrupt_mining);
    }

    bool checkBlock(const CBlock& block, const node::BlockCheckOptions& options, std::string& reason, std::string& debug) override
    {
        LOCK(chainman().GetMutex());
        BlockValidationState state{TestBlockValidity(chainman().ActiveChainstate(), block, /*check_pow=*/options.check_pow, /*check_merkle_root=*/options.check_merkle_root)};
        reason = state.GetRejectReason();
        debug = state.GetDebugMessage();
        return state.IsValid();
    }

    const NodeContext* context() override { return &m_node; }
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    KernelNotifications& notifications() { return *Assert(m_node.notifications); }
    // Treat as if guarded by notifications().m_tip_block_mutex
    bool m_interrupt_mining{false};
    const NodeContext& m_node;
};

// Difficulty as a multiple of the minimum difficulty, mirroring the old
// rpc/blockchain.cpp helper.
double GetDifficulty(const CBlockIndex& blockindex)
{
    int nShift = (blockindex.nBits >> 24) & 0xff;
    double dDiff = (double)0x0000ffff / (double)(blockindex.nBits & 0x00ffffff);
    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }
    return dDiff;
}

// Difficulty target for a block, mirroring the old rpc/util.cpp GetTarget.
uint256 GetTarget(const CBlockIndex& blockindex, const uint256 pow_limit)
{
    arith_uint256 target{*CHECK_NONFATAL(DeriveTarget(blockindex.nBits, pow_limit))};
    return ArithToUint256(target);
}

// Height of the last pruned block, mirroring the old rpc/blockchain.cpp helper.
std::optional<int> GetPruneHeight(const BlockManager& blockman, const CChain& chain) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);

    // Search for the last block missing block data or undo data. Don't let the
    // search consider the genesis block, because the genesis block does not
    // have undo data, but should not be considered pruned.
    const CBlockIndex* first_block{chain[1]};
    const CBlockIndex* chain_tip{chain.Tip()};

    // If there are no blocks after the genesis block, or no blocks at all, nothing is pruned.
    if (!first_block || !chain_tip) return std::nullopt;

    // If the chain tip is pruned, everything is pruned.
    if ((chain_tip->nStatus & BLOCK_HAVE_MASK) != BLOCK_HAVE_MASK) return chain_tip->nHeight;

    const auto& first_unpruned{blockman.GetFirstBlock(*chain_tip, /*status_mask=*/BLOCK_HAVE_MASK, first_block)};
    if (&first_unpruned == first_block) {
        // All blocks between first_block and chain_tip have data, so nothing is pruned.
        return std::nullopt;
    }

    // Block before the first unpruned block is the last pruned block.
    return CHECK_NONFATAL(first_unpruned.pprev)->nHeight;
}

class NodeRpcImpl : public NodeRpc
{
public:
    explicit NodeRpcImpl(NodeContext& node) : m_node(node) {}
    BlockchainInfo getBlockchainInfo() override
    {
        ChainstateManager& chainman = *Assert(m_node.chainman);
        LOCK(::cs_main);
        Chainstate& active_chainstate = chainman.ActiveChainstate();
        const CBlockIndex& tip{*CHECK_NONFATAL(active_chainstate.m_chain.Tip())};

        BlockchainInfo info;
        info.chain = chainman.GetParams().GetChainTypeString();
        info.blocks = tip.nHeight;
        info.headers = chainman.m_best_header ? chainman.m_best_header->nHeight : -1;
        info.bestblockhash = tip.GetBlockHash().GetHex();
        info.bits = strprintf("%08x", tip.nBits);
        info.target = GetTarget(tip, chainman.GetConsensus().powLimit).GetHex();
        info.difficulty = GetDifficulty(tip);
        info.time = tip.GetBlockTime();
        info.mediantime = tip.GetMedianTimePast();
        info.verificationprogress = chainman.GuessVerificationProgress(&tip);
        info.initialblockdownload = chainman.IsInitialBlockDownload();
        info.chainwork = tip.nChainWork.GetHex();
        info.size_on_disk = chainman.m_blockman.CalculateCurrentUsage();
        info.pruned = chainman.m_blockman.IsPruneMode();
        if (info.pruned) {
            BlockchainPruneInfo& prune = info.prune.emplace();
            const auto prune_height{GetPruneHeight(chainman.m_blockman, active_chainstate.m_chain)};
            prune.height = prune_height ? prune_height.value() + 1 : 0;
            prune.automatic = chainman.m_blockman.GetPruneTarget() != BlockManager::PRUNE_TARGET_MANUAL;
            if (prune.automatic) {
                prune.target_size = chainman.m_blockman.GetPruneTarget();
            }
        }
        if (chainman.GetParams().GetChainType() == ChainType::SIGNET) {
            info.signet_challenge = HexStr(chainman.GetParams().GetConsensus().signet_challenge);
        }
        if (m_node.warnings) {
            for (const auto& warning : m_node.warnings->GetMessages()) {
                info.warnings.push_back(warning.original);
            }
        }
        return info;
    }
    SmartFeeEstimate estimateSmartFee(int conf_target, bool conservative) override
    {
        SmartFeeEstimate result;
        if (!m_node.fee_estimator || !m_node.mempool) {
            result.errors.emplace_back("Fee estimation disabled");
            return result;
        }
        CBlockPolicyEstimator& fee_estimator = *m_node.fee_estimator;
        CTxMemPool& mempool = *m_node.mempool;

        // Ensure the estimator has seen all recently connected blocks.
        CHECK_NONFATAL(mempool.m_opts.signals)->SyncWithValidationInterfaceQueue();

        const unsigned int max_target = fee_estimator.HighestTargetTracked(FeeEstimateHorizon::LONG_HALFLIFE);
        if (conf_target < 1 || static_cast<unsigned int>(conf_target) > max_target) {
            throw std::runtime_error(strprintf("Invalid conf_target, must be between %u and %u", 1, max_target));
        }

        FeeCalculation fee_calc;
        CFeeRate fee_rate{fee_estimator.estimateSmartFee(conf_target, &fee_calc, conservative)};
        if (fee_rate != CFeeRate(0)) {
            // Floor the estimate at the mempool min fee and the relay fee, like the RPC.
            const CFeeRate min_mempool_feerate{mempool.GetMinFee()};
            const CFeeRate min_relay_feerate{mempool.m_opts.min_relay_feerate};
            fee_rate = std::max({fee_rate, min_mempool_feerate, min_relay_feerate});
            result.feerate = fee_rate.GetFeePerK();
        } else {
            result.errors.emplace_back("Insufficient data or no feerate found");
        }
        result.blocks = fee_calc.returnedTarget;
        return result;
    }
    NetworkInfo getNetworkInfo() override
    {
        NetworkInfo info;
        info.version = CLIENT_VERSION;
        info.subversion = strSubVersion;
        info.protocolversion = PROTOCOL_VERSION;
        if (m_node.connman) {
            const ServiceFlags services = m_node.connman->GetLocalServices();
            info.localservices = strprintf("%016x", static_cast<uint64_t>(services));
            info.localservicesnames = serviceFlagsToStr(services);
        }
        if (m_node.peerman) {
            const auto peerman_info{m_node.peerman->GetInfo()};
            info.localrelay = !peerman_info.ignores_incoming_txs;
            info.timeoffset = Ticks<std::chrono::seconds>(peerman_info.median_outbound_time_offset);
        }
        if (m_node.connman) {
            info.networkactive = m_node.connman->GetNetworkActive();
            info.connections = m_node.connman->GetNodeCount(ConnectionDirection::Both);
            info.connections_in = m_node.connman->GetNodeCount(ConnectionDirection::In);
            info.connections_out = m_node.connman->GetNodeCount(ConnectionDirection::Out);
        }
        for (int n = 0; n < NET_MAX; ++n) {
            const enum Network network = static_cast<enum Network>(n);
            if (network == NET_UNROUTABLE || network == NET_INTERNAL) continue;
            NetworkInfoNetwork& net = info.networks.emplace_back();
            net.name = GetNetworkName(network);
            net.limited = !g_reachable_nets.Contains(network);
            net.reachable = g_reachable_nets.Contains(network);
            if (const auto proxy = GetProxy(network)) {
                net.proxy = proxy->ToString();
                net.proxy_randomize_credentials = proxy->m_tor_stream_isolation;
            }
        }
        if (m_node.mempool) {
            info.relayfee = m_node.mempool->m_opts.min_relay_feerate.GetFeePerK();
            info.incrementalfee = m_node.mempool->m_opts.incremental_relay_feerate.GetFeePerK();
        }
        {
            LOCK(g_maplocalhost_mutex);
            for (const auto& [addr, service] : mapLocalHost) {
                NetworkInfoLocalAddress& local = info.localaddresses.emplace_back();
                local.address = addr.ToStringAddr();
                local.port = service.nPort;
                local.score = service.nScore;
            }
        }
        if (m_node.warnings) {
            for (const auto& warning : m_node.warnings->GetMessages()) {
                info.warnings.push_back(warning.original);
            }
        }
        return info;
    }
    DeploymentsInfo getDeploymentInfo(const std::optional<uint256>& block_hash) override
    {
        ChainstateManager& chainman = *Assert(m_node.chainman);
        LOCK(::cs_main);

        const CBlockIndex* blockindex;
        if (!block_hash) {
            blockindex = Assert(chainman.ActiveChain().Tip());
        } else {
            blockindex = chainman.m_blockman.LookupBlockIndex(*block_hash);
            if (!blockindex) {
                throw std::runtime_error("Block not found");
            }
        }

        DeploymentsInfo info;
        info.hash = blockindex->GetBlockHash().ToString();
        info.height = blockindex->nHeight;
        for (const auto& flag : GetScriptFlagNames(GetBlockScriptFlags(*blockindex, chainman))) {
            info.script_flags.push_back(flag);
        }

        // Buried deployments (BIP 90): active from a hardcoded height.
        const auto add_buried = [&](Consensus::BuriedDeployment dep) {
            if (!DeploymentEnabled(chainman, dep)) return;
            DeploymentInfo& d = info.deployments.emplace_back();
            d.name = DeploymentName(dep);
            d.type = "buried";
            d.active = DeploymentActiveAfter(blockindex, chainman, dep);
            d.height = chainman.GetConsensus().DeploymentHeight(dep);
        };
        add_buried(Consensus::DEPLOYMENT_HEIGHTINCB);
        add_buried(Consensus::DEPLOYMENT_DERSIG);
        add_buried(Consensus::DEPLOYMENT_CLTV);
        add_buried(Consensus::DEPLOYMENT_CSV);
        add_buried(Consensus::DEPLOYMENT_SEGWIT);

        // BIP 9 versionbits deployments.
        const auto add_bip9 = [&](Consensus::DeploymentPos pos) {
            if (!DeploymentEnabled(chainman, pos)) return;
            DeploymentInfo& d = info.deployments.emplace_back();
            d.name = DeploymentName(pos);
            d.type = "bip9";

            const BIP9Info bip9_info{chainman.m_versionbitscache.Info(*blockindex, chainman.GetConsensus(), pos)};
            const Consensus::BIP9Deployment& depparams{chainman.GetConsensus().vDeployments[pos]};
            BIP9DeploymentInfo& bip9 = d.bip9.emplace();
            if (bip9_info.stats.has_value()) {
                bip9.bit = depparams.bit;
            }
            bip9.start_time = depparams.nStartTime;
            bip9.timeout = depparams.nTimeout;
            bip9.min_activation_height = depparams.min_activation_height;
            bip9.status = bip9_info.current_state;
            bip9.since = bip9_info.since;
            bip9.status_next = bip9_info.next_state;
            if (bip9_info.stats.has_value()) {
                BIP9Statistics& stats = bip9.statistics.emplace();
                stats.period = bip9_info.stats->period;
                stats.elapsed = bip9_info.stats->elapsed;
                stats.count = bip9_info.stats->count;
                stats.threshold = bip9_info.stats->threshold;
                stats.possible = bip9_info.stats->possible;

                bip9.signalling.reserve(bip9_info.signalling_blocks.size());
                for (const bool s : bip9_info.signalling_blocks) {
                    bip9.signalling.push_back(s ? '#' : '-');
                }
            }
            if (bip9_info.active_since.has_value()) {
                d.height = *bip9_info.active_since;
                d.active = (*bip9_info.active_since <= blockindex->nHeight + 1);
            }
        };
        add_bip9(Consensus::DEPLOYMENT_TESTDUMMY);

        return info;
    }
    NodeContext& m_node;
};

} // namespace
} // namespace node

namespace interfaces {
std::unique_ptr<Node> MakeNode(node::NodeContext& context) { return std::make_unique<node::NodeImpl>(context); }
std::unique_ptr<Chain> MakeChain(node::NodeContext& context) { return std::make_unique<node::ChainImpl>(context); }
std::unique_ptr<NodeRpc> MakeNodeRpc(node::NodeContext& context) { return std::make_unique<node::NodeRpcImpl>(context); }
std::unique_ptr<Mining> MakeMining(const node::NodeContext& context, bool wait_loaded)
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
