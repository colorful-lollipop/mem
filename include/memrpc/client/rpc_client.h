#ifndef MEMRPC_CLIENT_RPC_CLIENT_H_
#define MEMRPC_CLIENT_RPC_CLIENT_H_

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
  uint32_t queue_timeout_ms = 1000;
  uint32_t exec_timeout_ms = 30000;
  uint32_t flags = 0;
  std::vector<uint8_t> payload;
};

struct RpcReply {
  StatusCode status = StatusCode::Ok;
  int32_t engine_code = 0;
  int32_t detail_code = 0;
  std::vector<uint8_t> payload;
};

class RpcFuture {
 public:
  RpcFuture();
  ~RpcFuture();

  RpcFuture(const RpcFuture&) = default;
  RpcFuture& operator=(const RpcFuture&) = default;
  RpcFuture(RpcFuture&&) noexcept = default;
  RpcFuture& operator=(RpcFuture&&) noexcept = default;

  bool IsReady() const;
  StatusCode Wait(RpcReply* reply);

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
  StatusCode Init();
  RpcFuture InvokeAsync(const RpcCall& call);
  StatusCode InvokeSync(const RpcCall& call, RpcReply* reply);
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
