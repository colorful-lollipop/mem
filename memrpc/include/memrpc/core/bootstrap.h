#ifndef MEMRPC_CORE_BOOTSTRAP_H_
#define MEMRPC_CORE_BOOTSTRAP_H_

#include <cstdint>
#include <functional>

#include "memrpc/core/types.h"

namespace MemRpc {

using EngineDeathCallback = std::function<void(uint64_t)>;

enum class ChannelHealthStatus : uint8_t {
  Healthy = 0,
  Timeout = 1,
  Malformed = 2,
  Unhealthy = 3,
  SessionMismatch = 4,
  Unsupported = 5,
};

struct ChannelHealthResult {
  ChannelHealthStatus status = ChannelHealthStatus::Unsupported;
  uint64_t sessionId = 0;
};

struct BootstrapHandles {
  // 这里后续会是生成的代码无法使用默认值
  // 这些 fd/元数据描述了一次共享内存 RPC session 的连接入口。
  int shmFd;
  int highReqEventFd;
  int normalReqEventFd;
  int respEventFd;
  int reqCreditEventFd;
  int respCreditEventFd;
  uint32_t protocolVersion;
  uint64_t sessionId;
};

[[nodiscard]] inline BootstrapHandles MakeDefaultBootstrapHandles() {
  BootstrapHandles handles{};
  handles.shmFd = -1;
  handles.highReqEventFd = -1;
  handles.normalReqEventFd = -1;
  handles.respEventFd = -1;
  handles.reqCreditEventFd = -1;
  handles.respCreditEventFd = -1;
  handles.protocolVersion = 0;
  handles.sessionId = 0;
  return handles;
}

class IBootstrapChannel {
 public:
  virtual ~IBootstrapChannel() = default;

  // 建立 session：若资源未创建则惰性创建，返回 dup 后的句柄集合。幂等。
  virtual StatusCode OpenSession(BootstrapHandles& handles) = 0;
  // 关闭 session：通知 bootstrap 层客户端断开。不销毁 server 侧资源。
  virtual StatusCode CloseSession() = 0;
  // 通用健康检查：bootstrap 可将业务协议翻译成通用健康结果。
  virtual ChannelHealthResult CheckHealth(uint64_t expectedSessionId) {
    (void)expectedSessionId;
    return {};
  }
  // 用于将”子进程死亡”从 bootstrap 层回传给 client。
  virtual void SetEngineDeathCallback(EngineDeathCallback callback) = 0;
};

}  // namespace MemRpc

namespace OHOS::Security::VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace OHOS::Security::VirusProtectionService

#endif  // MEMRPC_CORE_BOOTSTRAP_H_
