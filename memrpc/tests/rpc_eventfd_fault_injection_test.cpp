#include <gtest/gtest.h>

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "memrpc/client/dev_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr MemRpc::Opcode kFaultInjectionOpcode = 301U;
constexpr uint32_t kFaultEventDomain = 7U;
constexpr uint32_t kFaultEventType = 11U;

bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

void CloseFd(int* fd)
{
    if (fd == nullptr || *fd < 0) {
        return;
    }
    close(*fd);
    *fd = -1;
}

void CloseHandles(MemRpc::BootstrapHandles* handles)
{
    if (handles == nullptr) {
        return;
    }
    CloseFd(&handles->shmFd);
    CloseFd(&handles->highReqEventFd);
    CloseFd(&handles->normalReqEventFd);
    CloseFd(&handles->respEventFd);
    CloseFd(&handles->reqCreditEventFd);
    CloseFd(&handles->respCreditEventFd);
}

struct ClientFaultConfig {
    bool breakNormalRequestSignal = false;
    bool breakRequestCredit = false;
};

class FaultInjectingBootstrap final : public MemRpc::IBootstrapChannel {
public:
    explicit FaultInjectingBootstrap(MemRpc::DevBootstrapConfig config = {}, ClientFaultConfig faults = {})
        : inner_(std::make_shared<MemRpc::DevBootstrapChannel>(std::move(config))),
          faults_(faults)
    {
    }

    MemRpc::StatusCode OpenSession(MemRpc::BootstrapHandles& handles) override
    {
        const MemRpc::StatusCode status = inner_->OpenSession(handles);
        if (status != MemRpc::StatusCode::Ok) {
            return status;
        }
        if (faults_.breakNormalRequestSignal) {
            CloseFd(&handles.normalReqEventFd);
        }
        if (faults_.breakRequestCredit) {
            CloseFd(&handles.reqCreditEventFd);
        }
        return MemRpc::StatusCode::Ok;
    }

    MemRpc::StatusCode CloseSession() override
    {
        return inner_->CloseSession();
    }

    void SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) override
    {
        inner_->SetEngineDeathCallback(std::move(callback));
    }

    [[nodiscard]] MemRpc::BootstrapHandles serverHandles() const
    {
        return inner_->serverHandles();
    }

private:
    std::shared_ptr<MemRpc::DevBootstrapChannel> inner_;
    ClientFaultConfig faults_;
};

void RegisterEchoHandler(MemRpc::RpcServer* server, std::atomic<int>* call_count = nullptr)
{
    ASSERT_NE(server, nullptr);
    server->RegisterHandler(kFaultInjectionOpcode,
                            [call_count](const MemRpc::RpcServerCall& call, MemRpc::RpcServerReply* reply) {
                                ASSERT_NE(reply, nullptr);
                                if (call_count != nullptr) {
                                    call_count->fetch_add(1, std::memory_order_relaxed);
                                }
                                reply->status = MemRpc::StatusCode::Ok;
                                reply->payload = call.payload;
                            });
}

}  // namespace

