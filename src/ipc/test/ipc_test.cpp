// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ipc/test/ipc_test.h>

#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>
#include <interfaces/init.h>
#include <interfaces/mining.h>
#include <ipc/capnp/common-types.h>
#include <ipc/capnp/init.capnp.h>
#include <ipc/capnp/mining.capnp.h>
#include <ipc/capnp/protocol.h>
#include <ipc/process.h>
#include <ipc/protocol.h>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <tinyformat.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>

static_assert(ipc::capnp::messages::MAX_MONEY == MAX_MONEY);
static_assert(ipc::capnp::messages::MAX_DOUBLE == std::numeric_limits<double>::max());
static_assert(ipc::capnp::messages::DEFAULT_BLOCK_RESERVED_WEIGHT == DEFAULT_BLOCK_RESERVED_WEIGHT);
static_assert(ipc::capnp::messages::DEFAULT_COINBASE_OUTPUT_MAX_ADDITIONAL_SIGOPS == DEFAULT_COINBASE_OUTPUT_MAX_ADDITIONAL_SIGOPS);

namespace {

namespace messages = ipc::capnp::messages;

::capnp::rpc::twoparty::VatId::Reader ServerVatId(::capnp::MallocMessageBuilder& message)
{
    auto vat_id{message.getRoot<::capnp::rpc::twoparty::VatId>()};
    vat_id.setSide(::capnp::rpc::twoparty::Side::SERVER);
    return vat_id.asReader();
}

void SetData(::capnp::Data::Builder output, std::span<const unsigned char> bytes)
{
    std::copy(bytes.begin(), bytes.end(), output.begin());
}

uint256 ReadUint256(::capnp::Data::Reader data)
{
    BOOST_REQUIRE_EQUAL(data.size(), uint256::size());
    uint256 value;
    std::copy(data.begin(), data.end(), value.begin());
    return value;
}

interfaces::BlockRef ReadBlockRef(messages::BlockRef::Reader reader)
{
    return {.hash = ReadUint256(reader.getHash()), .height = reader.getHeight()};
}

struct TestMiningState {
    std::mutex mutex;
    std::condition_variable cv;
    int cancellations{0};
};

class TestBlockTemplate final : public interfaces::BlockTemplate
{
public:
    CBlockHeader getBlockHeader() override { return {}; }
    CBlock getBlock() override { return {}; }
    std::vector<CAmount> getTxFees() override { return {12, 34}; }
    std::vector<int64_t> getTxSigops() override { return {1, 2}; }
    node::CoinbaseTx getCoinbaseTx() override
    {
        return {
            .version = 1,
            .sequence = CTxIn::SEQUENCE_FINAL,
            .script_sig_prefix = {},
            .witness = std::nullopt,
            .block_reward_remaining = 0,
            .required_outputs = {},
            .lock_time = 0,
        };
    }
    std::vector<uint256> getCoinbaseMerklePath() override { return {}; }
    std::unique_ptr<interfaces::Handler> submitSolutionAsync(uint32_t, uint32_t, uint32_t, CTransactionRef, SubmitSolutionFn fn) override
    {
        fn(true);
        return interfaces::MakeCleanupHandler([] {});
    }
    std::unique_ptr<interfaces::Handler> watchNext(node::BlockWaitOptions, NextTemplateFn fn) override
    {
        fn(std::make_unique<TestBlockTemplate>());
        return interfaces::MakeCleanupHandler([] {});
    }
};

class TestMining final : public interfaces::Mining
{
public:
    explicit TestMining(std::shared_ptr<TestMiningState> state) : m_state(std::move(state)) {}

    bool isTestChain() override { return true; }
    bool isInitialBlockDownload() override { return false; }

    std::optional<interfaces::BlockRef> getTip() override
    {
        return interfaces::BlockRef{.hash = uint256::ONE, .height = 7};
    }

    std::unique_ptr<interfaces::Handler> watchTip(uint256 current_tip, MillisecondsDouble, TipChangedFn fn) override
    {
        if (current_tip != uint256::ONE) fn(getTip());
        return interfaces::MakeCleanupHandler([state = m_state] {
            {
                std::lock_guard lock{state->mutex};
                ++state->cancellations;
            }
            state->cv.notify_all();
        });
    }

