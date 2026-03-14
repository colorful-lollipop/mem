#include "transport/ves_control_proxy.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <utility>
#include <unistd.h>

#include "scm_rights.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

namespace {

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

bool HasDisconnectSignal(const pollfd& pfd)
{
    return (pfd.revents & (POLLHUP | POLLERR | POLLIN)) != 0;
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
    stop_monitor_ = true;
    if (sock_fd_ >= 0) {
        shutdown(sock_fd_, SHUT_RDWR);
    }
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

void VesControlProxy::CloseSocket() {
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
}

void VesControlProxy::ResetSocketConnection() {
    StopMonitorThread();
    CloseSocket();
}

bool VesControlProxy::SendCommand(int fd, char cmd) const {
    return send(fd, &cmd, 1, MSG_NOSIGNAL) == 1;
}

MemRpc::StatusCode VesControlProxy::ReceiveSessionHandles(MemRpc::BootstrapHandles& handles) {
    handles = MemRpc::BootstrapHandles{};
    int fds[FD_COUNT] = {-1, -1, -1, -1, -1, -1};
    SessionMetadata meta{};
    size_t data_len = sizeof(meta);
    const size_t received = OHOS::RecvFds(sock_fd_, fds, FD_COUNT, &meta, &data_len);
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
    sessionId_ = meta.sessionId;
    return MemRpc::StatusCode::Ok;
}

bool VesControlProxy::IsPeerDisconnected() const {
    char buf = 0;
    const ssize_t received = recv(sock_fd_, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    return received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

void VesControlProxy::NotifyPeerDisconnected() {
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        if (deathCallback_) {
            deathCallback_(sessionId_);
        }
    }
    auto object = AsObject();
    if (object != nullptr) {
        object->NotifyRemoteDiedForTest();
    }
}

MemRpc::StatusCode VesControlProxy::OpenSession(MemRpc::BootstrapHandles& handles) {
    ResetSocketConnection();

    sock_fd_ = ConnectToService(service_socket_path_);
    if (sock_fd_ < 0) {
        HILOGE("connect to %{public}s failed", service_socket_path_.c_str());
        return MemRpc::StatusCode::PeerDisconnected;
    }

    if (!SendCommand(sock_fd_, 1)) {
        ResetSocketConnection();
        return MemRpc::StatusCode::PeerDisconnected;
    }

    const MemRpc::StatusCode receive_status = ReceiveSessionHandles(handles);
    if (receive_status != MemRpc::StatusCode::Ok) {
        ResetSocketConnection();
        return receive_status;
    }
    stop_monitor_ = false;
    monitor_thread_ = std::thread(&VesControlProxy::MonitorSocket, this);
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VesControlProxy::Heartbeat(VesHeartbeatReply& reply) {
    // Use a short-lived connection for heartbeat (server closes after reply).
    int hb_fd = ConnectToService(service_socket_path_);
    if (hb_fd < 0) {
        return MemRpc::StatusCode::PeerDisconnected;
    }

    if (!SendCommand(hb_fd, 3)) {
        close(hb_fd);
        return MemRpc::StatusCode::PeerDisconnected;
    }

    VesHeartbeatReply buf{};
    ssize_t n = recv(hb_fd, &buf, sizeof(buf), MSG_WAITALL);
    close(hb_fd);

    if (n != static_cast<ssize_t>(sizeof(buf))) {
        return MemRpc::StatusCode::ProtocolMismatch;
    }
    reply = buf;
    return MemRpc::StatusCode::Ok;
}

MemRpc::StatusCode VesControlProxy::CloseSession() {
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

void VesControlProxy::SetEngineDeathCallback(MemRpc::EngineDeathCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    deathCallback_ = std::move(callback);
}

void VesControlProxy::MonitorSocket() {
    struct pollfd pfd{};
    pfd.fd = sock_fd_;
    pfd.events = POLLIN | POLLHUP | POLLERR;

    while (!stop_monitor_) {
        const int poll_result = poll(&pfd, 1, 500);
        if (poll_result <= 0 || !HasDisconnectSignal(pfd) || !IsPeerDisconnected()) {
            continue;
        }
        NotifyPeerDisconnected();
        break;
    }
}

VesControlChannelAdapter::VesControlChannelAdapter(
    std::shared_ptr<VesControlProxy> proxy)
    : proxy_(std::move(proxy)) {}

MemRpc::StatusCode VesControlChannelAdapter::OpenSession(
    MemRpc::BootstrapHandles& handles) {
    if (proxy_ == nullptr) {
        handles = MemRpc::BootstrapHandles{};
        return MemRpc::StatusCode::InvalidArgument;
    }
    return proxy_->OpenSession(handles);
}

MemRpc::StatusCode VesControlChannelAdapter::CloseSession() {
    if (proxy_ == nullptr) {
        return MemRpc::StatusCode::Ok;
    }
    return proxy_->CloseSession();
}

void VesControlChannelAdapter::SetEngineDeathCallback(
    MemRpc::EngineDeathCallback callback) {
    if (proxy_ != nullptr) {
        proxy_->SetEngineDeathCallback(std::move(callback));
    }
}

}  // namespace VirusExecutorService
