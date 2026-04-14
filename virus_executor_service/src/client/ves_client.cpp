#include "client/ves_client.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "iremote_broker.h"
#include "iservice_registry.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "virus_protection_executor_log.h"

namespace VirusExecutorService {
namespace {
constexpr uint32_t DEFAULT_RESTART_DELAY_MS = 200;
std::atomic<uint64_t> g_nextVesClientGeneration{1};
std::atomic<uint64_t> g_activeVesClientGeneration{0};

MemRpc::RecoveryPolicy BuildRecoveryPolicy(const VesClientOptions& options)
{
    MemRpc::RecoveryPolicy policy = options.recoveryPolicy;
    if (!policy.onFailure) {
        policy.onFailure = [](const MemRpc::RpcFailure& failure) {
            if (failure.status == MemRpc::StatusCode::ExecTimeout) {
                return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, DEFAULT_RESTART_DELAY_MS};
            }
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
        };
    }
    if (!policy.onEngineDeath) {
        policy.onEngineDeath = [](const MemRpc::EngineDeathReport& report) {
            HILOGW("engine death: session=%{public}llu", static_cast<unsigned long long>(report.deadSessionId));
            return MemRpc::RecoveryDecision{
                MemRpc::RecoveryAction::Restart,
                DEFAULT_RESTART_DELAY_MS,
            };
        };
    }
    if (!policy.onIdle && options.idleShutdownTimeoutMs > 0) {
        policy.onIdle = [timeout = options.idleShutdownTimeoutMs](uint64_t idleMs) {
            if (idleMs < timeout) {
                return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
            }
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::IdleClose, 0};
        };
    }
    return policy;
}

