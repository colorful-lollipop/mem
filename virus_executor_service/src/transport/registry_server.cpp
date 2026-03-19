#include "transport/registry_server.h"

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>

#include "transport/registry_protocol.h"
#include "virus_protection_service_log.h"

namespace VirusExecutorService {

namespace {

bool IsReachableServiceSocket(const std::string& socketPath) {
    if (socketPath.empty()) {
        HILOGE("IsReachableServiceSocket failed: empty socket path");
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        HILOGE("IsReachableServiceSocket socket failed: path=%{public}s errno=%{public}d",
            socketPath.c_str(), errno);
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    const bool connected =
        connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
    close(fd);
    return connected;
}

}  // namespace

RegistryServer::RegistryServer(const std::string& socketPath)
    : socket_path_(socketPath) {}

RegistryServer::~RegistryServer() {
    Stop();
}

bool RegistryServer::Start() {
    unlink(socket_path_.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        HILOGE("RegistryServer::Start socket failed: path=%{public}s errno=%{public}d",
            socket_path_.c_str(), errno);
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        HILOGE("RegistryServer::Start bind failed: path=%{public}s errno=%{public}d",
            socket_path_.c_str(), errno);
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 8) < 0) {
        HILOGE("RegistryServer::Start listen failed: path=%{public}s errno=%{public}d",
            socket_path_.c_str(), errno);
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    const int listen_fd = listen_fd_;
    running_.store(true, std::memory_order_release);
    accept_thread_ = std::thread(&RegistryServer::AcceptLoop, this, listen_fd);
    return true;
}

void RegistryServer::Stop() {
    running_.store(false, std::memory_order_release);
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    unlink(socket_path_.c_str());
}

void RegistryServer::RegisterService(int32_t sa_id, const std::string& serviceSocketPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    services_[sa_id] = serviceSocketPath;
}

void RegistryServer::UnregisterService(int32_t sa_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    services_.erase(sa_id);
}

void RegistryServer::AcceptLoop(int listen_fd) {
    while (running_.load(std::memory_order_acquire)) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (running_.load(std::memory_order_acquire)) {
                HILOGE("RegistryServer::AcceptLoop accept failed: errno=%{public}d", errno);
                continue;
            }
            break;
        }
        // Handle one request per connection (simple protocol).
        HandleClient(client_fd);
        close(client_fd);
    }
}

void RegistryServer::HandleClient(int client_fd) {
    std::string msg;
    if (!RecvMessage(client_fd, &msg)) {
        HILOGE("RegistryServer::HandleClient recv failed: client_fd=%{public}d", client_fd);
        return;
    }

    RegistryRequest req;
    if (!DecodeRegistryRequest(reinterpret_cast<const uint8_t*>(msg.data()),
                               msg.size(), &req)) {
        HILOGE("RegistryServer::HandleClient decode failed: client_fd=%{public}d msg_size=%{public}zu",
            client_fd, msg.size());
        return;
    }

    RegistryResponse resp;

    switch (req.op) {
        case RegistryOp::Register: {
            std::lock_guard<std::mutex> lock(mutex_);
            services_[req.sa_id] = req.payload;
            resp.err_code = 0;
            break;
        }
        case RegistryOp::Get: {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = services_.find(req.sa_id);
            if (it != services_.end()) {
                resp.err_code = 0;
                resp.payload = it->second;
            } else {
                resp.err_code = -1;
            }
            break;
        }
        case RegistryOp::Load: {
            bool found = false;
            std::string servicePath;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = services_.find(req.sa_id);
                if (it != services_.end()) {
                    servicePath = it->second;
                    found = true;
                }
            }
            if (found && !IsReachableServiceSocket(servicePath)) {
                HILOGW("RegistryServer::HandleClient removing stale service: sa_id=%{public}d path=%{public}s",
                    req.sa_id, servicePath.c_str());
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = services_.find(req.sa_id);
                if (it != services_.end() && it->second == servicePath) {
                    services_.erase(it);
                }
                found = false;
            }
            if (!found && load_cb_) {
                // Ask supervisor to start the SA.
                found = load_cb_(req.sa_id);
                if (!found) {
                    HILOGE("RegistryServer::HandleClient load callback failed: sa_id=%{public}d",
                        req.sa_id);
                }
            }
            if (found) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = services_.find(req.sa_id);
                if (it != services_.end()) {
                    resp.err_code = 0;
                    resp.payload = it->second;
                } else {
                    HILOGE("RegistryServer::HandleClient load succeeded but service missing: sa_id=%{public}d",
                        req.sa_id);
                    resp.err_code = -1;
                }
            } else {
                HILOGE("RegistryServer::HandleClient load failed: sa_id=%{public}d", req.sa_id);
                resp.err_code = -1;
            }
            break;
        }
        case RegistryOp::Unload: {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                services_.erase(req.sa_id);
            }
            if (unload_cb_) {
                unload_cb_(req.sa_id);
            }
            resp.err_code = 0;
            break;
        }
    }

    std::string resp_msg;
    if (EncodeRegistryResponse(resp, &resp_msg)) {
        if (!SendMessage(client_fd, resp_msg)) {
            HILOGE("RegistryServer::HandleClient send failed: client_fd=%{public}d op=%{public}d sa_id=%{public}d",
                client_fd, static_cast<int>(req.op), req.sa_id);
        }
    } else {
        HILOGE("RegistryServer::HandleClient encode response failed: op=%{public}d sa_id=%{public}d",
            static_cast<int>(req.op), req.sa_id);
    }
}

}  // namespace VirusExecutorService
