// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ipc/capnp/protocol.h>

#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>
#include <interfaces/echo.h>
#include <interfaces/init.h>
#include <interfaces/mining.h>
#include <ipc/capnp/common-types.h>
#include <ipc/capnp/context.h>
#include <ipc/capnp/echo.capnp.h>
#include <ipc/capnp/init.capnp.h>
#include <ipc/capnp/mining.capnp.h>
#include <ipc/exception.h>
#include <ipc/protocol.h>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <logging.h>
#include <node/mining_types.h>
#include <policy/feerate.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sys/socket.h>
#include <uint256.h>
#include <unistd.h>
#include <util/result.h>
#include <util/syserror.h>
#include <util/threadnames.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ipc {
namespace capnp {
namespace {

namespace messages = ipc::capnp::messages;

::capnp::rpc::twoparty::VatId::Reader ServerVatId(::capnp::MallocMessageBuilder& message)
{
    auto vat_id{message.getRoot<::capnp::rpc::twoparty::VatId>()};
    vat_id.setSide(::capnp::rpc::twoparty::Side::SERVER);
    return vat_id.asReader();
}

uint256 ReadUint256(::capnp::Data::Reader data)
{
    if (data.size() != uint256::size()) {
        throw Exception{"invalid uint256 byte length"};
    }
    uint256 value;
    std::copy(data.begin(), data.end(), value.begin());
    return value;
}

void SetBytes(::capnp::Data::Builder output, std::span<const unsigned char> bytes)
{
    std::memcpy(output.begin(), bytes.data(), bytes.size());
}

void SetBlockRef(messages::BlockRef::Builder builder, const interfaces::BlockRef& block)
{
    SetBytes(builder.initHash(uint256::size()), {block.hash.data(), block.hash.size()});
    builder.setHeight(block.height);
}

interfaces::BlockRef ReadBlockRef(messages::BlockRef::Reader reader)
{
    return {.hash = ReadUint256(reader.getHash()), .height = reader.getHeight()};
}

CScript ReadScript(::capnp::Data::Reader data)
{
    return {data.begin(), data.end()};
}

node::BlockCreateOptions ReadBlockCreateOptions(messages::BlockCreateOptions::Reader reader)
{
    node::BlockCreateOptions options;
    options.use_mempool = reader.getUseMempool();
    if (reader.getHasBlockMinFeeRate()) options.block_min_fee_rate = ipc::capnp::DeserializeBytes<CFeeRate>(reader.getBlockMinFeeRate());
    if (reader.getHasPrintModifiedFee()) options.print_modified_fee = reader.getPrintModifiedFee();
    if (reader.getHasBlockReservedWeight()) options.block_reserved_weight = reader.getBlockReservedWeight();
    if (reader.getHasBlockMaxWeight()) options.block_max_weight = reader.getBlockMaxWeight();
    options.coinbase_output_max_additional_sigops = reader.getCoinbaseOutputMaxAdditionalSigops();
    options.test_block_validity = reader.getTestBlockValidity();
    if (reader.getCoinbaseOutputScript().size() > 0) {
        options.coinbase_output_script = ReadScript(reader.getCoinbaseOutputScript());
    }
    return options;
}

void SetBlockCreateOptions(messages::BlockCreateOptions::Builder builder, const node::BlockCreateOptions& options)
{
    builder.setUseMempool(options.use_mempool);
    builder.setHasBlockMinFeeRate(options.block_min_fee_rate.has_value());
    if (options.block_min_fee_rate) {
        const auto fee_rate_bytes{ipc::capnp::SerializeBytes(*options.block_min_fee_rate)};
        SetBytes(builder.initBlockMinFeeRate(fee_rate_bytes.size()), fee_rate_bytes);
    }
    builder.setHasPrintModifiedFee(options.print_modified_fee.has_value());
    if (options.print_modified_fee) builder.setPrintModifiedFee(*options.print_modified_fee);
    builder.setHasBlockReservedWeight(options.block_reserved_weight.has_value());
    if (options.block_reserved_weight) builder.setBlockReservedWeight(*options.block_reserved_weight);
    builder.setHasBlockMaxWeight(options.block_max_weight.has_value());
    if (options.block_max_weight) builder.setBlockMaxWeight(*options.block_max_weight);
    builder.setCoinbaseOutputMaxAdditionalSigops(options.coinbase_output_max_additional_sigops);
    SetBytes(builder.initCoinbaseOutputScript(options.coinbase_output_script.size()), {options.coinbase_output_script.data(), options.coinbase_output_script.size()});
    builder.setTestBlockValidity(options.test_block_validity);
}

node::BlockWaitOptions ReadBlockWaitOptions(messages::BlockWaitOptions::Reader reader)
{
    return {
        .timeout = MillisecondsDouble{reader.getTimeout()},
        .fee_threshold = reader.getFeeThreshold(),
    };
}

void SetBlockWaitOptions(messages::BlockWaitOptions::Builder builder, const node::BlockWaitOptions& options)
{
    builder.setTimeout(options.timeout.count());
    builder.setFeeThreshold(options.fee_threshold);
}

node::BlockCheckOptions ReadBlockCheckOptions(messages::BlockCheckOptions::Reader reader)
{
    return {
        .check_merkle_root = reader.getCheckMerkleRoot(),
        .check_pow = reader.getCheckPow(),
    };
}

void SetBlockCheckOptions(messages::BlockCheckOptions::Builder builder, const node::BlockCheckOptions& options)
{
    builder.setCheckMerkleRoot(options.check_merkle_root);
    builder.setCheckPow(options.check_pow);
}

void SetCoinbaseTx(messages::CoinbaseTx::Builder builder, const node::CoinbaseTx& tx)
{
    builder.setVersion(tx.version);
    builder.setSequence(tx.sequence);
    SetBytes(builder.initScriptSigPrefix(tx.script_sig_prefix.size()), {tx.script_sig_prefix.data(), tx.script_sig_prefix.size()});
    if (tx.witness) {
        SetBytes(builder.initWitness(uint256::size()), {tx.witness->data(), tx.witness->size()});
    }
    builder.setBlockRewardRemaining(tx.block_reward_remaining);
    auto outputs{builder.initRequiredOutputs(tx.required_outputs.size())};
    for (size_t i{0}; i < tx.required_outputs.size(); ++i) {
        const auto bytes{ipc::capnp::SerializeBytes(tx.required_outputs[i])};
        SetBytes(outputs.init(i, bytes.size()), bytes);
    }
    builder.setLockTime(tx.lock_time);
}

node::CoinbaseTx ReadCoinbaseTx(messages::CoinbaseTx::Reader reader)
{
    std::vector<CTxOut> outputs;
    outputs.reserve(reader.getRequiredOutputs().size());
    for (auto output : reader.getRequiredOutputs()) {
        outputs.push_back(ipc::capnp::DeserializeBytes<CTxOut>(output));
    }
    std::optional<uint256> witness;
    if (reader.getWitness().size() > 0) {
        witness = ReadUint256(reader.getWitness());
    }
    return {
        .version = reader.getVersion(),
        .sequence = reader.getSequence(),
        .script_sig_prefix = ReadScript(reader.getScriptSigPrefix()),
        .witness = witness,
        .block_reward_remaining = reader.getBlockRewardRemaining(),
        .required_outputs = std::move(outputs),
        .lock_time = reader.getLockTime(),
    };
}

template <typename T, typename Start>
kj::Promise<T> NodePromise(Start&& start)
{
    auto pair{kj::newPromiseAndCrossThreadFulfiller<T>()};
    auto fulfiller{std::make_shared<kj::Own<kj::CrossThreadPromiseFulfiller<T>>>(kj::mv(pair.fulfiller))};
    auto complete{[fulfiller](T result) mutable {
        if (*fulfiller) {
            (*fulfiller)->fulfill(std::move(result));
            *fulfiller = nullptr;
        }
    }};
    auto handler{start(std::move(complete))};
    return kj::mv(pair.promise).attach(std::move(handler), std::move(fulfiller));
}

class TaskErrorLogger final : public kj::TaskSet::ErrorHandler
{
public:
    void taskFailed(kj::Exception&& exception) override
    {
        LogDebug(BCLog::IPC, "IPC background task failed: %s\n", exception.getDescription().cStr());
    }
};

class CallbackExecutor
{
public:
    CallbackExecutor()
        : m_thread([this] {
              util::ThreadRename("ipc-callback");
              Run();
          })
    {
    }

