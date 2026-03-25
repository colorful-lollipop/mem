#include "transport/ves_control_proxy.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "iremote_broker.h"
#include "iremote_broker_registry.h"
#include "iservice_registry.h"
#include "scm_rights.h"
#include "ves/ves_codec.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

namespace {
constexpr int DEFAULT_HEARTBEAT_TIMEOUT_MS = 500;

bool RegisterVesControlProxyFactory()
{
    OHOS::BrokerRegistration::GetInstance().Register(
        VIRUS_PROTECTION_EXECUTOR_SA_ID,
        [](const OHOS::sptr<OHOS::IRemoteObject>& remote) -> OHOS::sptr<OHOS::IRemoteBroker> {
            return std::make_shared<VesControlProxy>(remote, remote->GetServicePath());
        });
    return true;
}

[[maybe_unused]] const bool kVesControlProxyFactoryRegistered = RegisterVesControlProxyFactory();

std::shared_ptr<VesControlProxy> RetainProxy(VesControlProxy* proxy)
{
    try {
        return std::dynamic_pointer_cast<VesControlProxy>(proxy->OHOS::RefBase::shared_from_this());
    } catch (const std::bad_weak_ptr&) {
        return {};
    }
}

int ConnectToService(const std::string& path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Metadata sent alongside SCM_RIGHTS fds.
struct SessionMetadata {
    uint32_t protocolVersion = 0;
    uint64_t sessionId = 0;
};

constexpr size_t FD_COUNT = 6;

struct AnyCallRequestHeader {
    uint16_t opcode = 0;
    uint16_t priority = static_cast<uint16_t>(MemRpc::Priority::Normal);
    uint32_t timeoutMs = 0;
    uint32_t payloadSize = 0;
};

struct AnyCallReplyHeader {
    uint32_t status = 0;
    int32_t errorCode = 0;
    uint32_t payloadSize = 0;
};

bool SendAll(int fd, const void* buffer, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t rc = send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (rc <= 0) {
            return false;
        }
        offset += static_cast<size_t>(rc);
    }
    return true;
}

bool RecvAll(int fd, void* buffer, size_t size)
{
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t rc = recv(fd, bytes + offset, size - offset, 0);
        if (rc <= 0) {
            return false;
        }
        offset += static_cast<size_t>(rc);
    }
    return true;
}

bool HasDisconnectSignal(const pollfd& pfd)
{
    return (pfd.revents & (POLLHUP | POLLERR | POLLIN)) != 0;
}

void JoinThread(std::thread* worker)
{
    if (worker == nullptr || !worker->joinable()) {
        return;
    }
    if (worker->get_id() == std::this_thread::get_id()) {
        worker->detach();
        return;
    }
    worker->join();
}

bool IsHealthyHeartbeatStatus(uint32_t status)
{
    return status == static_cast<uint32_t>(VesHeartbeatStatus::OkIdle) ||
           status == static_cast<uint32_t>(VesHeartbeatStatus::OkBusy) ||
           status == static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning);
}

bool ShouldRetryOpenSessionAfterRefresh(MemRpc::StatusCode status)
{
    switch (status) {
        case MemRpc::StatusCode::PeerDisconnected:
        case MemRpc::StatusCode::CrashedDuringExecution:
            return true;
        default:
            return false;
    }
}

[[noreturn]] void AbortForMissingControlLoader()
{
    HILOGE("VesBootstrapChannel requires a non-null control loader");
    std::abort();
}

MemRpc::ChannelHealthResult ToHealthResult(const VesHeartbeatReply& reply, uint64_t expectedSessionId)
{
    MemRpc::ChannelHealthResult result;
    result.sessionId = reply.sessionId;
    if (expectedSessionId != 0 && reply.sessionId != 0 && reply.sessionId != expectedSessionId) {
        result.status = MemRpc::ChannelHealthStatus::SessionMismatch;
        return result;
    }
    result.status = IsHealthyHeartbeatStatus(reply.status) ? MemRpc::ChannelHealthStatus::Healthy
                                                           : MemRpc::ChannelHealthStatus::Unhealthy;
    return result;
}

