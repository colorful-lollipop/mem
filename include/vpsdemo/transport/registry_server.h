#ifndef INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_SERVER_H_
#define INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_SERVER_H_

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace vpsdemo {

using LoadCallback = std::function<bool(int32_t sa_id)>;
using UnloadCallback = std::function<void(int32_t sa_id)>;

class RegistryServer {
 public:
    explicit RegistryServer(const std::string& socketPath);
    ~RegistryServer();

    void SetLoadCallback(LoadCallback cb) { load_cb_ = std::move(cb); }
    void SetUnloadCallback(UnloadCallback cb) { unload_cb_ = std::move(cb); }

    bool Start();
    void Stop();

    void RegisterService(int32_t sa_id, const std::string& serviceSocketPath);
    void UnregisterService(int32_t sa_id);

 private:
    void AcceptLoop();
    void HandleClient(int client_fd);

    std::string socket_path_;
    int listen_fd_ = -1;
    std::thread accept_thread_;
    bool running_ = false;

    std::mutex mutex_;
    std::unordered_map<int32_t, std::string> services_;

    LoadCallback load_cb_;
    UnloadCallback unload_cb_;
};

}  // namespace vpsdemo

#endif  // INCLUDE_VPSDEMO_TRANSPORT_REGISTRY_SERVER_H_
