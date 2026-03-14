#include "mock_service_socket.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "scm_rights.h"

namespace OHOS {

namespace {

bool RecvAll(int fd, void* buffer, size_t size) {
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

bool SendAll(int fd, const void* buffer, size_t size) {
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

}  // namespace

MockServiceSocket::~MockServiceSocket() {
    Stop();
}

bool MockServiceSocket::Start(const std::string& path, MockIpcHandler handler) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = std::move(handler);
        socket_path_ = path;
    }
    unlink(path.c_str());

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd);
        return false;
    }

    if (listen(listen_fd, 4) < 0) {
        close(listen_fd);
        return false;
    }

    stop_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listen_fd_ = listen_fd;
    }
    accept_thread_ = std::thread(&MockServiceSocket::AcceptLoop, this);
    return true;
}

void MockServiceSocket::Stop() {
    stop_ = true;
    int listen_fd = -1;
    std::string socket_path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listen_fd = listen_fd_;
        listen_fd_ = -1;
        socket_path = std::move(socket_path_);
    }
    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
    }
    if (accept_thread_.joinable()) {
        if (accept_thread_.get_id() == std::this_thread::get_id()) {
            accept_thread_.detach();
        } else {
            accept_thread_.join();
        }
    }
    if (!socket_path.empty()) {
        unlink(socket_path.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = {};
    }
}

void MockServiceSocket::AcceptLoop() {
    while (!stop_) {
        int listen_fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            listen_fd = listen_fd_;
        }
        if (listen_fd < 0) {
            break;
        }

        int client_fd = accept(listen_fd, nullptr, nullptr);
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

        uint32_t request_size = 0;
        if (!RecvAll(client_fd, &request_size, sizeof(request_size))) {
            close(client_fd);
            continue;
        }

        MockIpcRequest request{};
        request.data.resize(request_size);
        if (request_size > 0 &&
            !RecvAll(client_fd, request.data.data(), request.data.size())) {
            close(client_fd);
            continue;
        }

        // Dispatch to handler.
        MockIpcReply reply{};
        MockIpcHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = handler_;
        }
        if (!handler || !handler(static_cast<int>(cmd), request, &reply)) {
            close(client_fd);
            continue;
        }

        // Send fds + data via SCM_RIGHTS, or data-only reply.
        if (reply.fd_count > 0) {
            if (!SendFds(client_fd, reply.fds, reply.fd_count,
                         reply.data.data(), reply.data.size())) {
                std::cerr << "[MockServiceSocket] failed to send fds" << std::endl;
            }
        } else {
            const uint32_t reply_size = static_cast<uint32_t>(reply.data.size());
            if (!SendAll(client_fd, &reply_size, sizeof(reply_size)) ||
                (reply_size > 0 &&
                 !SendAll(client_fd, reply.data.data(), reply.data.size()))) {
                std::cerr << "[MockServiceSocket] failed to send data reply" << std::endl;
            }
        }

        if (reply.close_after_reply) {
            close(client_fd);
            continue;
        }

        // Keep client_fd open — proxy monitors it for death detection.
        // The socket will close when the process exits.
    }
}

}  // namespace OHOS