bool IsDeadRemoteObject(const OHOS::sptr<IVirusProtectionExecutor>& control)
{
    if (control == nullptr) {
        return false;
    }
    const auto remote = control->AsObject();
    return remote != nullptr && remote->IsObjectDead();
}

std::vector<uint8_t> EncodeAnyCallWire(const VesAnyCallRequest& request)
{
    AnyCallRequestHeader header{};
    header.opcode = request.opcode;
    header.priority = request.priority;
    header.timeoutMs = request.timeoutMs;
    header.payloadSize = static_cast<uint32_t>(request.payload.size());

    std::vector<uint8_t> wire(sizeof(header) + request.payload.size());
    std::memcpy(wire.data(), &header, sizeof(header));
    if (!request.payload.empty()) {
        std::memcpy(wire.data() + sizeof(header), request.payload.data(), request.payload.size());
    }
    return wire;
}

MemRpc::StatusCode DecodeAnyCallWire(const std::vector<uint8_t>& payload, uint16_t opcode, VesAnyCallReply* reply)
{
    if (payload.size() < sizeof(AnyCallReplyHeader)) {
        HILOGE(
            "VesControlProxy::AnyCall protocol mismatch opcode=%{public}u payload_size=%{public}zu "
            "header_size=%{public}zu",
            opcode,
            payload.size(),
            sizeof(AnyCallReplyHeader));
        return MemRpc::StatusCode::ProtocolMismatch;
    }

    AnyCallReplyHeader replyHeader{};
    std::memcpy(&replyHeader, payload.data(), sizeof(replyHeader));
    if (payload.size() != sizeof(replyHeader) + replyHeader.payloadSize) {
        HILOGE(
            "VesControlProxy::AnyCall payload size mismatch opcode=%{public}u payload_size=%{public}zu "
            "declared_payload=%{public}u",
            opcode,
            payload.size(),
            replyHeader.payloadSize);
        return MemRpc::StatusCode::ProtocolMismatch;
    }
    reply->status = static_cast<MemRpc::StatusCode>(replyHeader.status);
    reply->errorCode = replyHeader.errorCode;
    reply->payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(sizeof(replyHeader)), payload.end());
    return MemRpc::StatusCode::Ok;
}

class ControlDeathRecipient final : public OHOS::IRemoteObject::DeathRecipient {
public:
    explicit ControlDeathRecipient(std::function<void()> callback)
        : callback_(std::move(callback))
    {
    }

    void OnRemoteDied(const OHOS::wptr<OHOS::IRemoteObject>&) override
    {
        if (callback_) {
            callback_();
        }
    }

private:
    std::function<void()> callback_;
};

}  // namespace

struct VesBootstrapChannel::DeathRecipientContext {
    std::mutex mutex;
    std::condition_variable cv;
    VesBootstrapChannel* owner = nullptr;
    size_t inFlightCallbacks = 0;
    bool shuttingDown = false;
};

VesBootstrapChannel* VesBootstrapChannel::TryEnterDeathRecipientCallback(DeathRecipientContext& context)
{
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.owner == nullptr || context.shuttingDown) {
        return nullptr;
    }
    ++context.inFlightCallbacks;
    return context.owner;
}

void VesBootstrapChannel::LeaveDeathRecipientCallback(DeathRecipientContext& context)
{
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.inFlightCallbacks > 0) {
        --context.inFlightCallbacks;
    }
    if (context.inFlightCallbacks == 0) {
        context.cv.notify_all();
    }
}

VesControlProxy::VesControlProxy(const OHOS::sptr<OHOS::IRemoteObject>& remote, const std::string& serviceSocketPath)
    : OHOS::IRemoteProxy<IVirusProtectionExecutor>(remote),
      service_socket_path_(serviceSocketPath)
{
}

VesControlProxy::~VesControlProxy()
{
    ResetSocketConnection();
}

