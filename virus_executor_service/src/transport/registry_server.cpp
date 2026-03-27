#include "transport/registry_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

#include <cerrno>

#include "transport/registry_protocol.h"
#include "virus_protection_executor_log.h"

namespace VirusExecutorService {

namespace {

bool IsReachableServiceSocket(const std::string& socketPath)
{
    if (socketPath.empty()) {
        HILOGE("IsReachableServiceSocket failed: empty socket path");
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        HILOGE("IsReachableServiceSocket socket failed: path=%{public}s errno=%{public}d", socketPath.c_str(), errno);
        return false;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    const bool connected = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
    close(fd);
    return connected;
}

}  // namespace

RegistryServer::RegistryServer(const std::string& socketPath)
    : socket_path_(socketPath)
{
}

RegistryServer::~RegistryServer()
{
    Stop();
}

bool RegistryServer::Start()
{
    unlink(socket_path_.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        HILOGE("RegistryServer::Start socket failed: path=%{public}s errno=%{public}d", socket_path_.c_str(), errno);
        return false;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        HILOGE("RegistryServer::Start bind failed: path=%{public}s errno=%{public}d", socket_path_.c_str(), errno);
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 8) < 0) {
        HILOGE("RegistryServer::Start listen failed: path=%{public}s errno=%{public}d", socket_path_.c_str(), errno);
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    const int listen_fd = listen_fd_;
    running_.store(true, std::memory_order_release);
    accept_thread_ = std::thread(&RegistryServer::AcceptLoop, this, listen_fd);
    return true;
}

void RegistryServer::Stop()
{
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

void RegistryServer::RegisterService(int32_t sa_id, const std::string& serviceSocketPath)
{
    std::lock_guard<std::mutex> lock(mutex_);
    services_[sa_id] = serviceSocketPath;
}

void RegistryServer::UnregisterService(int32_t sa_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    services_.erase(sa_id);
}

void RegistryServer::AcceptLoop(int listen_fd)
{
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

bool RegistryServer::DecodeClientRequest(int client_fd, RegistryRequest* req)
{
    std::string msg;
    if (!RecvMessage(client_fd, &msg)) {
        HILOGE("RegistryServer::HandleClient recv failed: client_fd=%{public}d", client_fd);
        return false;
    }

    if (!DecodeRegistryRequest(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), req)) {
        HILOGE("RegistryServer::HandleClient decode failed: client_fd=%{public}d msg_size=%{public}zu",
               client_fd,
               msg.size());
        return false;
    }
    return true;
}

bool RegistryServer::TryGetServicePath(int32_t sa_id, std::string* servicePath)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = services_.find(sa_id);
    if (it == services_.end()) {
        return false;
    }
    *servicePath = it->second;
    return true;
}

bool RegistryServer::RemoveStaleService(int32_t sa_id, const std::string& servicePath)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = services_.find(sa_id);
    if (it == services_.end() || it->second != servicePath) {
        return false;
    }
    services_.erase(it);
    return true;
}

bool RegistryServer::TryLoadService(int32_t sa_id)
{
    if (!load_cb_) {
        return false;
    }
    const bool loaded = load_cb_(sa_id);
    if (!loaded) {
        HILOGE("RegistryServer::HandleClient load callback failed: sa_id=%{public}d", sa_id);
    }
    return loaded;
}

bool RegistryServer::PopulateServiceResponse(int32_t sa_id, RegistryResponse* resp)
{
    std::string servicePath;
    if (!TryGetServicePath(sa_id, &servicePath)) {
        return false;
    }
    resp->err_code = 0;
    resp->payload = std::move(servicePath);
    return true;
}

RegistryResponse RegistryServer::HandleLoadRequest(const RegistryRequest& req)
{
    RegistryResponse resp;
    std::string servicePath;
    bool found = TryGetServicePath(req.sa_id, &servicePath);
    if (found && !IsReachableServiceSocket(servicePath)) {
        HILOGW("RegistryServer::HandleClient removing stale service: sa_id=%{public}d path=%{public}s",
               req.sa_id,
               servicePath.c_str());
        (void)RemoveStaleService(req.sa_id, servicePath);
        found = false;
    }
    if (!found) {
        found = TryLoadService(req.sa_id);
    }
    if (!found) {
        HILOGE("RegistryServer::HandleClient load failed: sa_id=%{public}d", req.sa_id);
        resp.err_code = -1;
        return resp;
    }
    if (!PopulateServiceResponse(req.sa_id, &resp)) {
        HILOGE("RegistryServer::HandleClient load succeeded but service missing: sa_id=%{public}d", req.sa_id);
        resp.err_code = -1;
    }
    return resp;
}

RegistryResponse RegistryServer::ProcessRequest(const RegistryRequest& req)
{
    RegistryResponse resp;
    switch (req.op) {
        case RegistryOp::Register: {
            std::lock_guard<std::mutex> lock(mutex_);
            services_[req.sa_id] = req.payload;
            resp.err_code = 0;
            break;
        }
        case RegistryOp::Get: {
            resp.err_code = PopulateServiceResponse(req.sa_id, &resp) ? 0 : -1;
            break;
        }
        case RegistryOp::Load:
            return HandleLoadRequest(req);
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
    return resp;
}

void RegistryServer::SendClientResponse(int client_fd, const RegistryRequest& req, const RegistryResponse& resp)
{
    std::string resp_msg;
    if (EncodeRegistryResponse(resp, &resp_msg)) {
        if (!SendMessage(client_fd, resp_msg)) {
            HILOGE("RegistryServer::HandleClient send failed: client_fd=%{public}d op=%{public}d sa_id=%{public}d",
                   client_fd,
                   static_cast<int>(req.op),
                   req.sa_id);
        }
    } else {
        HILOGE("RegistryServer::HandleClient encode response failed: op=%{public}d sa_id=%{public}d",
               static_cast<int>(req.op),
               req.sa_id);
    }
}

void RegistryServer::HandleClient(int client_fd)
{
    RegistryRequest req;
    if (!DecodeClientRequest(client_fd, &req)) {
        return;
    }
    SendClientResponse(client_fd, req, ProcessRequest(req));
}

}  // namespace VirusExecutorService
