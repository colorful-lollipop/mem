#include "client/ves_client.h"

#include "iremote_broker.h"
#include "iremote_broker_registry.h"
#include "memrpc/client/typed_invoker.h"
#include "transport/ves_control_interface.h"
#include "ves/ves_codec.h"
#include "ves/ves_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

namespace {

template <typename Request, typename Reply>
MemRpc::StatusCode InvokeWithAnyCallFallback(MemRpc::RpcClient* client,
                                             const std::shared_ptr<VesControlProxy>& proxy,
                                             VesOpcode opcode,
                                             const Request& request,
                                             Reply* reply,
                                             MemRpc::Priority priority,
                                             uint32_t execTimeoutMs) {
    std::vector<uint8_t> payload;
    if (!MemRpc::EncodeMessage<Request>(request, &payload)) {
        return MemRpc::StatusCode::ProtocolMismatch;
    }

    if (payload.size() <= MemRpc::DEFAULT_MAX_REQUEST_BYTES) {
        return MemRpc::InvokeTypedSync<Request, Reply>(
            client,
            static_cast<MemRpc::Opcode>(opcode),
            request,
            reply,
            priority,
            execTimeoutMs);
    }

    if (proxy == nullptr) {
        return MemRpc::StatusCode::PeerDisconnected;
    }

    VesAnyCallRequest anyRequest;
    anyRequest.opcode = static_cast<uint16_t>(opcode);
    anyRequest.priority = static_cast<uint16_t>(priority);
    anyRequest.payload = std::move(payload);

    VesAnyCallReply anyReply;
    const MemRpc::StatusCode status = proxy->AnyCall(anyRequest, anyReply);
    if (status != MemRpc::StatusCode::Ok) {
        return status;
    }
    if (anyReply.status != MemRpc::StatusCode::Ok) {
        return anyReply.status;
    }
    return MemRpc::DecodeMessage<Reply>(anyReply.payload, reply)
               ? MemRpc::StatusCode::Ok
               : MemRpc::StatusCode::ProtocolMismatch;
}

}  // namespace

VesClient::VesClient(const OHOS::sptr<OHOS::IRemoteObject>& remote,
                     VesClientOptions options)
    : remote_(remote),
      options_(options) {}

VesClient::~VesClient() {
    Shutdown();
}

void VesClient::RegisterProxyFactory() {
    OHOS::BrokerRegistration::GetInstance().Register(
        VES_CONTROL_SA_ID,
        [](const OHOS::sptr<OHOS::IRemoteObject>& remote) -> OHOS::sptr<OHOS::IRemoteBroker> {
            std::string servicePath = remote->GetServicePath();
            return std::make_shared<VesControlProxy>(remote, servicePath);
        });
}

MemRpc::StatusCode VesClient::Init() {
    auto bootstrap = OHOS::iface_cast<IVesControl>(remote_);
    if (bootstrap == nullptr) {
        HILOGE("iface_cast<IVesControl> failed");
        return MemRpc::StatusCode::InvalidArgument;
    }

    proxy_ = std::dynamic_pointer_cast<VesControlProxy>(bootstrap);
    if (proxy_ == nullptr) {
        HILOGE("dynamic_pointer_cast to VesControlProxy failed");
        return MemRpc::StatusCode::InvalidArgument;
    }

    bootstrapChannel_ = std::make_shared<VesControlChannelAdapter>(proxy_);
    client_.SetBootstrapChannel(bootstrapChannel_);
    if (healthSnapshotCallback_) {
        bootstrapChannel_->SetHealthSnapshotCallback(healthSnapshotCallback_);
    }

    MemRpc::RecoveryPolicy policy;
    policy.onFailure = [delay = options_.execTimeoutRestartDelayMs](const MemRpc::RpcFailure& failure) {
        if (failure.status == MemRpc::StatusCode::ExecTimeout) {
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Restart, delay};
        }
        return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
    };
    policy.onEngineDeath = [this](const MemRpc::EngineDeathReport& report) {
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
        engineDied_ = true;
        return MemRpc::RecoveryDecision{
            MemRpc::RecoveryAction::Restart,
            options_.engineDeathRestartDelayMs,
        };
    };
    if (options_.idleShutdownTimeoutMs > 0) {
        policy.onIdle = [timeout = options_.idleShutdownTimeoutMs](uint64_t idleMs) {
            if (idleMs < timeout) {
                return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::Ignore, 0};
            }
            return MemRpc::RecoveryDecision{MemRpc::RecoveryAction::CloseSession, 0};
        };
    }

    client_.SetRecoveryPolicy(std::move(policy));
    return client_.Init();
}

void VesClient::SetHealthSnapshotCallback(HealthSnapshotCallback callback) {
    healthSnapshotCallback_ = std::move(callback);
    if (bootstrapChannel_ != nullptr) {
        bootstrapChannel_->SetHealthSnapshotCallback(healthSnapshotCallback_);
    }
}

void VesClient::RequestRecovery(uint32_t delayMs) {
    client_.RequestExternalRecovery({
        MemRpc::ExternalRecoverySignal::ChannelHealthTimeout,
        0,
        delayMs,
    });
}

void VesClient::Shutdown() {
    client_.Shutdown();
    bootstrapChannel_.reset();
    proxy_.reset();
}

bool VesClient::EngineDied() const {
    return engineDied_.load();
}

MemRpc::StatusCode VesClient::ScanFile(const ScanTask& scanTask,
                                       ScanFileReply* reply,
                                       MemRpc::Priority priority,
                                       uint32_t execTimeoutMs) {
    if (reply == nullptr) {
        return MemRpc::StatusCode::InvalidArgument;
    }

    return InvokeWithAnyCallFallback<ScanTask, ScanFileReply>(
        &client_,
        proxy_,
        VesOpcode::ScanFile,
        scanTask,
        reply,
        priority,
        execTimeoutMs);
}

}  // namespace VirusExecutorService