void VesControlProxy::StopMonitorThread()
{
    std::thread monitorThread;
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        stop_monitor_.store(true);
        fd = sock_fd_;
        sock_fd_ = -1;
        sessionId_ = 0;
        if (monitor_thread_.joinable()) {
            monitorThread = std::move(monitor_thread_);
        }
    }
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
    JoinThread(&monitorThread);
    if (fd >= 0) {
        close(fd);
    }
}

void VesControlProxy::ResetSocketConnection()
{
    StopMonitorThread();
}

bool VesControlProxy::SendCommand(int fd, char cmd, const std::vector<uint8_t>& payload) const
{
    const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    return send(fd, &cmd, 1, MSG_NOSIGNAL) == 1 && SendAll(fd, &payloadSize, sizeof(payloadSize)) &&
           (payload.empty() || SendAll(fd, payload.data(), payload.size()));
}

MemRpc::StatusCode VesControlProxy::ReceiveSizedReply(int fd, std::vector<uint8_t>* payload, int timeoutMs) const
{
    if (payload == nullptr) {
        HILOGE("VesControlProxy::ReceiveSizedReply failed: payload is null");
        return MemRpc::StatusCode::InvalidArgument;
    }
    if (timeoutMs > 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        const int pollResult = poll(&pfd, 1, timeoutMs);
        if (pollResult <= 0) {
            HILOGE(
                "VesControlProxy::ReceiveSizedReply poll failed fd=%{public}d timeout_ms=%{public}d result=%{public}d",
                fd,
                timeoutMs,
                pollResult);
            return MemRpc::StatusCode::PeerDisconnected;
        }
    }

    uint32_t replySize = 0;
    if (!RecvAll(fd, &replySize, sizeof(replySize))) {
        HILOGE("VesControlProxy::ReceiveSizedReply failed reading size fd=%{public}d", fd);
        return MemRpc::StatusCode::PeerDisconnected;
    }
    payload->resize(replySize);
    if (replySize > 0 && !RecvAll(fd, payload->data(), payload->size())) {
        HILOGE("VesControlProxy::ReceiveSizedReply failed reading payload fd=%{public}d size=%{public}u",
               fd,
               replySize);
        payload->clear();
        return MemRpc::StatusCode::PeerDisconnected;
    }
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VesControlProxy::ReceiveSessionHandles(int fd, MemRpc::BootstrapHandles& handles)
{
    handles = MemRpc::MakeDefaultBootstrapHandles();
    int fds[FD_COUNT] = {-1, -1, -1, -1, -1, -1};
    SessionMetadata meta{};
    size_t data_len = sizeof(meta);
    const size_t received = OHOS::RecvFds(fd, fds, FD_COUNT, &meta, &data_len);
    if (received != FD_COUNT || data_len != sizeof(meta)) {
        HILOGE(
            "VesControlProxy::ReceiveSessionHandles failed fd=%{public}d received=%{public}zu expected=%{public}d "
            "data_len=%{public}zu meta_size=%{public}zu",
            fd,
            received,
            FD_COUNT,
            data_len,
            sizeof(meta));
        for (size_t i = 0; i < received; ++i) {
            close(fds[i]);
        }
        return MemRpc::StatusCode::ProtocolMismatch;
    }

    handles.shmFd = fds[0];
    handles.highReqEventFd = fds[1];
    handles.normalReqEventFd = fds[2];
    handles.respEventFd = fds[3];
    handles.reqCreditEventFd = fds[4];
    handles.respCreditEventFd = fds[5];
    handles.protocolVersion = meta.protocolVersion;
    handles.sessionId = meta.sessionId;
    return MemRpc::StatusCode::Ok;
}

bool VesControlProxy::IsPeerDisconnected(int fd) const
{
    char buf = 0;
    const ssize_t received = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    return received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

void VesControlProxy::NotifyPeerDisconnected()
{
    // Keep the proxy alive while the callback reports the disconnect event.
    const auto self = RetainProxy(this);
    (void)self;
    if (stop_monitor_.load()) {
        return;
    }
    uint64_t sessionId = 0;
    MemRpc::EngineDeathCallback callback;
    {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        sessionId = sessionId_;
    }
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = deathCallback_;
    }
    HILOGW("VesControlProxy::NotifyPeerDisconnected session_id=%{public}llu",
           static_cast<unsigned long long>(sessionId));
    if (callback) {
        callback(sessionId);
    }
}

MemRpc::StatusCode VesControlProxy::OpenSession(const VesOpenSessionRequest& request, MemRpc::BootstrapHandles& handles)
{
    if (!IsValidVesOpenSessionRequest(request)) {
        HILOGE("VesControlProxy::OpenSession failed: invalid request version=%{public}u engines=%{public}zu",
               request.version,
               request.engineKinds.size());
        return MemRpc::StatusCode::InvalidArgument;
    }

    std::vector<uint8_t> payload;
    if (!MemRpc::EncodeMessage(request, &payload)) {
        HILOGE("VesControlProxy::OpenSession encode failed");
        return MemRpc::StatusCode::ProtocolMismatch;
    }

    std::lock_guard<std::mutex> lock(operationMutex_);
    ResetSocketConnection();

    int fd = ConnectToService(service_socket_path_);
    if (fd < 0) {
        HILOGE("connect to %{public}s failed", service_socket_path_.c_str());
        return MemRpc::StatusCode::PeerDisconnected;
    }

    if (!SendCommand(fd, 1, payload)) {
        HILOGE("VesControlProxy::OpenSession send failed path=%{public}s", service_socket_path_.c_str());
        close(fd);
        return MemRpc::StatusCode::PeerDisconnected;
    }

    const MemRpc::StatusCode receive_status = ReceiveSessionHandles(fd, handles);
    if (receive_status != MemRpc::StatusCode::Ok) {
        HILOGE("VesControlProxy::OpenSession receive handles failed status=%{public}d",
               static_cast<int>(receive_status));
        close(fd);
        return receive_status;
    }
    {
        std::lock_guard<std::mutex> stateLock(connectionMutex_);
        auto self = RetainProxy(this);
        stop_monitor_.store(false);
        sock_fd_ = fd;
        sessionId_ = handles.sessionId;
        // Keep the proxy alive until the monitor thread exits.
        if (self != nullptr) {
            monitor_thread_ = std::thread([self]() { self->MonitorSocket(); });
        } else {
            monitor_thread_ = std::thread([this]() { MonitorSocket(); });
        }
    }
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VesControlProxy::HeartbeatWithTimeout(VesHeartbeatReply& reply, int timeoutMs) const
{
    int hb_fd = ConnectToService(service_socket_path_);
    if (hb_fd < 0) {
        HILOGE("VesControlProxy::HeartbeatWithTimeout connect failed path=%{public}s", service_socket_path_.c_str());
        return MemRpc::StatusCode::PeerDisconnected;
    }

    if (!SendCommand(hb_fd, 3)) {
        HILOGE("VesControlProxy::HeartbeatWithTimeout send failed");
        close(hb_fd);
        return MemRpc::StatusCode::PeerDisconnected;
    }

    std::vector<uint8_t> payload;
    const MemRpc::StatusCode receiveStatus = ReceiveSizedReply(hb_fd, &payload, timeoutMs);
    close(hb_fd);
    if (receiveStatus != MemRpc::StatusCode::Ok) {
        HILOGE("VesControlProxy::HeartbeatWithTimeout receive failed status=%{public}d",
               static_cast<int>(receiveStatus));
        return receiveStatus;
    }
    if (payload.size() != sizeof(VesHeartbeatReply)) {
        HILOGE("VesControlProxy::HeartbeatWithTimeout protocol mismatch payload_size=%{public}zu expected=%{public}zu",
               payload.size(),
               sizeof(VesHeartbeatReply));
        return MemRpc::StatusCode::ProtocolMismatch;
    }
    VesHeartbeatReply buf{};
    std::memcpy(&buf, payload.data(), sizeof(buf));
    reply = buf;
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VesControlProxy::Heartbeat(VesHeartbeatReply& reply)
{
    return HeartbeatWithTimeout(reply, DEFAULT_HEARTBEAT_TIMEOUT_MS);
}

MemRpc::StatusCode VesControlProxy::AnyCall(const VesAnyCallRequest& request, VesAnyCallReply& reply)
{
    std::vector<uint8_t> wire = EncodeAnyCallWire(request);

    int fd = ConnectToService(service_socket_path_);
    if (fd < 0) {
        HILOGE("VesControlProxy::AnyCall connect failed path=%{public}s opcode=%{public}u",
               service_socket_path_.c_str(),
               request.opcode);
        return MemRpc::StatusCode::PeerDisconnected;
    }
    if (!SendCommand(fd, 4, wire)) {
        HILOGE("VesControlProxy::AnyCall send failed opcode=%{public}u payload_size=%{public}zu",
               request.opcode,
               request.payload.size());
        close(fd);
        return MemRpc::StatusCode::PeerDisconnected;
    }

    std::vector<uint8_t> payload;
    const MemRpc::StatusCode receiveStatus =
        ReceiveSizedReply(fd, &payload, request.timeoutMs > 0 ? static_cast<int>(request.timeoutMs) : 0);
    close(fd);
    if (receiveStatus != MemRpc::StatusCode::Ok) {
        HILOGE("VesControlProxy::AnyCall receive failed opcode=%{public}u status=%{public}d",
               request.opcode,
               static_cast<int>(receiveStatus));
        return receiveStatus;
    }
    return DecodeAnyCallWire(payload, request.opcode, &reply);
}

MemRpc::StatusCode VesControlProxy::CloseSession()
{
    std::lock_guard<std::mutex> lock(operationMutex_);
    ResetSocketConnection();

    int close_fd = ConnectToService(service_socket_path_);
    if (close_fd < 0) {
        HILOGE("VesControlProxy::CloseSession connect failed path=%{public}s", service_socket_path_.c_str());
        return MemRpc::StatusCode::PeerDisconnected;
    }

    const bool sent = SendCommand(close_fd, 2);
    close(close_fd);
    if (!sent) {
        HILOGE("VesControlProxy::CloseSession send failed");
        return MemRpc::StatusCode::PeerDisconnected;
    }
    return MemRpc::StatusCode::Ok;
}

void VesControlProxy::SetEngineDeathCallback(MemRpc::EngineDeathCallback callback)
{
    std::lock_guard<std::mutex> lock(callbackMutex_);
    deathCallback_ = std::move(callback);
}

void VesControlProxy::MonitorSocket()
{
    int monitoredFd = -1;
    {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        monitoredFd = sock_fd_;
    }
    if (monitoredFd < 0) {
        return;
    }

    struct pollfd pfd {};
    pfd.fd = monitoredFd;
    pfd.events = POLLIN | POLLHUP | POLLERR;

    while (!stop_monitor_) {
        const int poll_result = poll(&pfd, 1, 500);
        if (stop_monitor_) {
            break;
        }
        if (poll_result < 0) {
            HILOGE("VesControlProxy::MonitorSocket poll failed fd=%{public}d", monitoredFd);
            continue;
        }
        if (poll_result <= 0 || !HasDisconnectSignal(pfd) || !IsPeerDisconnected(monitoredFd)) {
            continue;
        }
        NotifyPeerDisconnected();
        break;
    }
}

VesBootstrapChannel::VesBootstrapChannel(ControlLoader controlLoader, VesOpenSessionRequest openSessionRequest)
    : controlLoader_(std::move(controlLoader)),
      deathRecipientContext_(std::make_shared<DeathRecipientContext>()),
      deathRecipient_(std::make_shared<ControlDeathRecipient>([context = deathRecipientContext_]() {
          auto* owner = TryEnterDeathRecipientCallback(*context);
          if (owner == nullptr) {
              return;
          }

          try {
              owner->HandleRemoteDied();
          } catch (...) {
              HILOGW("VesBootstrapChannel death recipient callback threw");
          }
          LeaveDeathRecipientCallback(*context);
      })),
      openSessionRequest_(std::move(openSessionRequest))
{
    if (!controlLoader_) {
        AbortForMissingControlLoader();
    }
    openSessionRequest_.engineKinds = NormalizeVesEngineKinds(std::move(openSessionRequest_.engineKinds));
    {
        std::lock_guard<std::mutex> guard(deathRecipientContext_->mutex);
        deathRecipientContext_->owner = this;
    }
}

VesBootstrapChannel::~VesBootstrapChannel()
{
    ShutdownDeathRecipient();
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_ != nullptr && control_->AsObject() != nullptr && deathRecipient_ != nullptr) {
        (void)control_->AsObject()->RemoveDeathRecipient(deathRecipient_);
    }
}

void VesBootstrapChannel::HandleRemoteDied()
{
    uint64_t sessionId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessionId = sessionId_;
        RebindControlLocked(nullptr);
    }
    NotifyEngineDeath(sessionId);
}

void VesBootstrapChannel::ShutdownDeathRecipient()
{
    const auto context = deathRecipientContext_;
    if (context == nullptr) {
        return;
    }

    std::unique_lock<std::mutex> lock(context->mutex);
    context->owner = nullptr;
    context->shuttingDown = true;
    context->cv.wait(lock, [&context]() { return context->inFlightCallbacks == 0; });
}

OHOS::sptr<IVirusProtectionExecutor> VesBootstrapChannel::EnsureControlBoundLocked()
{
    if (control_ != nullptr) {
        return control_;
    }
    return RefreshControlLocked();
}

OHOS::sptr<IVirusProtectionExecutor> VesBootstrapChannel::RefreshControlLocked()
{
    auto nextControl = controlLoader_();
    if (nextControl != nullptr) {
        RebindControlLocked(nextControl);
        return control_;
    }
    return nullptr;
}

void VesBootstrapChannel::RebindControlLocked(const OHOS::sptr<IVirusProtectionExecutor>& nextControl)
{
    auto previousObject = control_ != nullptr ? control_->AsObject() : nullptr;
    auto nextObject = nextControl != nullptr ? nextControl->AsObject() : nullptr;
    if (previousObject != nullptr && deathRecipient_ != nullptr && previousObject != nextObject) {
        (void)previousObject->RemoveDeathRecipient(deathRecipient_);
    }

    control_ = nextControl;
    if (nextObject != nullptr && deathRecipient_ != nullptr && nextObject != previousObject) {
        (void)nextObject->AddDeathRecipient(deathRecipient_);
    }
}

void VesBootstrapChannel::NotifyEngineDeath(uint64_t sessionId)
{
    MemRpc::EngineDeathCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = deathCallback_;
        if (sessionId == 0) {
            sessionId = sessionId_;
        }
    }
    if (callback) {
        callback(sessionId);
    }
}

