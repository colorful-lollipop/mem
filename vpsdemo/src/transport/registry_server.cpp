#include "vpsdemo/transport/registry_server.h"

#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vpsdemo/transport/registry_protocol.h"

namespace vpsdemo {

RegistryServer::RegistryServer(const std::string& socketPath)
    : socket_path_(socketPath) {}

RegistryServer::~RegistryServer() {
    Stop();
}

bool RegistryServer::Start() {
    unlink(socket_path_.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 8) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&RegistryServer::AcceptLoop, this);
    return true;
}

void RegistryServer::Stop() {
    running_ = false;
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

void RegistryServer::AcceptLoop() {
    while (running_) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
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
        return;
    }

    RegistryRequest req;
    if (!DecodeRegistryRequest(reinterpret_cast<const uint8_t*>(msg.data()),
                               msg.size(), &req)) {
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
            {
                std::lock_guard<std::mutex> lock(mutex_);
                found = services_.count(req.sa_id) > 0;
            }
            if (!found && load_cb_) {
                // Ask supervisor to start the SA.
                found = load_cb_(req.sa_id);
            }
            if (found) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = services_.find(req.sa_id);
                if (it != services_.end()) {
                    resp.err_code = 0;
                    resp.payload = it->second;
                } else {
                    resp.err_code = -1;
                }
            } else {
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
        SendMessage(client_fd, resp_msg);
    }
}

}  // namespace vpsdemo
