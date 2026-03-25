#include "client/ves_client.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "iremote_broker.h"
#include "iremote_broker_registry.h"
#include "iservice_registry.h"
#include "memrpc/client/typed_invoker.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {
namespace {
MemRpc::RecoveryPolicy BuildRecoveryPolicy(const VesClientOptions& options)
{
    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [delay = options.execTimeoutRestartDelayMs](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, delay};
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    policy.onEngineDeath = [restartDelayMs =
                                options.engineDeathRestartDelayMs](const MemRpc::EngineDeathReport& report) {
        HILOGW("engine death: session=%{public}llu, safe_to_replay=%{public}u, poison_pills=%{public}zu",
               static_cast<unsigned long long>(report.deadSessionId),
               report.safeToReplayCount,
               report.poisonPillSuspects.size());
        for (const auto& suspect : report.poisonPillSuspects) {
            HILOGW("  poison pill: request_id=%{public}llu, opcode=%{public}u, last_state=%{public}d",
                   static_cast<unsigned long long>(suspect.requestId),
                   static_cast<unsigned>(suspect.opcode),
                   static_cast<int>(suspect.lastState));
        }
        return MemRpc::RecoveryDecision{
            MemRpc::RecoveryAction::Restart,
            restartDelayMs,
        };
    };
    if (options.idleShutdownTimeoutMs > 0) {
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
}  // namespace

VesClient::VesClient(ControlLoader controlLoader, VesClientOptions options)
    : controlLoader_(std::move(controlLoader)),
      options_(std::move(options))
{
    if (!controlLoader_) {
        AbortForMissingControlLoader();
    }
}

VesClient::~VesClient()
{
    Shutdown();
}

void VesClient::RegisterProxyFactory()
{
    OHOS::BrokerRegistration::GetInstance().Register(
        VIRUS_PROTECTION_EXECUTOR_SA_ID,
        [](const OHOS::sptr<OHOS::IRemoteObject>& remote) -> OHOS::sptr<OHOS::IRemoteBroker> {
            std::string servicePath = remote->GetServicePath();
            return std::make_shared<VesControlProxy>(remote, servicePath);
        });
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
    bootstrapChannel_ = std::make_shared<VesBootstrapChannel>(controlLoader_, options_.openSessionRequest);
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

    const uint32_t minRecoveryWaitMs = std::max(options_.execTimeoutRestartDelayMs, options_.engineDeathRestartDelayMs);
    return client_.InvokeWithRecovery(
        [&]() {
            std::vector<uint8_t> payload;
            if (!MemRpc::EncodeMessage<Request>(request, &payload)) {
                HILOGE("VesClient::InvokeApi encode failed opcode=%{public}u", opcode);
                return MemRpc::StatusCode::ProtocolMismatch;
            }

            if (payload.size() <= MemRpc::DEFAULT_MAX_REQUEST_BYTES) {
                MemRpc::RpcCall call;
                call.opcode = opcode;
                call.priority = priority;
                call.waitForRecovery = true;
                call.recoveryTimeoutMs = minRecoveryWaitMs;
                call.execTimeoutMs = execTimeoutMs;
                call.payload = std::move(payload);
                return MemRpc::WaitAndDecode<Reply>(client_.InvokeAsync(std::move(call)), reply);
            }

            auto control = CurrentControl();
            if (control == nullptr) {
                HILOGE("VesClient::InvokeApi failed: control is null opcode=%{public}u", opcode);
                return MemRpc::StatusCode::PeerDisconnected;
            }

            VesAnyCallRequest anyRequest;
            anyRequest.opcode = static_cast<uint16_t>(opcode);
            anyRequest.priority = static_cast<uint16_t>(priority);
            anyRequest.timeoutMs = execTimeoutMs;
            anyRequest.payload = std::move(payload);

            VesAnyCallReply anyReply;
            const MemRpc::StatusCode status = control->AnyCall(anyRequest, anyReply);
            if (status != MemRpc::StatusCode::Ok) {
                HILOGE("VesClient::InvokeApi AnyCall failed opcode=%{public}u status=%{public}d",
                       opcode,
                       static_cast<int>(status));
                return status;
            }
            if (anyReply.status != MemRpc::StatusCode::Ok) {
                HILOGE("VesClient::InvokeApi AnyCall reply failed opcode=%{public}u status=%{public}d error=%{public}d",
                       opcode,
                       static_cast<int>(anyReply.status),
                       anyReply.errorCode);
                return anyReply.status;
            }
            if (!MemRpc::DecodeMessage<Reply>(anyReply.payload, reply)) {
                HILOGE("VesClient::InvokeApi decode failed opcode=%{public}u payload_size=%{public}zu",
                       opcode,
                       anyReply.payload.size());
                return MemRpc::StatusCode::ProtocolMismatch;
            }
            return MemRpc::StatusCode::Ok;
        },
        minRecoveryWaitMs,
        100);
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