OHOS::sptr<IVirusProtectionExecutor> VesBootstrapChannel::LoadOpenSessionControl(VesOpenSessionRequest* request,
                                                                                 MemRpc::BootstrapHandles* handles,
                                                                                 bool* deadBeforeOpen)
{
    OHOS::sptr<IVirusProtectionExecutor> control;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        *request = openSessionRequest_;
        control = EnsureControlBoundLocked();
        *deadBeforeOpen = IsDeadRemoteObject(control);
        if (*deadBeforeOpen) {
            control = RefreshControlLocked();
        }
        *handles = MemRpc::MakeDefaultBootstrapHandles();
    }
    return control;
}

MemRpc::StatusCode VesBootstrapChannel::RetryOpenSessionAfterRefresh(const VesOpenSessionRequest& request,
                                                                     MemRpc::BootstrapHandles* handles,
                                                                     MemRpc::StatusCode initialStatus)
{
    HILOGW("VesBootstrapChannel::OpenSession retrying after recoverable failure: status=%{public}d",
           static_cast<int>(initialStatus));

    OHOS::sptr<IVirusProtectionExecutor> control;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        control = RefreshControlLocked();
    }
    if (control == nullptr) {
        HILOGE("VesBootstrapChannel::OpenSession rebind failed: control is null status=%{public}d",
               static_cast<int>(initialStatus));
        *handles = MemRpc::MakeDefaultBootstrapHandles();
        return initialStatus;
    }

    MemRpc::StatusCode status = control->OpenSession(request, *handles);
    if (status == MemRpc::StatusCode::Ok) {
        HILOGW("VesBootstrapChannel::OpenSession recovered after control refresh");
        std::lock_guard<std::mutex> lock(mutex_);
        sessionId_ = handles->sessionId;
    } else {
        HILOGW("VesBootstrapChannel::OpenSession retry failed after control refresh: status=%{public}d",
               static_cast<int>(status));
    }
    return status;
}

