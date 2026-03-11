#ifndef OHOS_SA_MOCK_MOCK_IPC_TYPES_H_
#define OHOS_SA_MOCK_MOCK_IPC_TYPES_H_

#include <cstddef>
#include <functional>

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

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_MOCK_IPC_TYPES_H_