    ~CallbackExecutor() noexcept { Stop(); }

    void Post(std::function<void()> task)
    {
        {
            std::lock_guard lock{m_mutex};
            if (m_stopped) return;
            m_tasks.push_back(std::move(task));
        }
        m_cv.notify_one();
    }

    void Stop() noexcept
    {
        {
            std::lock_guard lock{m_mutex};
            if (m_stopped) return;
            m_stopped = true;
        }
        m_cv.notify_one();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    void Run() noexcept
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock{m_mutex};
                m_cv.wait(lock, [this] { return m_stopped || !m_tasks.empty(); });
                if (m_stopped && m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }

            try {
                task();
            } catch (const std::exception& e) {
                LogDebug(BCLog::IPC, "IPC callback failed: %s\n", e.what());
            } catch (...) {
                LogDebug(BCLog::IPC, "IPC callback failed\n");
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_tasks;
    std::thread m_thread;
    bool m_stopped{false};
};

// Cap'n Proto clients are thread-affine. Remote client slots may be held by
// ordinary C++ interface objects, but the contained capability is created,
// used, and destroyed on the client event-loop thread.
template <typename Client>
struct RemoteClient {
    std::optional<Client> client;
};

struct ClientThreadContext {
    explicit ClientThreadContext(int fd)
        : io(kj::setupAsyncIo()),
          stream(io.lowLevelProvider->wrapSocketFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP)),
          network(*stream, ::capnp::rpc::twoparty::Side::CLIENT, ::capnp::ReaderOptions{}),
          rpc_system(::capnp::makeRpcClient(network)),
          tasks(task_errors)
    {
    }

    kj::AsyncIoContext io;
    kj::Own<kj::AsyncIoStream> stream;
    ::capnp::TwoPartyVatNetwork network;
    ::capnp::RpcSystem<::capnp::rpc::twoparty::VatId> rpc_system;
    TaskErrorLogger task_errors;
    kj::TaskSet tasks;
};

class ClientConnection
{
public:
    explicit ClientConnection(int fd)
    {
        m_init = MakeClient<messages::Init::Client>();

        std::promise<void> ready;
        auto ready_future{ready.get_future()};
        auto state{m_state};
        auto init{m_init};
        m_thread = std::thread{[fd, state, init, ready = std::move(ready)]() mutable {
            util::ThreadRename("capnp-client");
            bool ready_sent{false};
            try {
                ClientThreadContext context{fd};
                ::capnp::MallocMessageBuilder vat_message;
                init->client.emplace(context.rpc_system.bootstrap(ServerVatId(vat_message)).castAs<messages::Init>());

                auto shutdown_pair{kj::newPromiseAndFulfiller<void>()};
                {
                    std::lock_guard lock{state->mutex};
                    state->context = &context;
                    state->executor = kj::getCurrentThreadExecutor().addRef();
                    state->shutdown_fulfiller = std::make_shared<kj::Own<kj::PromiseFulfiller<void>>>(kj::mv(shutdown_pair.fulfiller));
                    state->thread_id = std::this_thread::get_id();
                }
                ready.set_value();
                ready_sent = true;

                try {
                    kj::mv(shutdown_pair.promise).exclusiveJoin(context.network.onDisconnect()).wait(context.io.waitScope);
                } catch (kj::Exception& exception) {
                    LogDebug(BCLog::IPC, "IPC client connection closed: %s\n", exception.getDescription().cStr());
                }
                context.tasks.clear();
                init->client.reset();
            } catch (...) {
                if (!ready_sent) ready.set_exception(std::current_exception());
            }
            {
                std::lock_guard lock{state->mutex};
                state->context = nullptr;
                state->executor = nullptr;
                state->shutdown_fulfiller.reset();
                state->stopped = true;
            }
        }};
        try {
            ready_future.get();
        } catch (...) {
            if (m_thread.joinable()) m_thread.join();
            throw;
        }
    }

    ~ClientConnection() noexcept
    {
        Stop();
        m_callbacks->Stop();
    }

    template <typename Client>
    using ClientPtr = std::shared_ptr<RemoteClient<Client>>;

    template <typename Client>
    ClientPtr<Client> MakeClient()
    {
        std::weak_ptr<State> state{m_state};
        return ClientPtr<Client>{new RemoteClient<Client>{}, [state](RemoteClient<Client>* client) noexcept {
            DestroyClient(state, client);
        }};
    }

    ClientPtr<messages::Init::Client> init() { return m_init; }

    void PostCallback(std::function<void()> task)
    {
        m_callbacks->Post(std::move(task));
    }

    template <typename Fn>
    decltype(auto) Call(Fn&& fn)
    {
        ClientThreadContext* context{nullptr};
        kj::Maybe<kj::Own<const kj::Executor>> executor;
        bool on_thread{false};
        {
            std::lock_guard lock{m_state->mutex};
            if (m_state->stopped || !m_state->context) {
                throw Exception{"IPC client connection closed"};
            }
            context = m_state->context;
            on_thread = std::this_thread::get_id() == m_state->thread_id;
            if (!on_thread) {
                KJ_IF_MAYBE(e, m_state->executor) {
                    executor = (*e)->addRef();
                } else {
                    throw Exception{"IPC client connection closed"};
                }
            }
        }

        if (on_thread) {
            throw Exception{"synchronous IPC client call from IPC event loop"};
        }
        KJ_IF_MAYBE(e, executor) {
            // Compatibility path for synchronous interface methods. If fn
            // returns a KJ promise, executeSync drives it on the event loop.
            return (*e)->executeSync([context, fn = std::forward<Fn>(fn)]() mutable -> decltype(auto) {
                return fn(*context);
            });
        }
        throw Exception{"IPC client connection closed"};
    }