MemRpc::StatusCode VesBootstrapChannel::OpenSession(MemRpc::BootstrapHandles& handles)
{
    VesOpenSessionRequest request;
    bool deadBeforeOpen = false;
    OHOS::sptr<IVirusProtectionExecutor> control = LoadOpenSessionControl(&request, &handles, &deadBeforeOpen);
    if (control == nullptr) {
        if (deadBeforeOpen) {
            HILOGE("VesBootstrapChannel::OpenSession failed: control remote is dead before OpenSession");
            return MemRpc::StatusCode::PeerDisconnected;
        }
        HILOGE("VesBootstrapChannel::OpenSession failed: control is null before OpenSession");
        return MemRpc::StatusCode::InvalidArgument;
    }
    if (deadBeforeOpen && IsDeadRemoteObject(control)) {
        HILOGE("VesBootstrapChannel::OpenSession failed: refreshed control remote is still dead");
        return MemRpc::StatusCode::PeerDisconnected;
    }

    MemRpc::StatusCode status = control->OpenSession(request, handles);
    if (status == MemRpc::StatusCode::Ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessionId_ = handles.sessionId;
        return status;
    }
    if (!ShouldRetryOpenSessionAfterRefresh(status)) {
        return status;
    }
    return RetryOpenSessionAfterRefresh(request, &handles, status);
}

