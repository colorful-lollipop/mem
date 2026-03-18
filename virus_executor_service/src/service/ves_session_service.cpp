#include "ves/ves_session_service.h"

#include <random>
#include <string>
#include <utility>
#include <unistd.h>

#include "memrpc/server/rpc_server.h"
#include "ves/ves_engine_service.h"
#include "ves/ves_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

namespace {
class RpcServerHandlerSink final : public AnyCallHandlerSink {
 public:
    explicit RpcServerHandlerSink(MemRpc::RpcServer* server)
        : server_(server) {}

    void RegisterHandler(MemRpc::Opcode opcode, MemRpc::RpcHandler handler) override
    {
        if (server_ == nullptr) {
            return;
        }
        server_->RegisterHandler(opcode, std::move(handler));
    }

 private:
    MemRpc::RpcServer* server_ = nullptr;
};

class AnyCallHandlerSinkImpl final : public AnyCallHandlerSink {
 public:
    explicit AnyCallHandlerSinkImpl(std::unordered_map<uint16_t, MemRpc::RpcHandler>* handlers)
        : handlers_(handlers) {}

    void RegisterHandler(MemRpc::Opcode opcode, MemRpc::RpcHandler handler) override
    {
        if (handlers_ == nullptr) {
            return;
        }
        const uint16_t key = static_cast<uint16_t>(opcode);
        if (!handler) {
            handlers_->erase(key);
            return;
        }
        (*handlers_)[key] = std::move(handler);
    }