    template <typename Fn>
    void AddTaskSync(Fn&& fn)
    {
        ClientThreadContext* context{nullptr};
        kj::Maybe<kj::Own<const kj::Executor>> executor;
        bool on_thread{false};
        {
            std::lock_guard lock{m_state->mutex};
            if (m_state->stopped || !m_state->context) {
                throw Exception{"IPC client connection closed"};
            }
            context = m_state->context;
            on_thread = std::this_thread::get_id() == m_state->thread_id;
            if (!on_thread) {
                KJ_IF_MAYBE(e, m_state->executor) {
                    executor = (*e)->addRef();
                } else {
                    throw Exception{"IPC client connection closed"};
                }
            }
        }

        // Preserve the async interface contract even when the RPC promise is
        // ready immediately: callers must receive the handler before callbacks
        // can run.
        if (on_thread) {
            context->tasks.add(kj::evalLater([context, fn = std::forward<Fn>(fn)]() mutable {
                return fn(*context);
            }));
            return;
        }
        KJ_IF_MAYBE(e, executor) {
            // Async interface methods schedule the RPC and return immediately;
            // completion callbacks run from the Cap'n Proto event loop.
            (*e)->executeSync([context, fn = std::forward<Fn>(fn)]() mutable {
                context->tasks.add(kj::evalLater([context, fn = std::move(fn)]() mutable {
                    return fn(*context);
                }));
            });
            return;
        }
        throw Exception{"IPC client connection closed"};
    }

private:
    struct State {
        std::mutex mutex;
        ClientThreadContext* context{nullptr};
        kj::Maybe<kj::Own<const kj::Executor>> executor;
        std::shared_ptr<kj::Own<kj::PromiseFulfiller<void>>> shutdown_fulfiller;
        std::thread::id thread_id;
        bool stopped{false};
    };

    template <typename Client>
    static void DestroyClient(std::weak_ptr<State> weak_state, RemoteClient<Client>* client) noexcept
    {
        if (auto state{weak_state.lock()}) {
            kj::Maybe<kj::Own<const kj::Executor>> executor;
            bool on_thread{false};
            {
                std::lock_guard lock{state->mutex};
                on_thread = std::this_thread::get_id() == state->thread_id;
                if (!on_thread && !state->stopped) {
                    KJ_IF_MAYBE(e, state->executor) {
                        executor = (*e)->addRef();
                    }
                }
            }

            if (on_thread) {
                delete client;
                return;
            }
            KJ_IF_MAYBE(e, executor) {
                try {
                    (*e)->executeSync([client] {
                        delete client;
                    });
                    return;
                } catch (...) {
                }
            }
        }
        delete client;
    }

    void Stop() noexcept
    {
        std::shared_ptr<kj::Own<kj::PromiseFulfiller<void>>> shutdown_fulfiller;
        kj::Maybe<kj::Own<const kj::Executor>> executor;
        bool on_thread{false};
        {
            std::lock_guard lock{m_state->mutex};
            on_thread = std::this_thread::get_id() == m_state->thread_id;
            shutdown_fulfiller = m_state->shutdown_fulfiller;
            if (!on_thread && !m_state->stopped) {
                KJ_IF_MAYBE(e, m_state->executor) {
                    executor = (*e)->addRef();
                }
            }
        }

        auto stop{[shutdown_fulfiller] {
            if (shutdown_fulfiller && *shutdown_fulfiller) {
                (*shutdown_fulfiller)->fulfill();
                *shutdown_fulfiller = nullptr;
            }
        }};

        try {
            if (on_thread) {
                stop();
            } else {
                KJ_IF_MAYBE(e, executor) {
                    (*e)->executeSync(std::move(stop));
                }
            }
        } catch (...) {
        }

        if (m_thread.joinable()) {
            if (std::this_thread::get_id() == m_thread.get_id()) {
                m_thread.detach();
            } else {
                m_thread.join();
            }
        }
    }

    std::shared_ptr<State> m_state{std::make_shared<State>()};
    std::shared_ptr<CallbackExecutor> m_callbacks{std::make_shared<CallbackExecutor>()};
    std::thread m_thread;
    ClientPtr<messages::Init::Client> m_init;
};

struct RemoteWatchState {
    explicit RemoteWatchState(ClientConnection::ClientPtr<messages::Watch::Client> watch_client)
        : watch(std::move(watch_client))
    {
    }

    std::atomic<bool> active{true};
    ClientConnection::ClientPtr<messages::Watch::Client> watch;
};

kj::Promise<void> CancelRemoteWatch(const std::shared_ptr<RemoteWatchState>& state)
{
    if (!state->watch->client) return kj::READY_NOW;
    return state->watch->client->cancelRequest().send().ignoreResult();
}

std::unique_ptr<interfaces::BlockTemplate> MakeRemoteBlockTemplate(std::shared_ptr<ClientConnection> connection, ClientConnection::ClientPtr<messages::BlockTemplate::Client> client);

class RemoteTipListener final : public messages::TipListener::Server
{
public:
    RemoteTipListener(std::shared_ptr<ClientConnection> connection, std::shared_ptr<RemoteWatchState> state, interfaces::Mining::TipChangedFn fn)
        : m_connection(std::move(connection)), m_state(std::move(state)), m_fn(std::move(fn))
    {
    }

    kj::Promise<void> tipChanged(TipChangedContext context) override
    {
        const auto tip{ReadBlockRef(context.getParams().getTip())};
        m_connection->PostCallback([state = m_state, fn = m_fn, tip] {
            if (state->active.exchange(false, std::memory_order_acq_rel)) {
                fn(tip);
            }
        });
        return CancelRemoteWatch(m_state);
    }

    kj::Promise<void> stopped(StoppedContext) override
    {
        m_connection->PostCallback([state = m_state, fn = m_fn] {
            if (state->active.exchange(false, std::memory_order_acq_rel)) {
                fn(std::nullopt);
            }
        });
        return CancelRemoteWatch(m_state);
    }

private:
    std::shared_ptr<ClientConnection> m_connection;
    std::shared_ptr<RemoteWatchState> m_state;
    interfaces::Mining::TipChangedFn m_fn;
};

class RemoteTemplateListener final : public messages::BlockTemplateListener::Server
{
public:
    RemoteTemplateListener(std::shared_ptr<ClientConnection> connection, std::shared_ptr<RemoteWatchState> state, interfaces::BlockTemplate::NextTemplateFn fn)
        : m_connection(std::move(connection)), m_state(std::move(state)), m_fn(std::move(fn))
    {
    }