MemRpc::StatusCode VesBootstrapChannel::CloseSession()
{
    OHOS::sptr<IVirusProtectionExecutor> control;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        control = control_;
        RebindControlLocked(nullptr);
        sessionId_ = 0;
    }
    if (control == nullptr) {
        return MemRpc::StatusCode::Ok;
    }
    return control->CloseSession();
}

MemRpc::ChannelHealthResult VesBootstrapChannel::CheckHealth(uint64_t expectedSessionId)
{
    OHOS::sptr<IVirusProtectionExecutor> control;
    uint64_t sessionId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        control = control_;
        sessionId = sessionId_;
    }
    if (control == nullptr) {
        if (sessionId != 0) {
            return {MemRpc::ChannelHealthStatus::Unhealthy, sessionId};
        }
        return {};
    }
    if (IsDeadRemoteObject(control)) {
        return {MemRpc::ChannelHealthStatus::Unhealthy, sessionId};
    }

    VesHeartbeatReply reply{};
    const MemRpc::StatusCode status = control->Heartbeat(reply);
    if (status == MemRpc::StatusCode::Ok) {
        return ToHealthResult(reply, expectedSessionId);
    }
    if (status == MemRpc::StatusCode::ProtocolMismatch) {
        return {MemRpc::ChannelHealthStatus::Malformed, 0};
    }
    return {MemRpc::ChannelHealthStatus::Timeout, 0};
}

OHOS::sptr<IVirusProtectionExecutor> VesBootstrapChannel::CurrentControl()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return EnsureControlBoundLocked();
}

void VesBootstrapChannel::SetEngineDeathCallback(MemRpc::EngineDeathCallback callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    deathCallback_ = std::move(callback);
}

}  // namespace VirusExecutorService
