#include "mock_service_socket.h"

#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "scm_rights.h"

namespace OHOS {

MockServiceSocket::~MockServiceSocket() {
    Stop();
}

bool MockServiceSocket::Start(const std::string& path, MockIpcHandler handler) {
    handler_ = std::move(handler);
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
    accept_thread_ = std::thread(&MockServiceSocket::AcceptLoop, this);
    return true;
}

void MockServiceSocket::Stop() {
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

void MockServiceSocket::AcceptLoop() {
    while (!stop_) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            break;
        }

        // Read 1-byte command.
        char cmd = 0;
        ssize_t n = recv(client_fd, &cmd, 1, 0);
        if (n != 1) {
            close(client_fd);
            continue;
        }

        // Dispatch to handler.
        MockIpcReply reply{};
        if (!handler_ || !handler_(static_cast<int>(cmd), &reply)) {
            close(client_fd);
            continue;
        }

        // Send fds + data via SCM_RIGHTS.
        if (reply.fd_count > 0) {
            if (!SendFds(client_fd, reply.fds, reply.fd_count,
                         reply.data, reply.data_len)) {
                std::cerr << "[MockServiceSocket] failed to send fds" << std::endl;
            }
        }

        // Keep client_fd open — proxy monitors it for death detection.
        // The socket will close when the process exits.
    }
}

}  // namespace OHOS
