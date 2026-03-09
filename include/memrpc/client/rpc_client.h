#ifndef MEMRPC_CLIENT_RPC_CLIENT_H_
#define MEMRPC_CLIENT_RPC_CLIENT_H_

#include <chrono>
#include <memory>
#include <functional>
#include <vector>

#include "core/protocol.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"

namespace memrpc {

struct RpcCall {
  Opcode opcode = Opcode::ScanFile;
  Priority priority = Priority::Normal;
  // admission_timeout_ms 作用在 client 等待 slot / request ring 可写阶段。
  uint32_t admission_timeout_ms = 1000;
  // queue_timeout_ms 作用在服务端排队阶段。
  uint32_t queue_timeout_ms = 1000;
  // exec_timeout_ms 作用在服务端 handler 执行阶段；当前为软超时，不强杀 handler。
  uint32_t exec_timeout_ms = 30000;
  uint32_t flags = 0;
  std::vector<uint8_t> payload;
};

struct RpcReply {
  // status 是框架层结果；engine_code/detail_code 由业务 handler 自行定义。
  StatusCode status = StatusCode::Ok;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  std::vector<uint8_t> payload;
};

struct RpcClientRuntimeStats {
  uint32_t queued_submissions = 0;
  uint32_t pending_calls = 0;
  uint32_t request_slot_capacity = 0;
  uint32_t high_request_ring_pending = 0;
  uint32_t normal_request_ring_pending = 0;
  uint32_t response_ring_pending = 0;
  bool waiting_for_request_credit = false;
};

class RpcFuture {
 public:
  RpcFuture();
  ~RpcFuture();

  RpcFuture(const RpcFuture&) = default;
  RpcFuture& operator=(const RpcFuture&) = default;
  RpcFuture(RpcFuture&&) noexcept = default;
  RpcFuture& operator=(RpcFuture&&) noexcept = default;

  // IsReady 只表示 reply 已经落地，不区分成功或失败。
  bool IsReady() const;
  // Wait 会阻塞到 reply 就绪，并返回 reply.status。
  StatusCode Wait(RpcReply* reply);
  // WaitFor 在给定 deadline 内等待 reply；仅用于同步兼容路径。
  StatusCode WaitFor(RpcReply* reply, std::chrono::milliseconds timeout);

 private:
  struct State;
  explicit RpcFuture(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;

  friend class RpcClient;
};

class RpcClient {
 public:
  explicit RpcClient(std::shared_ptr<IBootstrapChannel> bootstrap = nullptr);
  ~RpcClient();

  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;
  RpcClient(RpcClient&&) noexcept = default;
  RpcClient& operator=(RpcClient&&) noexcept = default;

  void SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap);
  void SetEventCallback(RpcEventCallback callback);
  // Init 负责建立 session、映射共享内存并启动响应分发线程。
  StatusCode Init();
  // InvokeAsync 是框架层一等接口；失败时返回 ready future。
  RpcFuture InvokeAsync(const RpcCall& call);
  // InvokeSync 只是 InvokeAsync + deadline wait 的兼容包装，不做 1ms busy wait。
  StatusCode InvokeSync(const RpcCall& call, RpcReply* reply);
  RpcClientRuntimeStats GetRuntimeStats() const;
  void Shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace memrpc

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::memrpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // MEMRPC_CLIENT_RPC_CLIENT_H_