    kj::Promise<void> templateReady(TemplateReadyContext context) override
    {
        auto block_template{m_connection->MakeClient<messages::BlockTemplate::Client>()};
        block_template->client.emplace(context.getParams().getResult());
        auto result{std::make_shared<std::unique_ptr<interfaces::BlockTemplate>>(MakeRemoteBlockTemplate(m_connection, std::move(block_template)))};
        m_connection->PostCallback([state = m_state, fn = m_fn, result]() mutable {
            if (state->active.exchange(false, std::memory_order_acq_rel)) {
                fn(std::move(*result));
            }
        });
        return CancelRemoteWatch(m_state);
    }

    kj::Promise<void> stopped(StoppedContext) override
    {
        m_connection->PostCallback([state = m_state, fn = m_fn] {
            if (state->active.exchange(false, std::memory_order_acq_rel)) {
                fn(nullptr);
            }
        });
        return CancelRemoteWatch(m_state);
    }

private:
    std::shared_ptr<ClientConnection> m_connection;
    std::shared_ptr<RemoteWatchState> m_state;
    interfaces::BlockTemplate::NextTemplateFn m_fn;
};

class RemoteBlockTemplate final : public interfaces::BlockTemplate
{
public:
    RemoteBlockTemplate(std::shared_ptr<ClientConnection> connection, ClientConnection::ClientPtr<messages::BlockTemplate::Client> client)
        : m_connection(std::move(connection)), m_client(std::move(client))
    {
    }
    ~RemoteBlockTemplate() noexcept override {}

    CBlockHeader getBlockHeader() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->getBlockHeaderRequest().send().then([](auto response) {
                return ipc::capnp::DeserializeBytes<CBlockHeader>(response.getResult());
            });
        });
    }

    CBlock getBlock() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->getBlockRequest().send().then([](auto response) {
                return ipc::capnp::DeserializeBytes<CBlock>(response.getResult());
            });
        });
    }

    std::vector<CAmount> getTxFees() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->getTxFeesRequest().send().then([](auto response) {
                std::vector<CAmount> fees;
                fees.reserve(response.getResult().size());
                for (const int64_t fee : response.getResult()) {
                    fees.push_back(fee);
                }
                return fees;
            });
        });
    }

    std::vector<int64_t> getTxSigops() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->getTxSigopsRequest().send().then([](auto response) {
                std::vector<int64_t> sigops;
                sigops.reserve(response.getResult().size());
                for (const int64_t value : response.getResult()) {
                    sigops.push_back(value);
                }
                return sigops;
            });
        });
    }

    node::CoinbaseTx getCoinbaseTx() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->getCoinbaseTxRequest().send().then([](auto response) {
                return ReadCoinbaseTx(response.getResult());
            });
        });
    }

    std::vector<uint256> getCoinbaseMerklePath() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->getCoinbaseMerklePathRequest().send().then([](auto response) {
                std::vector<uint256> path;
                path.reserve(response.getResult().size());
                for (auto hash : response.getResult()) {
                    path.push_back(ReadUint256(hash));
                }
                return path;
            });
        });
    }

    std::unique_ptr<interfaces::Handler> submitSolutionAsync(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase, SubmitSolutionFn fn) override
    {
        auto active{std::make_shared<std::atomic<bool>>(true)};
        auto client{m_client};
        auto connection{m_connection};
        connection->AddTaskSync([connection, client, active, version, timestamp, nonce, coinbase = std::move(coinbase), fn = std::move(fn)](ClientThreadContext&) mutable -> kj::Promise<void> {
            if (!active->load(std::memory_order_acquire)) return kj::READY_NOW;
            auto request{client->client->submitSolutionRequest()};
            request.setVersion(version);
            request.setTimestamp(timestamp);
            request.setNonce(nonce);
            const auto coinbase_bytes{ipc::capnp::SerializeBytes(*coinbase)};
            SetBytes(request.initCoinbase(coinbase_bytes.size()), coinbase_bytes);
            return request.send().then([connection, active, fn = std::move(fn)](auto response) mutable {
                const bool accepted{response.getResult()};
                connection->PostCallback([active, fn = std::move(fn), accepted]() mutable {
                    if (active->exchange(false, std::memory_order_acq_rel)) {
                        fn(accepted);
                    }
                });
            });
        });
        return interfaces::MakeCleanupHandler([connection = std::move(connection), active] {
            active->store(false, std::memory_order_release);
        });
    }

    std::unique_ptr<interfaces::Handler> watchNext(node::BlockWaitOptions options, NextTemplateFn fn) override
    {
        auto client{m_client};
        auto connection{m_connection};
        auto state{std::make_shared<RemoteWatchState>(connection->MakeClient<messages::Watch::Client>())};
        connection->AddTaskSync([connection, client, state, options, fn = std::move(fn)](ClientThreadContext&) mutable -> kj::Promise<void> {
            if (!state->active.load(std::memory_order_acquire)) return kj::READY_NOW;
            auto request{client->client->watchNextRequest()};
            SetBlockWaitOptions(request.getOptions(), options);
            request.setListener(messages::BlockTemplateListener::Client(kj::heap<RemoteTemplateListener>(connection, state, std::move(fn))));
            return request.send().then([state](auto response) mutable -> kj::Promise<void> {
                state->watch->client.emplace(response.getWatch());
                if (!state->active.load(std::memory_order_acquire)) {
                    return CancelRemoteWatch(state);
                }
                return kj::READY_NOW;
            });
        });
        return interfaces::MakeCleanupHandler([connection = std::move(connection), state] {
            if (!state->active.exchange(false, std::memory_order_acq_rel)) return;
            try {
                connection->AddTaskSync([state](ClientThreadContext&) -> kj::Promise<void> {
                    return CancelRemoteWatch(state);
                });
            } catch (...) {
            }
        });
    }

private:
    std::shared_ptr<ClientConnection> m_connection;
    ClientConnection::ClientPtr<messages::BlockTemplate::Client> m_client;
};

std::unique_ptr<interfaces::BlockTemplate> MakeRemoteBlockTemplate(std::shared_ptr<ClientConnection> connection, ClientConnection::ClientPtr<messages::BlockTemplate::Client> client)
{
    return std::make_unique<RemoteBlockTemplate>(std::move(connection), std::move(client));
}

class RemoteMining final : public interfaces::Mining
{
public:
    RemoteMining(std::shared_ptr<ClientConnection> connection, ClientConnection::ClientPtr<messages::Mining::Client> client)
        : m_connection(std::move(connection)), m_client(std::move(client))
    {
    }
    ~RemoteMining() noexcept override {}

