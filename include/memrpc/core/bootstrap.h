#ifndef MEMRPC_CORE_BOOTSTRAP_H_
#define MEMRPC_CORE_BOOTSTRAP_H_

#include <cstdint>
#include <functional>

#include "memrpc/core/types.h"

namespace memrpc {

using EngineDeathCallback = std::function<void(uint64_t)>;

struct BootstrapHandles {
  // 这些 fd/元数据描述了一次共享内存 RPC session 的连接入口。
  int shmFd = -1;
  int high_req_event_fd = -1;
  int normal_req_event_fd = -1;
  int resp_event_fd = -1;
  int req_credit_event_fd = -1;
  int resp_credit_event_fd = -1;
  uint32_t protocol_version = 0;
  uint64_t session_id = 0;
};

class IBootstrapChannel {
 public:
  virtual ~IBootstrapChannel() = default;

  // 建立 session：若资源未创建则惰性创建，返回 dup 后的句柄集合。幂等。
  virtual StatusCode OpenSession(BootstrapHandles& handles) = 0;
  // 关闭 session：通知 bootstrap 层客户端断开。不销毁 server 侧资源。
  virtual StatusCode CloseSession() = 0;
  // 用于将”子进程死亡”从 bootstrap 层回传给 client。
  virtual void SetEngineDeathCallback(EngineDeathCallback callback) = 0;
};

}  // namespace memrpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::memrpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_CORE_BOOTSTRAP_H_
