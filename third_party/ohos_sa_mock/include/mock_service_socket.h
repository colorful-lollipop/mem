#ifndef OHOS_SA_MOCK_MOCK_SERVICE_SOCKET_H_
#define OHOS_SA_MOCK_MOCK_SERVICE_SOCKET_H_

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>

namespace OHOS {

struct MockIpcReply {
    static constexpr size_t MAX_FDS = 16;
    int fds[MAX_FDS] = {};
    size_t fd_count = 0;
    char data[256] = {};
    size_t data_len = 0;
};

// Handles a single client connection's 1-byte command, fills fds + data reply.
using MockIpcHandler = std::function<bool(int command, MockIpcReply* reply)>;

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