    bool isTestChain() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->isTestChainRequest().send().then([](auto response) {
                return response.getResult();
            });
        });
    }

    bool isInitialBlockDownload() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->isInitialBlockDownloadRequest().send().then([](auto response) {
                return response.getResult();
            });
        });
    }

    std::optional<interfaces::BlockRef> getTip() override
    {
        return m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->getTipRequest().send().then([](auto response) -> std::optional<interfaces::BlockRef> {
                if (!response.getFound()) return std::nullopt;
                return ReadBlockRef(response.getResult());
            });
        });
    }

    std::unique_ptr<interfaces::Handler> watchTip(uint256 current_tip, MillisecondsDouble timeout, TipChangedFn fn) override
    {
        auto client{m_client};
        auto connection{m_connection};
        auto state{std::make_shared<RemoteWatchState>(connection->MakeClient<messages::Watch::Client>())};
        connection->AddTaskSync([connection, client, state, current_tip, timeout, fn = std::move(fn)](ClientThreadContext&) mutable -> kj::Promise<void> {
            if (!state->active.load(std::memory_order_acquire)) return kj::READY_NOW;
            auto request{client->client->watchTipRequest()};
            SetBytes(request.initCurrentTip(uint256::size()), {current_tip.data(), current_tip.size()});
            request.setTimeout(timeout.count());
            request.setListener(messages::TipListener::Client(kj::heap<RemoteTipListener>(connection, state, std::move(fn))));
            return request.send().then([state](auto response) mutable -> kj::Promise<void> {
                state->watch->client.emplace(response.getWatch());
                if (!state->active.load(std::memory_order_acquire)) {
                    return CancelRemoteWatch(state);
                }
                return kj::READY_NOW;
            });
        });
        return interfaces::MakeCleanupHandler([connection = std::move(connection), state] {
            if (!state->active.exchange(false, std::memory_order_acq_rel)) return;
            try {
                connection->AddTaskSync([state](ClientThreadContext&) -> kj::Promise<void> {
                    return CancelRemoteWatch(state);
                });
            } catch (...) {
            }
        });
    }

    std::unique_ptr<interfaces::Handler> createNewBlockAsync(const node::BlockCreateOptions& options, bool cooldown, CreateBlockFn fn) override
    {
        auto active{std::make_shared<std::atomic<bool>>(true)};
        auto client{m_client};
        auto connection{m_connection};
        connection->AddTaskSync([connection, client, active, options, cooldown, fn = std::move(fn)](ClientThreadContext&) mutable -> kj::Promise<void> {
            if (!active->load(std::memory_order_acquire)) return kj::READY_NOW;
            auto request{client->client->createNewBlockRequest()};
            SetBlockCreateOptions(request.getOptions(), options);
            request.setCooldown(cooldown);
            return request.send().then([connection, active, fn = std::move(fn)](auto response) mutable {
                if (!response.getFound()) {
                    connection->PostCallback([active, fn = std::move(fn)]() mutable {
                        if (active->exchange(false, std::memory_order_acq_rel)) {
                            fn(CreateBlockResult{nullptr});
                        }
                    });
                    return;
                }
                auto block_template{connection->MakeClient<messages::BlockTemplate::Client>()};
                block_template->client.emplace(response.getResult());
                auto result{std::make_shared<std::unique_ptr<interfaces::BlockTemplate>>(MakeRemoteBlockTemplate(connection, std::move(block_template)))};
                connection->PostCallback([active, fn = std::move(fn), result]() mutable {
                    if (active->exchange(false, std::memory_order_acq_rel)) {
                        fn(CreateBlockResult{std::move(*result)});
                    }
                });
            });
        });
        return interfaces::MakeCleanupHandler([connection = std::move(connection), active] {
            active->store(false, std::memory_order_release);
        });
    }

    std::unique_ptr<interfaces::Handler> checkBlockAsync(CBlock block, node::BlockCheckOptions options, CheckBlockFn fn) override
    {
        auto active{std::make_shared<std::atomic<bool>>(true)};
        auto client{m_client};
        auto connection{m_connection};
        connection->AddTaskSync([connection, client, active, block = std::move(block), options, fn = std::move(fn)](ClientThreadContext&) mutable -> kj::Promise<void> {
            if (!active->load(std::memory_order_acquire)) return kj::READY_NOW;
            auto request{client->client->checkBlockRequest()};
            const auto block_bytes{ipc::capnp::SerializeBytes(block)};
            SetBytes(request.initBlock(block_bytes.size()), block_bytes);
            SetBlockCheckOptions(request.getOptions(), options);
            return request.send().then([connection, active, fn = std::move(fn)](auto response) mutable {
                const bool valid{response.getResult()};
                const std::string reason{response.getReason().cStr()};
                const std::string debug{response.getDebug().cStr()};
                connection->PostCallback([active, fn = std::move(fn), valid, reason = std::move(reason), debug = std::move(debug)]() mutable {
                    if (active->exchange(false, std::memory_order_acq_rel)) {
                        fn(valid, std::move(reason), std::move(debug));
                    }
                });
            });
        });
        return interfaces::MakeCleanupHandler([connection = std::move(connection), active] {
            active->store(false, std::memory_order_release);
        });
    }

    std::unique_ptr<interfaces::Handler> submitBlockAsync(CBlock block, SubmitBlockFn fn) override
    {
        auto active{std::make_shared<std::atomic<bool>>(true)};
        auto client{m_client};
        auto connection{m_connection};
        connection->AddTaskSync([connection, client, active, block = std::move(block), fn = std::move(fn)](ClientThreadContext&) mutable -> kj::Promise<void> {
            if (!active->load(std::memory_order_acquire)) return kj::READY_NOW;
            auto request{client->client->submitBlockRequest()};
            const auto block_bytes{ipc::capnp::SerializeBytes(block)};
            SetBytes(request.initBlock(block_bytes.size()), block_bytes);
            return request.send().then([connection, active, fn = std::move(fn)](auto response) mutable {
                const bool accepted{response.getResult()};
                const std::string reason{response.getReason().cStr()};
                const std::string debug{response.getDebug().cStr()};
                connection->PostCallback([active, fn = std::move(fn), accepted, reason = std::move(reason), debug = std::move(debug)]() mutable {
                    if (active->exchange(false, std::memory_order_acq_rel)) {
                        fn(accepted, std::move(reason), std::move(debug));
                    }
                });
            });
        });
        return interfaces::MakeCleanupHandler([connection = std::move(connection), active] {
            active->store(false, std::memory_order_release);
        });
    }

private:
    std::shared_ptr<ClientConnection> m_connection;
    ClientConnection::ClientPtr<messages::Mining::Client> m_client;
};