VesClient::ControlLoader BuildControlLoader(VesClientConnectOptions connectOptions)
{
    return [connectOptions]() -> OHOS::sptr<IVirusProtectionExecutor> {
        auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
        if (sam == nullptr) {
            HILOGE("GetSystemAbilityManager failed");
            return nullptr;
        }
        OHOS::sptr<OHOS::IRemoteObject> remote = sam->CheckSystemAbility(VIRUS_PROTECTION_EXECUTOR_SA_ID);
        if (remote != nullptr) {
            auto control = OHOS::iface_cast<IVirusProtectionExecutor>(remote);
            if (control != nullptr) {
                control->CloseSession();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        remote = sam->LoadSystemAbility(VIRUS_PROTECTION_EXECUTOR_SA_ID, connectOptions.loadTimeoutMs);
        return remote != nullptr ? OHOS::iface_cast<IVirusProtectionExecutor>(remote) : nullptr;
    };
}

[[noreturn]] void AbortForMissingControlLoader()
{
    HILOGE("VesClient requires a non-null control loader");
    std::abort();
}

struct VesInvokeExecutionContext {
    MemRpc::RpcClient* client = nullptr;
    OHOS::sptr<IVirusProtectionExecutor> control;
};

struct VesInvokeRequestView {
    MemRpc::Opcode opcode = MemRpc::OPCODE_INVALID;
    MemRpc::Priority priority = MemRpc::Priority::Normal;
    uint32_t execTimeoutMs = 0;
    const std::vector<uint8_t>* payload = nullptr;
};

std::string ToBinaryString(const std::vector<uint8_t>& payload)
{
    if (payload.empty()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

template <typename Reply>
MemRpc::StatusCode InvokeInlineApi(const VesInvokeExecutionContext& context,
                                   const VesInvokeRequestView& request,
                                   Reply* reply)
{
    if (context.client == nullptr || request.payload == nullptr) {
        return MemRpc::StatusCode::InvalidArgument;
    }

    MemRpc::RpcCall call;
    call.opcode = request.opcode;
    call.priority = request.priority;
    call.execTimeoutMs = request.execTimeoutMs;
    call.payload = *request.payload;
    return MemRpc::WaitAndDecode<Reply>(context.client->InvokeAsync(std::move(call)), reply);
}

template <typename Reply>
MemRpc::StatusCode InvokeAnyCallApi(const VesInvokeExecutionContext& context,
                                    const VesInvokeRequestView& request,
                                    Reply* reply)
{
    if (context.control == nullptr) {
        HILOGE("VesClient::InvokeApi failed: control is null opcode=%{public}u", request.opcode);
        return MemRpc::StatusCode::PeerDisconnected;
    }
    if (request.payload == nullptr) {
        return MemRpc::StatusCode::InvalidArgument;
    }

    VesAnyCallRequest anyRequest;
    anyRequest.opcode = static_cast<uint16_t>(request.opcode);
    anyRequest.priority = static_cast<uint16_t>(request.priority);
    anyRequest.timeoutMs = request.execTimeoutMs;
    anyRequest.payload = ToBinaryString(*request.payload);

    VesAnyCallReply anyReply;
    const MemRpc::StatusCode status = context.control->AnyCall(anyRequest, anyReply);
    if (status != MemRpc::StatusCode::Ok) {
        HILOGE("VesClient::InvokeApi AnyCall failed opcode=%{public}u status=%{public}d",
               request.opcode,
               static_cast<int>(status));
        return status;
    }
    if (anyReply.status != MemRpc::StatusCode::Ok) {
        HILOGE("VesClient::InvokeApi AnyCall reply failed opcode=%{public}u status=%{public}d",
               request.opcode,
               static_cast<int>(anyReply.status));
        return anyReply.status;
    }
    if (!MemRpc::DecodeMessage<Reply>(anyReply.payload, reply)) {
        HILOGE("VesClient::InvokeApi decode failed opcode=%{public}u payload_size=%{public}zu",
               request.opcode,
               anyReply.payload.size());
        return MemRpc::StatusCode::ProtocolMismatch;
    }
    return MemRpc::StatusCode::Ok;
}

enum class VesInvokeRoute : uint8_t {
    InlineMemRpc = 0,
    AnyCall = 1,
};

template <typename Request>
MemRpc::StatusCode EncodeInvokePayload(MemRpc::Opcode opcode, const Request& request, std::vector<uint8_t>* payload)
{
    if (!MemRpc::EncodeMessage<Request>(request, payload)) {
        HILOGE("VesClient::InvokeApi encode failed opcode=%{public}u", opcode);
        return MemRpc::StatusCode::ProtocolMismatch;
    }
    return MemRpc::StatusCode::Ok;
}

template <typename Reply>
MemRpc::StatusCode ExecuteInvokeRoute(VesInvokeRoute route,
                                      const VesInvokeExecutionContext& context,
                                      const VesInvokeRequestView& request,
                                      Reply* reply)
{
    if (context.client == nullptr || request.payload == nullptr) {
        return MemRpc::StatusCode::InvalidArgument;
    }

    switch (route) {
        case VesInvokeRoute::InlineMemRpc:
            return InvokeInlineApi(context, request, reply);
        case VesInvokeRoute::AnyCall:
            return InvokeAnyCallApi(context, request, reply);
        default:
            return MemRpc::StatusCode::InvalidArgument;
    }
}

}  // namespace

VesClient::VesClient(ControlLoader controlLoader, VesClientOptions options)
    : controlLoader_(std::move(controlLoader)),
      options_(std::move(options)),
      instanceGeneration_(g_nextVesClientGeneration.fetch_add(1, std::memory_order_relaxed))
{
    if (!controlLoader_) {
        AbortForMissingControlLoader();
    }
}

VesClient::~VesClient()
{
    Shutdown();
}

std::unique_ptr<VesClient> VesClient::Connect(VesClientOptions options, VesClientConnectOptions connectOptions)
{
    auto client = std::make_unique<VesClient>(BuildControlLoader(connectOptions), std::move(options));
    if (client->Init() != MemRpc::StatusCode::Ok) {
        HILOGE("VesClient init failed");
        return nullptr;
    }
    return client;
}

MemRpc::StatusCode VesClient::Init()
{
    ClaimProcessOwnership();
    bootstrapChannel_ = std::make_shared<VesBootstrapChannel>(
        controlLoader_,
        options_.openSessionRequest,
        [this]() { return IsProcessOwner(); });
    client_.SetBootstrapChannel(bootstrapChannel_);
    client_.SetRecoveryPolicy(BuildRecoveryPolicy(options_));
    const MemRpc::StatusCode status = client_.Init();
    if (status != MemRpc::StatusCode::Ok) {
        HILOGE("VesClient::Init failed for saId=%{public}d status=%{public}d",
               VIRUS_PROTECTION_EXECUTOR_SA_ID,
               static_cast<int>(status));
        return status;
    }
    return MemRpc::StatusCode::Ok;
}

void VesClient::ClaimProcessOwnership()
{
    g_activeVesClientGeneration.store(instanceGeneration_, std::memory_order_release);
}

bool VesClient::IsProcessOwner() const
{
    return g_activeVesClientGeneration.load(std::memory_order_acquire) == instanceGeneration_;
}

void VesClient::SetEventCallback(EventCallback callback)
{
    client_.SetEventCallback(std::move(callback));
}

void VesClient::Shutdown()
{
    client_.SetRecoveryPolicy({});
    client_.Shutdown();
    bootstrapChannel_.reset();
}

OHOS::sptr<IVirusProtectionExecutor> VesClient::CurrentControl()
{
    if (bootstrapChannel_ == nullptr) {
        return nullptr;
    }
    return bootstrapChannel_->CurrentControl();
}

template <typename Request, typename Reply>
MemRpc::StatusCode VesClient::InvokeApi(MemRpc::Opcode opcode,
                                        const Request& request,
                                        Reply* reply,
                                        MemRpc::Priority priority,
                                        uint32_t execTimeoutMs)
{
    if (reply == nullptr) {
        HILOGE("VesClient::InvokeApi failed: reply is null opcode=%{public}u", opcode);
        return MemRpc::StatusCode::InvalidArgument;
    }

    std::vector<uint8_t> payload;
    MemRpc::StatusCode status = EncodeInvokePayload(opcode, request, &payload);
    if (status != MemRpc::StatusCode::Ok) {
        return status;
    }

    VesInvokeRoute route = VesInvokeRoute::InlineMemRpc;
    if (payload.size() > MemRpc::DEFAULT_MAX_REQUEST_BYTES) {
        HILOGW("VesClient::InvokeApi oversized request uses AnyCall: size=%{public}zu/%{public}zu",
               payload.size(),
               MemRpc::DEFAULT_MAX_REQUEST_BYTES);
        route = VesInvokeRoute::AnyCall;
    }

    const VesInvokeRequestView invokeRequest{
        opcode,
        priority,
        execTimeoutMs,
        &payload,
    };
    return client_.RetryUntilRecoverySettles([&]() {
        const VesInvokeExecutionContext context{
            &client_,
            CurrentControl(),
        };
        return ExecuteInvokeRoute(route, context, invokeRequest, reply);
    });
}

MemRpc::StatusCode VesClient::ScanFile(const ScanTask& scanTask,
                                       ScanFileReply* reply,
                                       MemRpc::Priority priority,
                                       uint32_t execTimeoutMs)
{
    return InvokeApi<ScanTask, ScanFileReply>(static_cast<MemRpc::Opcode>(VesOpcode::ScanFile),
                                              scanTask,
                                              reply,
                                              priority,
                                              execTimeoutMs);
}

template MemRpc::StatusCode VesClient::InvokeApi<ScanTask, ScanFileReply>(MemRpc::Opcode opcode,
                                                                          const ScanTask& request,
                                                                          ScanFileReply* reply,
                                                                          MemRpc::Priority priority,
                                                                          uint32_t execTimeoutMs);
}  // namespace VirusExecutorService