    std::unique_ptr<interfaces::Handler> createNewBlockAsync(const node::BlockCreateOptions&, bool, CreateBlockFn fn) override
    {
        fn(CreateBlockResult{std::make_unique<TestBlockTemplate>()});
        return interfaces::MakeCleanupHandler([] {});
    }

    std::unique_ptr<interfaces::Handler> checkBlockAsync(CBlock, node::BlockCheckOptions, CheckBlockFn fn) override
    {
        fn(true, "accepted", "checked");
        return interfaces::MakeCleanupHandler([] {});
    }

    std::unique_ptr<interfaces::Handler> submitBlockAsync(CBlock, SubmitBlockFn fn) override
    {
        fn(true, "submitted", "processed");
        return interfaces::MakeCleanupHandler([] {});
    }

private:
    std::shared_ptr<TestMiningState> m_state;
};

//! Remote init class.
namespace {
class TestInit : public interfaces::Init
{
public:
    std::atomic<bool> stop_called{false};
    std::shared_ptr<TestMiningState> mining_state{std::make_shared<TestMiningState>()};
    std::unique_ptr<interfaces::Echo> makeEcho() override { return interfaces::MakeEcho(); }
    std::unique_ptr<interfaces::Mining> makeMining() override { return std::make_unique<TestMining>(mining_state); }
    void stop() override { stop_called.store(true); }
};
} // namespace

class TestTipListener final : public messages::TipListener::Server
{
public:
    explicit TestTipListener(kj::Own<kj::PromiseFulfiller<interfaces::BlockRef>> fulfiller)
    {
        m_fulfiller.emplace(kj::mv(fulfiller));
    }