class RemoteEcho final : public interfaces::Echo
{
public:
    RemoteEcho(std::shared_ptr<ClientConnection> connection, ClientConnection::ClientPtr<messages::Echo::Client> client)
        : m_connection(std::move(connection)), m_client(std::move(client))
    {
    }
    ~RemoteEcho() noexcept override {}

    std::string echo(const std::string& echo) override
    {
        return m_connection->Call([client = m_client, echo](ClientThreadContext& context) {
            auto request{client->client->echoRequest()};
            request.setMessage(echo);
            return request.send().then([](auto response) {
                return std::string{response.getMessage().cStr()};
            });
        });
    }

private:
    std::shared_ptr<ClientConnection> m_connection;
    ClientConnection::ClientPtr<messages::Echo::Client> m_client;
};

class RemoteInit final : public interfaces::Init
{
public:
    RemoteInit(std::shared_ptr<ClientConnection> connection, ClientConnection::ClientPtr<messages::Init::Client> client)
        : m_connection(std::move(connection)), m_client(std::move(client))
    {
    }
    ~RemoteInit() noexcept override {}

    std::unique_ptr<interfaces::Mining> makeMining() override
    {
        auto mining{m_connection->MakeClient<messages::Mining::Client>()};
        m_connection->Call([init = m_client, mining](ClientThreadContext& context) {
            return init->client->makeMiningRequest().send().then([mining](auto response) {
                mining->client.emplace(response.getResult());
            });
        });
        return std::make_unique<RemoteMining>(m_connection, std::move(mining));
    }

    std::unique_ptr<interfaces::Echo> makeEcho() override
    {
        auto echo{m_connection->MakeClient<messages::Echo::Client>()};
        m_connection->Call([init = m_client, echo](ClientThreadContext& context) {
            return init->client->makeEchoRequest().send().then([echo](auto response) {
                echo->client.emplace(response.getResult());
            });
        });
        return std::make_unique<RemoteEcho>(m_connection, std::move(echo));
    }

    void stop() override
    {
        m_connection->Call([client = m_client](ClientThreadContext& context) {
            return client->client->stopRequest().send().ignoreResult();
        });
    }

private:
    std::shared_ptr<ClientConnection> m_connection;
    ClientConnection::ClientPtr<messages::Init::Client> m_client;
};

struct BlockCheckResult {
    bool valid{false};
    std::string reason;
    std::string debug;
};

struct SubmitBlockResult {
    bool accepted{false};
    std::string reason;
    std::string debug;
};

class WatchServer final : public messages::Watch::Server
{
public:
    WatchServer(std::shared_ptr<std::atomic<bool>> active, std::function<void()> cancel) : m_active(std::move(active)), m_cancel(std::move(cancel)) {}
    ~WatchServer() noexcept { Cancel(); }

    kj::Promise<void> cancel(CancelContext) override
    {
        Cancel();
        return kj::READY_NOW;
    }

private:
    void Cancel() noexcept
    {
        if (m_active->exchange(false, std::memory_order_acq_rel)) {
            m_cancel();
        }
    }

    std::shared_ptr<std::atomic<bool>> m_active;
    std::function<void()> m_cancel;
};

kj::Promise<void> NotifyTipChanged(messages::TipListener::Client listener, const interfaces::BlockRef& tip)
{
    auto request{listener.tipChangedRequest()};
    SetBlockRef(request.getTip(), tip);
    return request.send().ignoreResult();
}

kj::Promise<void> NotifyTipStopped(messages::TipListener::Client listener)
{
    return listener.stoppedRequest().send().ignoreResult();
}

class BlockTemplateServer final : public messages::BlockTemplate::Server
{
public:
    explicit BlockTemplateServer(std::unique_ptr<interfaces::BlockTemplate> block_template)
        : m_block_template(std::move(block_template))
    {
    }

    kj::Promise<void> getBlockHeader(GetBlockHeaderContext context) override
    {
        const auto block_header_bytes{ipc::capnp::SerializeBytes(m_block_template->getBlockHeader())};
        SetBytes(context.getResults().initResult(block_header_bytes.size()), block_header_bytes);
        return kj::READY_NOW;
    }

    kj::Promise<void> getBlock(GetBlockContext context) override
    {
        const auto block_bytes{ipc::capnp::SerializeBytes(m_block_template->getBlock())};
        SetBytes(context.getResults().initResult(block_bytes.size()), block_bytes);
        return kj::READY_NOW;
    }

    kj::Promise<void> getTxFees(GetTxFeesContext context) override
    {
        const auto fees{m_block_template->getTxFees()};
        auto result{context.getResults().initResult(fees.size())};
        for (size_t i{0}; i < fees.size(); ++i)
            result.set(i, fees[i]);
        return kj::READY_NOW;
    }

    kj::Promise<void> getTxSigops(GetTxSigopsContext context) override
    {
        const auto sigops{m_block_template->getTxSigops()};
        auto result{context.getResults().initResult(sigops.size())};
        for (size_t i{0}; i < sigops.size(); ++i)
            result.set(i, sigops[i]);
        return kj::READY_NOW;
    }

    kj::Promise<void> getCoinbaseTx(GetCoinbaseTxContext context) override
    {
        SetCoinbaseTx(context.getResults().getResult(), m_block_template->getCoinbaseTx());
        return kj::READY_NOW;
    }

