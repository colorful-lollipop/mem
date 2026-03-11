#ifndef OHOS_SA_MOCK_MOCK_SERVICE_SOCKET_H_
#define OHOS_SA_MOCK_MOCK_SERVICE_SOCKET_H_

#include <atomic>
#include <string>
#include <thread>

#include "mock_ipc_types.h"

namespace OHOS {

// Encapsulates Unix domain socket listen + accept loop + SCM_RIGHTS reply.
// This is the mock-IPC transport that replaces OHOS binder for Linux dev.
class MockServiceSocket {
 public:
    MockServiceSocket() = default;
    ~MockServiceSocket();

    bool Start(const std::string& path, MockIpcHandler handler);
    void Stop();
    const std::string& socket_path() const { return socket_path_; }

 private:
    void AcceptLoop();

    MockIpcHandler handler_;
    std::string socket_path_;
    int listen_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_MOCK_SERVICE_SOCKET_H_
