#include "vps_bootstrap_proxy.h"

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include "scm_rights.h"
#include "virus_protection_service_log.h"

namespace vpsdemo {

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
    uint32_t protocol_version = 0;
    uint64_t session_id = 0;
};

}  // namespace

VpsBootstrapProxy::VpsBootstrapProxy(
    const OHOS::sptr<OHOS::IRemoteObject>& remote,
    const std::string& serviceSocketPath)
    : OHOS::IRemoteProxy<IVpsBootstrap>(remote),
      service_socket_path_(serviceSocketPath) {}

VpsBootstrapProxy::~VpsBootstrapProxy() {
    stop_monitor_ = true;
    if (sock_fd_ >= 0) {
        shutdown(sock_fd_, SHUT_RDWR);
    }
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
}

memrpc::StatusCode VpsBootstrapProxy::OpenSession(memrpc::BootstrapHandles* handles) {
    if (handles == nullptr) {
        return memrpc::StatusCode::InvalidArgument;
    }

    sock_fd_ = ConnectToService(service_socket_path_);
    if (sock_fd_ < 0) {
        HLOGE("connect to %{public}s failed", service_socket_path_.c_str());
        return memrpc::StatusCode::PeerDisconnected;
    }

    // Send "open_session" command (just 1 byte).
    char cmd = 1;
    if (send(sock_fd_, &cmd, 1, MSG_NOSIGNAL) != 1) {
        close(sock_fd_);
        sock_fd_ = -1;
        return memrpc::StatusCode::PeerDisconnected;
    }

    // Receive 6 FDs + metadata via SCM_RIGHTS.
    constexpr size_t FD_COUNT = 6;
    int fds[FD_COUNT] = {-1, -1, -1, -1, -1, -1};
    SessionMetadata meta{};
    size_t data_len = sizeof(meta);
    size_t received = RecvFds(sock_fd_, fds, FD_COUNT, &meta, &data_len);
    if (received != FD_COUNT || data_len != sizeof(meta)) {
        for (size_t i = 0; i < received; ++i) {
            close(fds[i]);
        }
        close(sock_fd_);
        sock_fd_ = -1;
        return memrpc::StatusCode::ProtocolMismatch;
    }

    handles->shm_fd = fds[0];
    handles->high_req_event_fd = fds[1];
    handles->normal_req_event_fd = fds[2];
    handles->resp_event_fd = fds[3];
    handles->req_credit_event_fd = fds[4];
    handles->resp_credit_event_fd = fds[5];
    handles->protocol_version = meta.protocol_version;
    handles->session_id = meta.session_id;
    session_id_ = meta.session_id;

    // Start monitoring the socket for disconnect (death detection).
    stop_monitor_ = false;
    monitor_thread_ = std::thread(&VpsBootstrapProxy::MonitorSocket, this);

    return memrpc::StatusCode::Ok;
}

memrpc::StatusCode VpsBootstrapProxy::CloseSession() {
    stop_monitor_ = true;
    if (sock_fd_ >= 0) {
        shutdown(sock_fd_, SHUT_RDWR);
    }
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    return memrpc::StatusCode::Ok;
}

void VpsBootstrapProxy::SetEngineDeathCallback(memrpc::EngineDeathCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    death_callback_ = std::move(callback);
}

void VpsBootstrapProxy::MonitorSocket() {
    struct pollfd pfd{};
    pfd.fd = sock_fd_;
    pfd.events = POLLIN | POLLHUP | POLLERR;

    while (!stop_monitor_) {
        int ret = poll(&pfd, 1, 500);  // 500ms timeout
        if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLIN))) {
            // Check if peer closed.
            char buf;
            ssize_t n = recv(sock_fd_, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                // Peer disconnected.
                // Framework path — RpcClient will clean up session and fail pending futures.
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    if (death_callback_) {
                        death_callback_(session_id_);
                    }
                }
                // OHOS path — trigger DeathRecipients registered on the remote object.
                auto object = AsObject();
                if (object != nullptr) {
                    object->NotifyRemoteDiedForTest();
                }
                break;
            }
        }
    }
}

}  // namespace vpsdemo