    kj::Promise<void> tipChanged(TipChangedContext context) override
    {
        if (m_fulfiller) {
            (*m_fulfiller)->fulfill(ReadBlockRef(context.getParams().getTip()));
            m_fulfiller.reset();
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> stopped(StoppedContext) override
    {
        if (m_fulfiller) {
            (*m_fulfiller)->reject(KJ_EXCEPTION(FAILED, "tip watch stopped"));
            m_fulfiller.reset();
        }
        return kj::READY_NOW;
    }

private:
    std::optional<kj::Own<kj::PromiseFulfiller<interfaces::BlockRef>>> m_fulfiller;
};

class TestTemplateListener final : public messages::BlockTemplateListener::Server
{
public:
    explicit TestTemplateListener(kj::Own<kj::PromiseFulfiller<messages::BlockTemplate::Client>> fulfiller)
    {
        m_fulfiller.emplace(kj::mv(fulfiller));
    }

    kj::Promise<void> templateReady(TemplateReadyContext context) override
    {
        if (m_fulfiller) {
            (*m_fulfiller)->fulfill(context.getParams().getResult());
            m_fulfiller.reset();
        }
        return kj::READY_NOW;
    }

    kj::Promise<void> stopped(StoppedContext) override
    {
        if (m_fulfiller) {
            (*m_fulfiller)->reject(KJ_EXCEPTION(FAILED, "template watch stopped"));
            m_fulfiller.reset();
        }
        return kj::READY_NOW;
    }

private:
    std::optional<kj::Own<kj::PromiseFulfiller<messages::BlockTemplate::Client>>> m_fulfiller;
};

} // namespace

//! Generate a temporary path with temp_directory_path and mkstemp
static std::string TempPath(std::string_view pattern)
{
    std::string temp{fs::PathToString(fs::path{fs::temp_directory_path()} / fs::PathFromString(std::string{pattern}))};
    temp.push_back('\0');
    int fd{mkstemp(temp.data())};
    BOOST_CHECK_GE(fd, 0);
    BOOST_CHECK_EQUAL(close(fd), 0);
    temp.resize(temp.size() - 1);
    fs::remove(fs::PathFromString(temp));
    return temp;
}

//! Test ipc::Protocol connect() and serve() methods connecting over a socketpair.
void IpcSocketPairTest()
{
    int fds[2];
    BOOST_CHECK_EQUAL(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    std::unique_ptr<interfaces::Init> init{std::make_unique<TestInit>()};
    std::unique_ptr<ipc::Protocol> protocol{ipc::capnp::MakeCapnpProtocol()};
    std::promise<void> promise;
    std::thread thread([&]() {
        protocol->serve(fds[0], "test-serve", *init, [&] { promise.set_value(); });
    });
    promise.get_future().wait();
    std::unique_ptr<interfaces::Init> remote_init{protocol->connect(fds[1], "test-connect")};
    std::unique_ptr<interfaces::Echo> remote_echo{remote_init->makeEcho()};
    BOOST_CHECK_EQUAL(remote_echo->echo("echo test"), "echo test");
    remote_echo.reset();
    std::unique_ptr<interfaces::Mining> remote_mining{remote_init->makeMining()};
    BOOST_REQUIRE(remote_mining);
    BOOST_CHECK(remote_mining->isTestChain());
    BOOST_CHECK(!remote_mining->isInitialBlockDownload());
    const auto tip{remote_mining->getTip()};
    BOOST_REQUIRE(tip);
    BOOST_CHECK_EQUAL(tip->hash.ToString(), uint256::ONE.ToString());
    BOOST_CHECK_EQUAL(tip->height, 7);
    const auto test_thread{std::this_thread::get_id()};

    struct TipResult {
        std::thread::id callback_thread;
        std::optional<interfaces::BlockRef> tip;
    };
    std::promise<TipResult> async_tip_promise;
    auto tip_handler{remote_mining->watchTip(uint256::ZERO, MillisecondsDouble{0}, [&async_tip_promise](std::optional<interfaces::BlockRef> tip) {
        async_tip_promise.set_value({std::this_thread::get_id(), std::move(tip)});
    })};
    (void)tip_handler;
    const auto tip_result{async_tip_promise.get_future().get()};
    BOOST_CHECK(tip_result.callback_thread != test_thread);
    const auto& changed_tip{tip_result.tip};
    BOOST_REQUIRE(changed_tip);
    BOOST_CHECK_EQUAL(changed_tip->height, 7);

    struct CreateResult {
        std::thread::id callback_thread;
        std::unique_ptr<interfaces::BlockTemplate> block_template;
    };
    std::promise<CreateResult> create_promise;
    auto create_handler{remote_mining->createNewBlockAsync(node::BlockCreateOptions{}, false, [&create_promise](interfaces::Mining::CreateBlockResult result) {
        std::unique_ptr<interfaces::BlockTemplate> block_template;
        if (result) block_template = std::move(*result);
        create_promise.set_value({std::this_thread::get_id(), std::move(block_template)});
    })};
    (void)create_handler;
    auto create_result{create_promise.get_future().get()};
    BOOST_CHECK(create_result.callback_thread != test_thread);
    BOOST_REQUIRE(create_result.block_template);
    const auto fees{create_result.block_template->getTxFees()};
    BOOST_REQUIRE_EQUAL(fees.size(), 2);
    BOOST_CHECK_EQUAL(fees[0], 12);
    BOOST_CHECK_EQUAL(fees[1], 34);

    struct NextResult {
        std::thread::id callback_thread;
        std::unique_ptr<interfaces::BlockTemplate> block_template;
    };
    std::promise<NextResult> next_promise;
    auto next_handler{create_result.block_template->watchNext(node::BlockWaitOptions{}, [&next_promise](std::unique_ptr<interfaces::BlockTemplate> block_template) {
        next_promise.set_value({std::this_thread::get_id(), std::move(block_template)});
    })};
    (void)next_handler;
    auto next_result{next_promise.get_future().get()};
    BOOST_CHECK(next_result.callback_thread != test_thread);
    BOOST_REQUIRE(next_result.block_template);

    struct SubmitResult {
        std::thread::id callback_thread;
        bool accepted{false};
    };
    std::promise<SubmitResult> submit_promise;
    auto submit_handler{next_result.block_template->submitSolutionAsync(0, 0, 0, MakeTransactionRef(CMutableTransaction{}), [&submit_promise](bool accepted) {
        submit_promise.set_value({std::this_thread::get_id(), accepted});
    })};
    (void)submit_handler;
    const auto submit_result{submit_promise.get_future().get()};
    BOOST_CHECK(submit_result.callback_thread != test_thread);
    BOOST_CHECK(submit_result.accepted);

    struct CheckResult {
        std::thread::id callback_thread;
        bool valid{false};
        std::string reason;
        std::string debug;
    };
    std::promise<CheckResult> check_promise;
    auto check_handler{remote_mining->checkBlockAsync(CBlock{}, node::BlockCheckOptions{}, [&check_promise](bool valid, std::string reason, std::string debug) {
        check_promise.set_value({std::this_thread::get_id(), valid, std::move(reason), std::move(debug)});
    })};
    (void)check_handler;
    const auto check_result{check_promise.get_future().get()};
    BOOST_CHECK(check_result.callback_thread != test_thread);
    BOOST_CHECK(check_result.valid);
    BOOST_CHECK_EQUAL(check_result.reason, "accepted");
    BOOST_CHECK_EQUAL(check_result.debug, "checked");

    struct SubmitBlockResult {
        std::thread::id callback_thread;
        bool accepted{false};
        std::string reason;
        std::string debug;
    };
    std::promise<SubmitBlockResult> submit_block_promise;
    auto submit_block_handler{remote_mining->submitBlockAsync(CBlock{}, [&submit_block_promise](bool accepted, std::string reason, std::string debug) {
        submit_block_promise.set_value({std::this_thread::get_id(), accepted, std::move(reason), std::move(debug)});
    })};
    (void)submit_block_handler;
    const auto submit_block_result{submit_block_promise.get_future().get()};
    BOOST_CHECK(submit_block_result.callback_thread != test_thread);
    BOOST_CHECK(submit_block_result.accepted);
    BOOST_CHECK_EQUAL(submit_block_result.reason, "submitted");
    BOOST_CHECK_EQUAL(submit_block_result.debug, "processed");

    remote_mining.reset();
    remote_init->stop();
    BOOST_CHECK(static_cast<TestInit*>(init.get())->stop_called.load());
    submit_block_handler.reset();
    check_handler.reset();
    submit_handler.reset();
    next_handler.reset();
    create_handler.reset();
    tip_handler.reset();
    next_result.block_template.reset();
    create_result.block_template.reset();
    remote_init.reset();
    thread.join();
}

//! Test the native Cap'n Proto mining watch/cancel API without going through
//! the synchronous C++ compatibility wrapper.
void IpcNativeMiningWatchTest()
{
    int fds[2];
    BOOST_CHECK_EQUAL(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    auto init{std::make_unique<TestInit>()};
    auto* test_init{init.get()};
    std::unique_ptr<ipc::Protocol> protocol{ipc::capnp::MakeCapnpProtocol()};
    std::promise<void> promise;
    std::thread thread([&] {
        protocol->serve(fds[0], "test-serve", *init, [&] { promise.set_value(); });
    });
    promise.get_future().wait();

    {
        auto io{kj::setupAsyncIo()};
        auto stream{io.lowLevelProvider->wrapSocketFd(fds[1], kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP)};
        ::capnp::TwoPartyVatNetwork network{*stream, ::capnp::rpc::twoparty::Side::CLIENT, ::capnp::ReaderOptions{}};
        auto rpc_system{::capnp::makeRpcClient(network)};
        ::capnp::MallocMessageBuilder vat_message;
        auto init_client{rpc_system.bootstrap(ServerVatId(vat_message)).castAs<messages::Init>()};
        auto mining{init_client.makeMiningRequest().send().wait(io.waitScope).getResult()};

        auto tip_pair{kj::newPromiseAndFulfiller<interfaces::BlockRef>()};
        auto watch_tip{mining.watchTipRequest()};
        SetData(watch_tip.initCurrentTip(uint256::size()), {uint256::ZERO.data(), uint256::ZERO.size()});
        watch_tip.setListener(messages::TipListener::Client(kj::heap<TestTipListener>(kj::mv(tip_pair.fulfiller))));
        auto watch_response{watch_tip.send().wait(io.waitScope)};
        const auto tip{tip_pair.promise.wait(io.waitScope)};
        BOOST_CHECK_EQUAL(tip.hash.ToString(), uint256::ONE.ToString());
        BOOST_CHECK_EQUAL(tip.height, 7);

        auto cancel_watch{mining.watchTipRequest()};
        SetData(cancel_watch.initCurrentTip(uint256::size()), {uint256::ONE.data(), uint256::ONE.size()});
        auto cancel_pair{kj::newPromiseAndFulfiller<interfaces::BlockRef>()};
        cancel_watch.setListener(messages::TipListener::Client(kj::heap<TestTipListener>(kj::mv(cancel_pair.fulfiller))));
        auto cancel_response{cancel_watch.send().wait(io.waitScope)};
        cancel_response.getWatch().cancelRequest().send().wait(io.waitScope);
        {
            std::unique_lock lock{test_init->mining_state->mutex};
            BOOST_CHECK(test_init->mining_state->cv.wait_for(lock, std::chrono::seconds{5}, [&] {
                return test_init->mining_state->cancellations > 0;
            }));
        }

        watch_response.getWatch().cancelRequest().send().wait(io.waitScope);

        auto create_template{mining.createNewBlockRequest()};
        create_template.setCooldown(false);
        auto template_response{create_template.send().wait(io.waitScope)};
        BOOST_REQUIRE(template_response.getFound());
        auto template_pair{kj::newPromiseAndFulfiller<messages::BlockTemplate::Client>()};
        auto watch_template{template_response.getResult().watchNextRequest()};
        watch_template.setListener(messages::BlockTemplateListener::Client(kj::heap<TestTemplateListener>(kj::mv(template_pair.fulfiller))));
        auto template_watch_response{watch_template.send().wait(io.waitScope)};
        auto next_template{template_pair.promise.wait(io.waitScope)};
        auto fees_response{next_template.getTxFeesRequest().send().wait(io.waitScope)};
        auto fees{fees_response.getResult()};
        BOOST_REQUIRE_EQUAL(fees.size(), 2);
        BOOST_CHECK_EQUAL(fees[0], 12);
        BOOST_CHECK_EQUAL(fees[1], 34);
        template_watch_response.getWatch().cancelRequest().send().wait(io.waitScope);

        auto submit_block{mining.submitBlockRequest()};
        const auto block_bytes{ipc::capnp::SerializeBytes(CBlock{})};
        SetData(submit_block.initBlock(block_bytes.size()), block_bytes);
        auto submit_response{submit_block.send().wait(io.waitScope)};
        BOOST_CHECK(submit_response.getResult());
        BOOST_CHECK_EQUAL(submit_response.getReason().cStr(), "submitted");
        BOOST_CHECK_EQUAL(submit_response.getDebug().cStr(), "processed");

        init_client.stopRequest().send().wait(io.waitScope);
    }
    thread.join();
}

//! Test ipc::Process bind() and connect() methods connecting over a unix socket.
void IpcSocketTest(const fs::path& datadir)
{
    std::unique_ptr<interfaces::Init> init{std::make_unique<TestInit>()};
    std::unique_ptr<ipc::Protocol> protocol{ipc::capnp::MakeCapnpProtocol()};
    std::unique_ptr<ipc::Process> process{ipc::MakeProcess()};

    std::string invalid_bind{"invalid:"};
    BOOST_CHECK_THROW(process->bind(datadir, "test_bitcoin", invalid_bind), std::invalid_argument);
    BOOST_CHECK_THROW(process->connect(datadir, "test_bitcoin", invalid_bind), std::invalid_argument);

    auto bind_and_listen{[&](const std::string& bind_address) {
        std::string address{bind_address};
        int serve_fd = process->bind(datadir, "test_bitcoin", address);
        BOOST_CHECK_GE(serve_fd, 0);
        BOOST_CHECK_EQUAL(address, bind_address);
        protocol->listen(serve_fd, "test-serve", *init);
    }};

    auto connect_and_test{[&](const std::string& connect_address) {
        std::string address{connect_address};
        int connect_fd{process->connect(datadir, "test_bitcoin", address)};
        BOOST_CHECK_EQUAL(address, connect_address);
        std::unique_ptr<interfaces::Init> remote_init{protocol->connect(connect_fd, "test-connect")};
        std::unique_ptr<interfaces::Echo> remote_echo{remote_init->makeEcho()};
        BOOST_CHECK_EQUAL(remote_echo->echo("echo test"), "echo test");
    }};

    // Need to specify explicit socket addresses outside the data directory, because the data
    // directory path is so long that the default socket address and any other
    // addresses in the data directory would fail with errors like:
    //   Address 'unix' path '"/tmp/test_common_Bitcoin Core/ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff/test_bitcoin.sock"' exceeded maximum socket path length
    std::vector<std::string> addresses{
        strprintf("unix:%s", TempPath("bitcoin_sock0_XXXXXX")),
        strprintf("unix:%s", TempPath("bitcoin_sock1_XXXXXX")),
    };

    // Bind and listen on multiple addresses
    for (const auto& address : addresses) {
        bind_and_listen(address);
    }

    // Connect and test each address multiple times.
    for (int i : {0, 1, 0, 0, 1}) {
        connect_and_test(addresses[i]);
    }
}
