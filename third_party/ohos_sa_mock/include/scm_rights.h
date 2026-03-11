#ifndef OHOS_SA_MOCK_SCM_RIGHTS_H_
#define OHOS_SA_MOCK_SCM_RIGHTS_H_

#include <cstddef>

namespace OHOS {

// Send fds over a Unix domain socket using SCM_RIGHTS.
// Also sends 'data_len' bytes of 'data' as the message payload.
bool SendFds(int sock_fd, const int* fds, size_t fd_count,
             const void* data, size_t data_len);

// Receive fds. Returns number of fds received (0 on error).
// 'data' receives the message payload, 'data_len' is in/out.
size_t RecvFds(int sock_fd, int* fds, size_t max_fds,
               void* data, size_t* data_len);

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_SCM_RIGHTS_H_
