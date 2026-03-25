#include "scm_rights.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <vector>

namespace OHOS {

bool SendFds(int sock_fd, const int* fds, size_t fd_count, const void* data, size_t data_len)
{
    if (fd_count == 0 || fds == nullptr) {
        return false;
    }

    // Ensure at least 1 byte of data for ancillary messages.
    char dummy = 0;
    struct iovec iov {};
    if (data != nullptr && data_len > 0) {
        iov.iov_base = const_cast<void*>(data);
        iov.iov_len = data_len;
    } else {
        iov.iov_base = &dummy;
        iov.iov_len = 1;
    }

    const size_t cmsg_space = CMSG_SPACE(fd_count * sizeof(int));
    std::vector<char> buf(cmsg_space, 0);

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf.data();
    msg.msg_controllen = cmsg_space;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fd_count * sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), fds, fd_count * sizeof(int));

    return sendmsg(sock_fd, &msg, MSG_NOSIGNAL) >= 0;
}

size_t RecvFds(int sock_fd, int* fds, size_t max_fds, void* data, size_t* data_len)
{
    if (fds == nullptr || max_fds == 0) {
        return 0;
    }

    char dummy = 0;
    struct iovec iov {};
    if (data != nullptr && data_len != nullptr && *data_len > 0) {
        iov.iov_base = data;
        iov.iov_len = *data_len;
    } else {
        iov.iov_base = &dummy;
        iov.iov_len = 1;
    }

    const size_t cmsg_space = CMSG_SPACE(max_fds * sizeof(int));
    std::vector<char> buf(cmsg_space, 0);

    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf.data();
    msg.msg_controllen = cmsg_space;

    ssize_t n = recvmsg(sock_fd, &msg, 0);
    if (n <= 0) {
        return 0;
    }
    if (data_len != nullptr) {
        *data_len = static_cast<size_t>(n);
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        return 0;
    }

    size_t fd_bytes = cmsg->cmsg_len - CMSG_LEN(0);
    size_t count = fd_bytes / sizeof(int);
    if (count > max_fds) {
        count = max_fds;
    }
    std::memcpy(fds, CMSG_DATA(cmsg), count * sizeof(int));
    return count;
}

}  // namespace OHOS