namespace MemRpc {

TEST(RpcEventFdFaultInjectionTest, ClientRequestSignalFailureFallsBackToPolling)
{
    auto bootstrap = std::make_shared<FaultInjectingBootstrap>(DevBootstrapConfig{}, ClientFaultConfig{true, false});
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(&unused_handles);

    RpcServer server;
    server.SetBootstrapHandles(bootstrap->serverHandles());
    std::atomic<int> call_count{0};
    RegisterEchoHandler(&server, &call_count);
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    RpcCall call;
    call.opcode = kFaultInjectionOpcode;
    call.payload = {1, 2, 3, 4};

    RpcReply reply;
    auto future = client.InvokeAsync(call);
    ASSERT_TRUE(WaitFor([&]() { return future.IsReady(); }, std::chrono::seconds(1)));
    EXPECT_EQ(std::move(future).Wait(&reply), StatusCode::Ok);
    EXPECT_EQ(reply.payload, call.payload);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(call_count.load(std::memory_order_relaxed), 1);

    client.Shutdown();
    server.Stop();
}

TEST(RpcEventFdFaultInjectionTest, ClientRequestCreditFailureLeavesBlockedAdmissionPendingUntilShutdown)
{
    DevBootstrapConfig config;
    config.highRingSize = 1;
    config.normalRingSize = 1;
    config.responseRingSize = 2;

    auto bootstrap = std::make_shared<FaultInjectingBootstrap>(config, ClientFaultConfig{false, true});
    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    RpcCall call;
    call.opcode = kFaultInjectionOpcode;
    // Keep the first in-flight request pending well beyond the observation
    // window so the second admission only resolves when shutdown tears the
    // client down, not because watchdog timeouts free queue capacity.
    call.execTimeoutMs = 5000;

    auto first_future = client.InvokeAsync(call);
    auto second_future = client.InvokeAsync(call);

    RpcReply second_reply;
    EXPECT_FALSE(WaitFor([&]() { return second_future.IsReady(); }, std::chrono::milliseconds(500)));

    client.Shutdown();

    EXPECT_EQ(std::move(second_future).Wait(&second_reply), StatusCode::ClientClosed);

    RpcReply first_reply;
    EXPECT_NE(std::move(first_future).Wait(&first_reply), StatusCode::Ok);
}

TEST(RpcEventFdFaultInjectionTest, ServerResponseSignalFailureFallsBackToPolling)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(&unused_handles);

    BootstrapHandles server_handles = bootstrap->serverHandles();
    CloseFd(&server_handles.respEventFd);

    RpcServer server;
    server.SetBootstrapHandles(std::move(server_handles));
    RegisterEchoHandler(&server);
    ASSERT_EQ(server.Start(), StatusCode::Ok);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    RpcCall call;
    call.opcode = kFaultInjectionOpcode;
    call.payload = {9, 8, 7};

    RpcReply reply;
    auto future = client.InvokeAsync(call);
    ASSERT_TRUE(WaitFor([&]() { return future.IsReady(); }, std::chrono::seconds(1)));
    EXPECT_EQ(std::move(future).Wait(&reply), StatusCode::Ok);
    EXPECT_EQ(reply.payload, call.payload);

    client.Shutdown();
    server.Stop();
}

TEST(RpcEventFdFaultInjectionTest, ServerEventSignalFailureStillDeliversCallback)
{
    auto bootstrap = std::make_shared<DevBootstrapChannel>();
    BootstrapHandles unused_handles = MakeDefaultBootstrapHandles();
    ASSERT_EQ(bootstrap->OpenSession(unused_handles), StatusCode::Ok);
    CloseHandles(&unused_handles);

    BootstrapHandles server_handles = bootstrap->serverHandles();
    RpcServer server;
    server.SetBootstrapHandles(server_handles);
    RegisterEchoHandler(&server);
    ASSERT_EQ(server.Start(), StatusCode::Ok);
    ASSERT_GE(server_handles.respEventFd, 0);
    close(server_handles.respEventFd);

    RpcClient client(bootstrap);
    ASSERT_EQ(client.Init(), StatusCode::Ok);

    std::mutex event_mutex;
    std::condition_variable event_cv;
    bool event_ready = false;
    RpcEvent received_event;
    client.SetEventCallback([&](const RpcEvent& event) {
        {
            std::lock_guard<std::mutex> lock(event_mutex);
            received_event = event;
            event_ready = true;
        }
        event_cv.notify_one();
    });

    RpcEvent event;
    event.eventDomain = kFaultEventDomain;
    event.eventType = kFaultEventType;
    event.payload = {4, 5, 6};
    ASSERT_EQ(server.PublishEvent(event), StatusCode::Ok);

    std::unique_lock<std::mutex> lock(event_mutex);
    ASSERT_TRUE(event_cv.wait_for(lock, std::chrono::seconds(1), [&] { return event_ready; }));
    EXPECT_EQ(received_event.eventDomain, event.eventDomain);
    EXPECT_EQ(received_event.eventType, event.eventType);
    EXPECT_EQ(received_event.payload, event.payload);

    client.Shutdown();
    server.Stop();
}

}  // namespace MemRpc
