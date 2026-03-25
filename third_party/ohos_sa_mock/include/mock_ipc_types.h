#ifndef OHOS_SA_MOCK_MOCK_IPC_TYPES_H_
#define OHOS_SA_MOCK_MOCK_IPC_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace OHOS {

struct MockIpcRequest {
    std::vector<uint8_t> data;
};

struct MockIpcReply {
    static constexpr size_t MAX_FDS = 16;
    int fds[MAX_FDS] = {};
    size_t fd_count = 0;
    std::vector<uint8_t> data;
    bool close_after_reply = false;
};

// Handles a single client connection's framed command + payload, fills fds + data reply.
using MockIpcHandler = std::function<bool(int command, const MockIpcRequest& request, MockIpcReply* reply)>;

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_MOCK_IPC_TYPES_H_
