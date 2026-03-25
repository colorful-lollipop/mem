#ifndef INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_SERVER_H_
#define INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_SERVER_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "transport/registry_protocol.h"

namespace VirusExecutorService {

using LoadCallback = std::function<bool(int32_t sa_id)>;
using UnloadCallback = std::function<void(int32_t sa_id)>;

class RegistryServer {
public:
    explicit RegistryServer(const std::string& socketPath);
    ~RegistryServer();

    void SetLoadCallback(LoadCallback cb)
    {
        load_cb_ = std::move(cb);
    }
    void SetUnloadCallback(UnloadCallback cb)
    {
        unload_cb_ = std::move(cb);
    }

    bool Start();
    void Stop();

    void RegisterService(int32_t sa_id, const std::string& serviceSocketPath);
    void UnregisterService(int32_t sa_id);

private:
    void AcceptLoop(int listen_fd);
    void HandleClient(int client_fd);
    bool DecodeClientRequest(int client_fd, RegistryRequest* req);
    RegistryResponse ProcessRequest(const RegistryRequest& req);
    RegistryResponse HandleLoadRequest(const RegistryRequest& req);
    bool TryGetServicePath(int32_t sa_id, std::string* servicePath);
    bool RemoveStaleService(int32_t sa_id, const std::string& servicePath);
    bool TryLoadService(int32_t sa_id);
    bool PopulateServiceResponse(int32_t sa_id, RegistryResponse* resp);
    void SendClientResponse(int client_fd, const RegistryRequest& req, const RegistryResponse& resp);

    std::string socket_path_;
    int listen_fd_ = -1;
    std::thread accept_thread_;
    std::atomic<bool> running_{false};

    std::mutex mutex_;
    std::unordered_map<int32_t, std::string> services_;

    LoadCallback load_cb_;
    UnloadCallback unload_cb_;
};

}  // namespace VirusExecutorService

#endif  // INCLUDE_VIRUS_EXECUTOR_SERVICE_TRANSPORT_REGISTRY_SERVER_H_
