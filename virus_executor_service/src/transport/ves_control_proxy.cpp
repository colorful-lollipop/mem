#include "transport/ves_control_proxy.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <utility>
#include <unistd.h>

#include "iremote_broker.h"
#include "scm_rights.h"
#include "iservice_registry.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

namespace {
constexpr int DEFAULT_HEARTBEAT_TIMEOUT_MS = 500;
constexpr int HEALTH_CHECK_TIMEOUT_MS = 100;
constexpr int LOAD_SERVICE_TIMEOUT_MS = 5000;

int ConnectToService(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr{};
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

}  // namespace

VesControlProxy::VesControlProxy(
    const OHOS::sptr<OHOS::IRemoteObject>& remote,
    const std::string& serviceSocketPath)
    : OHOS::IRemoteProxy<IVesControl>(remote),
      service_socket_path_(serviceSocketPath) {}

VesControlProxy::~VesControlProxy() {
    ResetSocketConnection();
}

void VesControlProxy::StopMonitorThread() {
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

void VesControlProxy::ResetSocketConnection() {
    StopMonitorThread();
}

bool VesControlProxy::SendCommand(int fd, char cmd, const std::vector<uint8_t>& payload) const {
    const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    return send(fd, &cmd, 1, MSG_NOSIGNAL) == 1 &&
           SendAll(fd, &payloadSize, sizeof(payloadSize)) &&
           (payload.empty() || SendAll(fd, payload.data(), payload.size()));
}

MemRpc::StatusCode VesControlProxy::ReceiveSizedReply(int fd, std::vector<uint8_t>* payload,
                                                      int timeoutMs) const {
    if (payload == nullptr) {
        return MemRpc::StatusCode::InvalidArgument;
    }
    if (timeoutMs > 0) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        const int pollResult = poll(&pfd, 1, timeoutMs);
        if (pollResult <= 0) {
            return MemRpc::StatusCode::PeerDisconnected;
        }
    }

    uint32_t replySize = 0;
    if (!RecvAll(fd, &replySize, sizeof(replySize))) {
        return MemRpc::StatusCode::PeerDisconnected;
    }
    payload->resize(replySize);
    if (replySize > 0 && !RecvAll(fd, payload->data(), payload->size())) {
        payload->clear();
        return MemRpc::StatusCode::PeerDisconnected;
    }
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VesControlProxy::ReceiveSessionHandles(int fd, MemRpc::BootstrapHandles& handles) {
    handles = MemRpc::BootstrapHandles{};
    int fds[FD_COUNT] = {-1, -1, -1, -1, -1, -1};
    SessionMetadata meta{};
    size_t data_len = sizeof(meta);
    const size_t received = OHOS::RecvFds(fd, fds, FD_COUNT, &meta, &data_len);
    if (received != FD_COUNT || data_len != sizeof(meta)) {
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

bool VesControlProxy::IsPeerDisconnected(int fd) const {
    char buf = 0;
    const ssize_t received = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    return received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

void VesControlProxy::NotifyPeerDisconnected() {
    if (stop_monitor_.load()) {
        return;
    }
    uint64_t sessionId = 0;
    {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        sessionId = sessionId_;
    }
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (deathCallback_) {
            deathCallback_(sessionId);
        }
    }
    auto object = AsObject();
    if (object != nullptr) {
        object->NotifyRemoteDiedForTest();
    }
}

MemRpc::StatusCode VesControlProxy::OpenSession(MemRpc::BootstrapHandles& handles) {
    std::lock_guard<std::mutex> lock(operationMutex_);
    ResetSocketConnection();

    int fd = ConnectToService(service_socket_path_);
    if (fd < 0) {
        HILOGE("connect to %{public}s failed", service_socket_path_.c_str());
        return MemRpc::StatusCode::PeerDisconnected;
    }

    if (!SendCommand(fd, 1)) {
        close(fd);
        return MemRpc::StatusCode::PeerDisconnected;
    }

    const MemRpc::StatusCode receive_status = ReceiveSessionHandles(fd, handles);
    if (receive_status != MemRpc::StatusCode::Ok) {
        close(fd);
        return receive_status;
    }
    {
        std::lock_guard<std::mutex> stateLock(connectionMutex_);
        stop_monitor_.store(false);
        sock_fd_ = fd;
        sessionId_ = handles.sessionId;
        monitor_thread_ = std::thread(&VesControlProxy::MonitorSocket, this);
    }
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VesControlProxy::HeartbeatWithTimeout(VesHeartbeatReply& reply,
                                                         int timeoutMs) const {
    int hb_fd = ConnectToService(service_socket_path_);
    if (hb_fd < 0) {
        return MemRpc::StatusCode::PeerDisconnected;
    }

    if (!SendCommand(hb_fd, 3)) {
        close(hb_fd);
        return MemRpc::StatusCode::PeerDisconnected;
    }

    std::vector<uint8_t> payload;
    const MemRpc::StatusCode receiveStatus = ReceiveSizedReply(hb_fd, &payload, timeoutMs);
    close(hb_fd);
    if (receiveStatus != MemRpc::StatusCode::Ok) {
        return receiveStatus;
    }
    if (payload.size() != sizeof(VesHeartbeatReply)) {
        return MemRpc::StatusCode::ProtocolMismatch;
    }
    VesHeartbeatReply buf{};
    std::memcpy(&buf, payload.data(), sizeof(buf));
    reply = buf;
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VesControlProxy::Heartbeat(VesHeartbeatReply& reply) {
    return HeartbeatWithTimeout(reply, DEFAULT_HEARTBEAT_TIMEOUT_MS);
}

MemRpc::StatusCode VesControlProxy::AnyCall(const VesAnyCallRequest& request,
                                            VesAnyCallReply& reply) {
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

    int fd = ConnectToService(service_socket_path_);
    if (fd < 0) {
        return MemRpc::StatusCode::PeerDisconnected;
    }
    if (!SendCommand(fd, 4, wire)) {
        close(fd);
        return MemRpc::StatusCode::PeerDisconnected;
    }

    std::vector<uint8_t> payload;
    const MemRpc::StatusCode receiveStatus =
        ReceiveSizedReply(fd, &payload, request.timeoutMs > 0 ? static_cast<int>(request.timeoutMs) : 0);
    close(fd);
    if (receiveStatus != MemRpc::StatusCode::Ok) {
        return receiveStatus;
    }
    if (payload.size() < sizeof(AnyCallReplyHeader)) {
        return MemRpc::StatusCode::ProtocolMismatch;
    }

    AnyCallReplyHeader replyHeader{};
    std::memcpy(&replyHeader, payload.data(), sizeof(replyHeader));
    if (payload.size() != sizeof(replyHeader) + replyHeader.payloadSize) {
        return MemRpc::StatusCode::ProtocolMismatch;
    }
    reply.status = static_cast<MemRpc::StatusCode>(replyHeader.status);
    reply.errorCode = replyHeader.errorCode;
    reply.payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(sizeof(replyHeader)),
                         payload.end());
    return MemRpc::StatusCode::Ok;
}

void VesControlProxy::PublishHealthSnapshot(const VesHeartbeatReply& reply) {
    HealthSnapshotCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = healthSnapshotCallback_;
    }
    if (callback) {
        std::thread([callback = std::move(callback), reply]() mutable {
            try {
                callback(reply);
            } catch (...) {
                HILOGW("health snapshot callback threw");
            }
        }).detach();
    }
}

MemRpc::ChannelHealthResult VesControlProxy::CheckHealth(uint64_t expectedSessionId) {
    VesHeartbeatReply reply{};
    const MemRpc::StatusCode status = HeartbeatWithTimeout(reply, HEALTH_CHECK_TIMEOUT_MS);
    if (status == MemRpc::StatusCode::Ok) {
        PublishHealthSnapshot(reply);
        MemRpc::ChannelHealthResult result;
        result.sessionId = reply.sessionId;
        if (expectedSessionId != 0 && reply.sessionId != 0 && reply.sessionId != expectedSessionId) {
            result.status = MemRpc::ChannelHealthStatus::SessionMismatch;
            return result;
        }
        result.status = IsHealthyHeartbeatStatus(reply.status)
                            ? MemRpc::ChannelHealthStatus::Healthy
                            : MemRpc::ChannelHealthStatus::Unhealthy;
        return result;
    }
    if (status == MemRpc::StatusCode::ProtocolMismatch) {
        return {MemRpc::ChannelHealthStatus::Malformed, 0};
    }
    return {MemRpc::ChannelHealthStatus::Timeout, 0};
}

MemRpc::StatusCode VesControlProxy::CloseSession() {
    std::lock_guard<std::mutex> lock(operationMutex_);
    ResetSocketConnection();

    int close_fd = ConnectToService(service_socket_path_);
    if (close_fd < 0) {
        return MemRpc::StatusCode::PeerDisconnected;
    }

    const bool sent = SendCommand(close_fd, 2);
    close(close_fd);
    if (!sent) {
        return MemRpc::StatusCode::PeerDisconnected;
    }
    return MemRpc::StatusCode::Ok;
}

void VesControlProxy::SetHealthSnapshotCallback(HealthSnapshotCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    healthSnapshotCallback_ = std::move(callback);
}

void VesControlProxy::SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    deathCallback_ = std::move(callback);
}

void VesControlProxy::MonitorSocket() {
    int monitoredFd = -1;
    {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        monitoredFd = sock_fd_;
    }
    if (monitoredFd < 0) {
        return;
    }

    struct pollfd pfd{};
    pfd.fd = monitoredFd;
    pfd.events = POLLIN | POLLHUP | POLLERR;

    while (!stop_monitor_) {
        const int poll_result = poll(&pfd, 1, 500);
        if (stop_monitor_) {
            break;
        }
        if (poll_result <= 0 || !HasDisconnectSignal(pfd) || !IsPeerDisconnected(monitoredFd)) {
            continue;
        }
        NotifyPeerDisconnected();
        break;
    }
}

VesControlChannelAdapter::VesControlChannelAdapter(
    std::shared_ptr<VesControlProxy> proxy)
    : proxy_(std::move(proxy)) {}

std::shared_ptr<VesControlProxy> VesControlChannelAdapter::ReloadProxyLocked(bool forceReload) {
    int32_t saId = VES_CONTROL_SA_ID;
    if (proxy_ != nullptr && proxy_->AsObject() != nullptr && proxy_->AsObject()->GetSaId() >= 0) {
        saId = proxy_->AsObject()->GetSaId();
    }
    auto sam = OHOS::SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (sam == nullptr) {
        return nullptr;
    }
    if (forceReload) {
        (void)sam->UnloadSystemAbility(saId);
    }
    auto remote = sam->LoadSystemAbility(saId, LOAD_SERVICE_TIMEOUT_MS);
    if (remote == nullptr) {
        return nullptr;
    }
    auto control = OHOS::iface_cast<IVesControl>(remote);
    auto proxy = std::dynamic_pointer_cast<VesControlProxy>(control);
    if (proxy != nullptr) {
        proxy->SetEngineDeathCallback(deathCallback_);
        proxy->SetHealthSnapshotCallback(healthSnapshotCallback_);
        proxy_ = proxy;
    }
    return proxy_;
}

MemRpc::StatusCode VesControlChannelAdapter::OpenSession(
    MemRpc::BootstrapHandles& handles) {
    std::shared_ptr<VesControlProxy> proxy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        proxy = proxy_;
    }
    if (proxy == nullptr) {
        handles = MemRpc::BootstrapHandles{};
        std::lock_guard<std::mutex> lock(mutex_);
        proxy = ReloadProxyLocked(false);
        if (proxy == nullptr) {
            return MemRpc::StatusCode::InvalidArgument;
        }
    }
    MemRpc::StatusCode status = proxy->OpenSession(handles);
    if (status == MemRpc::StatusCode::Ok) {
        return status;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    proxy = ReloadProxyLocked(status == MemRpc::StatusCode::PeerDisconnected);
    if (proxy == nullptr) {
        handles = MemRpc::BootstrapHandles{};
        return status;
    }
    return proxy->OpenSession(handles);
}

MemRpc::StatusCode VesControlChannelAdapter::CloseSession() {
    std::shared_ptr<VesControlProxy> proxy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        proxy = proxy_;
    }
    if (proxy == nullptr) {
        return MemRpc::StatusCode::Ok;
    }
    return proxy->CloseSession();
}

MemRpc::ChannelHealthResult VesControlChannelAdapter::CheckHealth(uint64_t expectedSessionId) {
    std::shared_ptr<VesControlProxy> proxy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        proxy = proxy_;
    }
    if (proxy == nullptr) {
        return {};
    }
    return proxy->CheckHealth(expectedSessionId);
}

void VesControlChannelAdapter::SetHealthSnapshotCallback(HealthSnapshotCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    healthSnapshotCallback_ = std::move(callback);
    if (proxy_ != nullptr) {
        proxy_->SetHealthSnapshotCallback(healthSnapshotCallback_);
    }
}

void VesControlChannelAdapter::SetEngineDeathCallback(
    MemRpc::EngineDeathCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    deathCallback_ = std::move(callback);
    if (proxy_ != nullptr) {
        proxy_->SetEngineDeathCallback(deathCallback_);
    }
}

}  // namespace VirusExecutorService
