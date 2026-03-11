#include "vps_bootstrap_stub.h"

#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "scm_rights.h"

namespace vpsdemo {

namespace {

struct SessionMetadata {
    uint32_t protocol_version = 0;
    uint64_t session_id = 0;
};

}  // namespace

VpsBootstrapStub::VpsBootstrapStub()
    : OHOS::SystemAbility(VPS_BOOTSTRAP_SA_ID, true) {}

VpsBootstrapStub::~VpsBootstrapStub() {
    StopServiceSocket();
}

void VpsBootstrapStub::SetBootstrapHandles(const memrpc::BootstrapHandles& handles) {
    handles_ = handles;
}

bool VpsBootstrapStub::StartServiceSocket(const std::string& path) {
    socket_path_ = path;
    unlink(path.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 4) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    stop_ = false;
    accept_thread_ = std::thread(&VpsBootstrapStub::AcceptLoop, this);
    return true;
}

void VpsBootstrapStub::StopServiceSocket() {
    stop_ = true;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
        socket_path_.clear();
    }
}

void VpsBootstrapStub::AcceptLoop() {
    while (!stop_) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            break;
        }

        // Read 1-byte command.
        char cmd = 0;
        ssize_t n = recv(client_fd, &cmd, 1, 0);
        if (n != 1 || cmd != 1) {
            close(client_fd);
            continue;
        }

        // Send 6 fds + metadata.
        constexpr size_t FD_COUNT = 6;
        int fds[FD_COUNT] = {
            handles_.shm_fd,
            handles_.high_req_event_fd,
            handles_.normal_req_event_fd,
            handles_.resp_event_fd,
            handles_.req_credit_event_fd,
            handles_.resp_credit_event_fd,
        };

        SessionMetadata meta{};
        meta.protocol_version = handles_.protocol_version;
        meta.session_id = handles_.session_id;

        if (!SendFds(client_fd, fds, FD_COUNT, &meta, sizeof(meta))) {
            std::cerr << "[engine] failed to send fds" << std::endl;
        }

        // Keep the socket open — proxy monitors it for death detection.
        // The socket will close when the engine process exits.
        // We intentionally leak client_fd here; the OS cleans up on process exit.
    }
}

memrpc::StatusCode VpsBootstrapStub::OpenSession(memrpc::BootstrapHandles* handles) {
    // Direct call path (not used in cross-process mode; SCM_RIGHTS path is used).
    if (handles == nullptr) {
        return memrpc::StatusCode::InvalidArgument;
    }
    *handles = handles_;
    return memrpc::StatusCode::Ok;
}

memrpc::StatusCode VpsBootstrapStub::CloseSession() {
    return memrpc::StatusCode::Ok;
}

void VpsBootstrapStub::OnStart() {
    std::cout << "[engine] OnStart sa_id=" << GetSystemAbilityId() << std::endl;
}

void VpsBootstrapStub::OnStop() {
    std::cout << "[engine] OnStop" << std::endl;
    StopServiceSocket();
}

}  // namespace vpsdemo