    kj::Promise<void> getCoinbaseMerklePath(GetCoinbaseMerklePathContext context) override
    {
        const auto path{m_block_template->getCoinbaseMerklePath()};
        auto result{context.getResults().initResult(path.size())};
        for (size_t i{0}; i < path.size(); ++i) {
            SetBytes(result.init(i, uint256::size()), {path[i].data(), path[i].size()});
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> submitSolution(SubmitSolutionContext context) override
    {
        const auto params{context.getParams()};
        const uint32_t version{params.getVersion()};
        const uint32_t timestamp{params.getTimestamp()};
        const uint32_t nonce{params.getNonce()};
        CTransactionRef coinbase{MakeTransactionRef(ipc::capnp::DeserializeBytes<CTransaction>(params.getCoinbase()))};
        auto result{NodePromise<bool>([block_template = m_block_template, version, timestamp, nonce, coinbase = std::move(coinbase)](std::function<void(bool)> done) mutable {
            return block_template->submitSolutionAsync(version, timestamp, nonce, std::move(coinbase), std::move(done));
        })};
        return kj::mv(result).then([KJ_CPCAP(context)](bool result) mutable {
            context.getResults().setResult(result);
        });
    }

    kj::Promise<void> watchNext(WatchNextContext context) override
    {
        auto active{std::make_shared<std::atomic<bool>>(true)};
        auto options{ReadBlockWaitOptions(context.getParams().getOptions())};
        auto listener{context.getParams().getListener()};
        auto handler{std::make_shared<std::unique_ptr<interfaces::Handler>>()};

        context.getResults().setWatch(kj::heap<WatchServer>(active, [handler] {
            if (*handler) (*handler)->disconnect();
        }));

        auto next_template{std::make_shared<std::unique_ptr<interfaces::BlockTemplate>>()};
        auto pair{kj::newPromiseAndCrossThreadFulfiller<bool>()};
        auto fulfiller{std::make_shared<kj::Own<kj::CrossThreadPromiseFulfiller<bool>>>(kj::mv(pair.fulfiller))};
        *handler = m_block_template->watchNext(options, [next_template, fulfiller](std::unique_ptr<interfaces::BlockTemplate> block_template) mutable {
            if (*fulfiller) {
                *next_template = std::move(block_template);
                (*fulfiller)->fulfill(static_cast<bool>(*next_template));
                *fulfiller = nullptr;
            }
        });
        auto next_template_keepalive{next_template};
        auto next{kj::mv(pair.promise).attach(std::move(handler), std::move(fulfiller), std::move(next_template_keepalive))};
        m_tasks.add(kj::mv(next).then([active, listener, next_template](bool found) mutable -> kj::Promise<void> {
            if (!active->exchange(false, std::memory_order_acq_rel)) return kj::READY_NOW;
            if (!found) return listener.stoppedRequest().send().ignoreResult();
            auto request{listener.templateReadyRequest()};
            request.setResult(kj::heap<BlockTemplateServer>(std::move(*next_template)));
            return request.send().ignoreResult();
        }));
        return kj::READY_NOW;
    }

private:
    std::shared_ptr<interfaces::BlockTemplate> m_block_template;
    TaskErrorLogger m_task_errors;
    kj::TaskSet m_tasks{m_task_errors};
};

class MiningServer final : public messages::Mining::Server
{
public:
    explicit MiningServer(std::unique_ptr<interfaces::Mining> mining) : m_mining(std::move(mining)) {}

    kj::Promise<void> isTestChain(IsTestChainContext context) override
    {
        context.getResults().setResult(m_mining->isTestChain());
        return kj::READY_NOW;
    }

    kj::Promise<void> isInitialBlockDownload(IsInitialBlockDownloadContext context) override
    {
        context.getResults().setResult(m_mining->isInitialBlockDownload());
        return kj::READY_NOW;
    }

    kj::Promise<void> getTip(GetTipContext context) override
    {
        const auto tip{m_mining->getTip()};
        context.getResults().setFound(tip.has_value());
        if (tip) SetBlockRef(context.getResults().getResult(), *tip);
        return kj::READY_NOW;
    }

    kj::Promise<void> createNewBlock(CreateNewBlockContext context) override
    {
        const auto params{context.getParams()};
        auto options{ReadBlockCreateOptions(params.getOptions())};
        const bool cooldown{params.getCooldown()};
        auto block_template{std::make_shared<std::unique_ptr<interfaces::BlockTemplate>>()};
        auto pair{kj::newPromiseAndCrossThreadFulfiller<bool>()};
        auto fulfiller{std::make_shared<kj::Own<kj::CrossThreadPromiseFulfiller<bool>>>(kj::mv(pair.fulfiller))};
        auto handler{m_mining->createNewBlockAsync(options, cooldown, [block_template, fulfiller](interfaces::Mining::CreateBlockResult result) mutable {
            if (!*fulfiller) return;
            if (result) {
                *block_template = std::move(*result);
                (*fulfiller)->fulfill(static_cast<bool>(*block_template));
            } else {
                (*fulfiller)->reject(KJ_EXCEPTION(FAILED, util::ErrorString(result).original));
            }
            *fulfiller = nullptr;
        })};
        auto block_template_keepalive{block_template};
        auto found{kj::mv(pair.promise).attach(std::move(handler), std::move(fulfiller), std::move(block_template_keepalive))};
        return kj::mv(found).then([KJ_CPCAP(context), block_template](bool found) mutable {
            context.getResults().setFound(found);
            if (found) {
                context.getResults().setResult(kj::heap<BlockTemplateServer>(std::move(*block_template)));
            }
        });
    }

    kj::Promise<void> checkBlock(CheckBlockContext context) override
    {
        const auto params{context.getParams()};
        auto block{ipc::capnp::DeserializeBytes<CBlock>(params.getBlock())};
        auto options{ReadBlockCheckOptions(params.getOptions())};
        auto result{NodePromise<BlockCheckResult>([mining = m_mining, block = std::move(block), options](std::function<void(BlockCheckResult)> done) mutable {
            return mining->checkBlockAsync(std::move(block), options, [done = std::move(done)](bool valid, std::string reason, std::string debug) mutable {
                done({valid, std::move(reason), std::move(debug)});
            });
        })};
        return kj::mv(result).then([KJ_CPCAP(context)](BlockCheckResult result) mutable {
            context.getResults().setReason(result.reason);
            context.getResults().setDebug(result.debug);
            context.getResults().setResult(result.valid);
        });
    }

    kj::Promise<void> submitBlock(SubmitBlockContext context) override
    {
        const auto params{context.getParams()};
        auto block{ipc::capnp::DeserializeBytes<CBlock>(params.getBlock())};
        auto result{NodePromise<SubmitBlockResult>([mining = m_mining, block = std::move(block)](std::function<void(SubmitBlockResult)> done) mutable {
            return mining->submitBlockAsync(std::move(block), [done = std::move(done)](bool accepted, std::string reason, std::string debug) mutable {
                done({accepted, std::move(reason), std::move(debug)});
            });
        })};
        return kj::mv(result).then([KJ_CPCAP(context)](SubmitBlockResult result) mutable {
            context.getResults().setReason(result.reason);
            context.getResults().setDebug(result.debug);
            context.getResults().setResult(result.accepted);
        });
    }

    kj::Promise<void> watchTip(WatchTipContext context) override
    {
        const auto params{context.getParams()};
        auto active{std::make_shared<std::atomic<bool>>(true)};
        auto listener{params.getListener()};
        const auto current_tip{ReadUint256(params.getCurrentTip())};
        const MillisecondsDouble timeout{params.getTimeout()};
        auto handler{std::make_shared<std::unique_ptr<interfaces::Handler>>()};

        context.getResults().setWatch(kj::heap<WatchServer>(active, [handler] {
            if (*handler) (*handler)->disconnect();
        }));

        auto pair{kj::newPromiseAndCrossThreadFulfiller<std::optional<interfaces::BlockRef>>()};
        auto fulfiller{std::make_shared<kj::Own<kj::CrossThreadPromiseFulfiller<std::optional<interfaces::BlockRef>>>>(kj::mv(pair.fulfiller))};
        *handler = m_mining->watchTip(current_tip, timeout, [fulfiller](std::optional<interfaces::BlockRef> tip) mutable {
            if (*fulfiller) {
                (*fulfiller)->fulfill(std::move(tip));
                *fulfiller = nullptr;
            }
        });
        auto tip{kj::mv(pair.promise).attach(std::move(handler), std::move(fulfiller))};
        m_tasks.add(kj::mv(tip).then([active, listener](std::optional<interfaces::BlockRef> tip) mutable -> kj::Promise<void> {
            if (!active->exchange(false, std::memory_order_acq_rel)) return kj::READY_NOW;
            if (!tip) return NotifyTipStopped(listener);
            return NotifyTipChanged(listener, *tip);
        }));
        return kj::READY_NOW;
    }

private:
    std::shared_ptr<interfaces::Mining> m_mining;
    TaskErrorLogger m_task_errors;
    kj::TaskSet m_tasks{m_task_errors};
};

class EchoServer final : public messages::Echo::Server
{
public:
    explicit EchoServer(std::unique_ptr<interfaces::Echo> echo) : m_echo(std::move(echo)) {}

    kj::Promise<void> echo(EchoContext context) override
    {
        context.getResults().setMessage(m_echo->echo(context.getParams().getMessage().cStr()));
        return kj::READY_NOW;
    }

private:
    std::unique_ptr<interfaces::Echo> m_echo;
};

class InitServer final : public messages::Init::Server
{
public:
    explicit InitServer(interfaces::Init& init) : m_init(init) {}

    kj::Promise<void> makeEcho(MakeEchoContext context) override
    {
        if (auto echo{m_init.makeEcho()}) {
            context.getResults().setResult(kj::heap<EchoServer>(std::move(echo)));
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> makeMining(MakeMiningContext context) override
    {
        if (auto mining{m_init.makeMining()}) {
            context.getResults().setResult(kj::heap<MiningServer>(std::move(mining)));
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> stop(StopContext) override
    {
        m_init.stop();
        return kj::READY_NOW;
    }

private:
    interfaces::Init& m_init;
};

void ServeConnection(int fd, const char* exe_name, interfaces::Init& init, const std::function<void()>& ready_fn = {})
{
    auto io{kj::setupAsyncIo()};
    auto stream{io.lowLevelProvider->wrapSocketFd(fd, kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP)};
    ::capnp::TwoPartyVatNetwork network{*stream, ::capnp::rpc::twoparty::Side::SERVER, ::capnp::ReaderOptions{}};
    auto rpc_system{::capnp::makeRpcServer(network, messages::Init::Client(kj::heap<InitServer>(init)))};
    if (ready_fn) ready_fn();
    network.onDisconnect().wait(io.waitScope);
}

class CapnpProtocol final : public Protocol
{
    class SharedFd
    {
    public:
        explicit SharedFd(int fd) : m_fd(fd) {}
        ~SharedFd() { Close(); }

        int fd() const { return m_fd.load(); }
        void Shutdown() const
        {
            const int fd{m_fd.load()};
            if (fd >= 0) (void)::shutdown(fd, SHUT_RDWR);
        }
        void Close() const
        {
            const int fd{m_fd.exchange(-1)};
            if (fd >= 0) (void)::close(fd);
        }

    private:
        mutable std::atomic<int> m_fd;
    };
    struct Listener {
        int fd{-1};
        std::thread thread;
    };
    struct ConnectionThread {
        std::shared_ptr<SharedFd> shutdown_fd;
        std::thread thread;
    };

public:
    ~CapnpProtocol() noexcept override { disconnectIncoming(); }

    std::unique_ptr<interfaces::Init> connect(int fd, const char*) override
    {
        auto connection{std::make_shared<ClientConnection>(fd)};
        return std::make_unique<RemoteInit>(connection, connection->init());
    }

    void listen(int listen_fd, const char* exe_name, interfaces::Init& init) override
    {
        if (::listen(listen_fd, /*backlog=*/5) != 0) {
            throw std::system_error(errno, std::system_category());
        }
        std::thread accept_thread{[this, listen_fd, exe_name, &init] {
            util::ThreadRename("capnp-accept");
            while (!m_stop.load()) {
                const int fd{::accept(listen_fd, nullptr, nullptr)};
                if (fd < 0) {
                    if (m_stop.load() || errno == EBADF || errno == EINVAL) return;
                    LogWarning("ipc: accept failed: %s\n", SysErrorString(errno));
                    continue;
                }
                auto shutdown_fd{std::make_shared<SharedFd>(::dup(fd))};
                if (shutdown_fd->fd() < 0) {
                    (void)::close(fd);
                    LogWarning("ipc: dup failed: %s\n", SysErrorString(errno));
                    continue;
                }
                std::lock_guard lock{m_mutex};
                m_connection_threads.push_back({.shutdown_fd = shutdown_fd,
                                                .thread = std::thread([fd, shutdown_fd, exe_name, &init] {
                                                    util::ThreadRename("capnp-serve");
                                                    try {
                                                        ServeConnection(fd, exe_name, init);
                                                    } catch (const kj::Exception& e) {
                                                        LogDebug(BCLog::IPC, "IPC connection closed: %s\n", e.getDescription().cStr());
                                                    } catch (const std::exception& e) {
                                                        LogWarning("ipc: connection failed: %s\n", e.what());
                                                    }
                                                    shutdown_fd->Close();
                                                })});
            }
        }};
        std::lock_guard lock{m_mutex};
        m_listeners.push_back({.fd = listen_fd, .thread = std::move(accept_thread)});
    }

    void serve(int fd, const char* exe_name, interfaces::Init& init, const std::function<void()>& ready_fn = {}) override
    {
        util::ThreadRename(exe_name);
        ServeConnection(fd, exe_name, init, ready_fn);
    }

    void disconnectIncoming() override
    {
        const bool was_stopped{m_stop.exchange(true)};
        if (was_stopped) return;
        std::vector<Listener> listeners;
        {
            std::lock_guard lock{m_mutex};
            listeners.swap(m_listeners);
        }
        for (auto& listener : listeners) {
            (void)::shutdown(listener.fd, SHUT_RDWR);
            (void)::close(listener.fd);
        }
        for (auto& listener : listeners) {
            if (listener.thread.joinable()) listener.thread.join();
        }
        std::vector<ConnectionThread> threads;
        {
            std::lock_guard lock{m_mutex};
            threads.swap(m_connection_threads);
        }
        for (auto& thread : threads) {
            thread.shutdown_fd->Shutdown();
            thread.shutdown_fd->Close();
        }
        for (auto& thread : threads) {
            if (thread.thread.joinable()) thread.thread.join();
        }
    }

    Context& context() override { return m_context; }

private:
    Context m_context;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    std::vector<Listener> m_listeners;
    std::vector<ConnectionThread> m_connection_threads;
};
} // namespace

std::unique_ptr<Protocol> MakeCapnpProtocol() { return std::make_unique<CapnpProtocol>(); }
} // namespace capnp
} // namespace ipc