 private:
    std::unordered_map<uint16_t, MemRpc::RpcHandler>* handlers_ = nullptr;
};

MemRpc::StatusCode InvokeAnyCallHandler(
    const std::unordered_map<uint16_t, MemRpc::RpcHandler>& handlers,
    const MemRpc::RpcServerCall& call,
    MemRpc::RpcServerReply* reply)
{
    if (reply == nullptr) {
        return MemRpc::StatusCode::InvalidArgument;
    }

    *reply = MemRpc::RpcServerReply{};
    const auto it = handlers.find(static_cast<uint16_t>(call.opcode));
    if (it == handlers.end()) {
        reply->status = MemRpc::StatusCode::InvalidArgument;
        return reply->status;
    }

    it->second(call, reply);
    return reply->status;
}

void CloseHandles(MemRpc::BootstrapHandles* handles) {
    if (handles == nullptr) return;
    int* fds[] = {
        &handles->shmFd,
        &handles->highReqEventFd,
        &handles->normalReqEventFd,
        &handles->respEventFd,
        &handles->reqCreditEventFd,
        &handles->respCreditEventFd,
    };
    for (int* fd : fds) {
        if (fd != nullptr && *fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
}

constexpr auto EVENT_PUBLISH_PERIOD_MIN = std::chrono::milliseconds(80);
constexpr auto EVENT_PUBLISH_PERIOD_MAX = std::chrono::milliseconds(180);
}  // namespace

EngineSessionService::EngineSessionService(std::vector<RpcHandlerRegistrar*> registrars)
    : registrars_(std::move(registrars)) {}

EngineSessionService::~EngineSessionService() {
    (void)CloseSession();
}

MemRpc::StatusCode EngineSessionService::EnsureInitialized() {
    std::lock_guard<std::mutex> lock(initMutex_);
    if (initialized_) {
        return MemRpc::StatusCode::Ok;
    }

    if (!bootstrap_) {
        bootstrap_ = std::make_shared<MemRpc::DevBootstrapChannel>();
    }

    MemRpc::BootstrapHandles throwaway{};
    const MemRpc::StatusCode open_status = bootstrap_->OpenSession(throwaway);
    if (open_status != MemRpc::StatusCode::Ok) {
        HILOGE("bootstrap OpenSession failed");
        CloseHandles(&throwaway);
        return open_status;
    }
    CloseHandles(&throwaway);

    const MemRpc::BootstrapHandles serverHandles = bootstrap_->serverHandles();
    rpcServer_ = std::make_shared<MemRpc::RpcServer>(serverHandles);
    anyCallHandlers_.clear();
    RpcServerHandlerSink rpcServerSink(rpcServer_.get());
    AnyCallHandlerSinkImpl anyCallSink(&anyCallHandlers_);
    for (auto* registrar : registrars_) {
        if (registrar != nullptr) {
            registrar->RegisterHandlers(&rpcServerSink);
            registrar->RegisterHandlers(&anyCallSink);
        }
    }
    const MemRpc::StatusCode start_status = rpcServer_->Start();
    if (start_status != MemRpc::StatusCode::Ok) {
        HILOGE("RpcServer start failed");
        return start_status;
    }

    initialized_ = true;
    HILOGI("EngineSessionService initialized");
    return MemRpc::StatusCode::Ok;
}

void EngineSessionService::StartEventPublisherLocked() {
    if (eventPublisherRunning_.load(std::memory_order_acquire)) {
        return;
    }
    eventPublisherRunning_.store(true, std::memory_order_release);
    eventPublisherThread_ = std::thread([this] { EventPublisherLoop(); });
}

MemRpc::StatusCode EngineSessionService::OpenSession(MemRpc::BootstrapHandles& handles) {
    const MemRpc::StatusCode init_status = EnsureInitialized();
    if (init_status != MemRpc::StatusCode::Ok) {
        return init_status;
    }
    MemRpc::StatusCode status = MemRpc::StatusCode::InvalidArgument;
    {
        std::lock_guard<std::mutex> lock(initMutex_);
        if (bootstrap_ == nullptr) {
            return MemRpc::StatusCode::InvalidArgument;
        }
        status = bootstrap_->OpenSession(handles);
        if (status == MemRpc::StatusCode::Ok) {
            sessionId_.store(handles.sessionId, std::memory_order_release);
            StartEventPublisherLocked();
        }
    }
    return status;
}

MemRpc::StatusCode EngineSessionService::PublishEventBlocking(const MemRpc::RpcEvent& event) {
    std::shared_ptr<MemRpc::RpcServer> rpcServer;
    {
        std::lock_guard<std::mutex> lock(initMutex_);
        if (!initialized_ || rpcServer_ == nullptr || sessionId_.load(std::memory_order_acquire) == 0) {
            return MemRpc::StatusCode::PeerDisconnected;
        }
        rpcServer = rpcServer_;
    }
    return rpcServer->PublishEvent(event);
}

MemRpc::StatusCode EngineSessionService::InvokeAnyCall(const MemRpc::RpcServerCall& call,
                                                       MemRpc::RpcServerReply* reply) {
    const MemRpc::StatusCode initStatus = EnsureInitialized();
    if (initStatus != MemRpc::StatusCode::Ok) {
        return initStatus;
    }
    return InvokeAnyCallHandler(anyCallHandlers_, call, reply);
}

MemRpc::StatusCode EngineSessionService::CloseSession() {
    std::thread eventPublisherThread;
    std::shared_ptr<MemRpc::RpcServer> rpcServer;
    {
        std::lock_guard<std::mutex> lock(initMutex_);
        if (!initialized_) {
            return MemRpc::StatusCode::Ok;
        }
        eventPublisherRunning_.store(false, std::memory_order_release);
        eventPublisherThread = std::move(eventPublisherThread_);
        rpcServer = std::move(rpcServer_);
        bootstrap_.reset();
        initialized_ = false;
        sessionId_.store(0, std::memory_order_release);
    }
    eventPublisherCv_.notify_all();
    if (eventPublisherThread.joinable()) {
        eventPublisherThread.join();
    }
    if (rpcServer) {
        rpcServer->Stop();
    }
    HILOGI("EngineSessionService closed");
    return MemRpc::StatusCode::Ok;
}

uint64_t EngineSessionService::session_id() const {
    return sessionId_.load(std::memory_order_acquire);
}

MemRpc::RpcServerRuntimeStats EngineSessionService::GetRuntimeStats() const {
    std::lock_guard<std::mutex> lock(initMutex_);
    if (rpcServer_ == nullptr) {
        return {};
    }
    return rpcServer_->GetRuntimeStats();
}

void EngineSessionService::EventPublisherLoop() {
    std::random_device device;
    std::mt19937 rng(device());
    std::uniform_int_distribution<int> delayDist(
        static_cast<int>(EVENT_PUBLISH_PERIOD_MIN.count()),
        static_cast<int>(EVENT_PUBLISH_PERIOD_MAX.count()));
    std::uniform_int_distribution<uint32_t> eventTypeDist(
        static_cast<uint32_t>(VesEventType::RandomScanResult),
        static_cast<uint32_t>(VesEventType::RandomLifecycle));
    std::uniform_int_distribution<int> stateDist(0, 99);

    while (eventPublisherRunning_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> waitLock(eventPublisherMutex_);
            if (eventPublisherCv_.wait_for(
                    waitLock,
                    std::chrono::milliseconds(delayDist(rng)),
                    [this] { return !eventPublisherRunning_.load(std::memory_order_acquire); })) {
                break;
            }
        }

        uint64_t sessionId = 0;
        {
            std::lock_guard<std::mutex> lock(initMutex_);
            if (!initialized_ || rpcServer_ == nullptr) {
                continue;
            }
            sessionId = sessionId_.load(std::memory_order_acquire);
        }

        if (sessionId == 0) {
            continue;
        }

        const uint32_t eventType = eventTypeDist(rng);
        const uint32_t sequence = eventSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::string payloadText = "ves-random-event session=" + std::to_string(sessionId) +
                                        " seq=" + std::to_string(sequence) +
                                        " type=" + std::to_string(eventType) +
                                        " state=" + std::to_string(stateDist(rng));

        MemRpc::RpcEvent event;
        event.eventDomain = VES_EVENT_DOMAIN_RUNTIME;
        event.eventType = eventType;
        event.payload.assign(payloadText.begin(), payloadText.end());
        const MemRpc::StatusCode status = PublishEventBlocking(event);
        if (status != MemRpc::StatusCode::Ok &&
            status != MemRpc::StatusCode::QueueFull &&
            status != MemRpc::StatusCode::PeerDisconnected) {
            HILOGW("random event publish failed, status=%{public}d", static_cast<int>(status));
        }
    }
}

}  // namespace VirusExecutorService
