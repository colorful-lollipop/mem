#ifndef MEMRPC_CORE_SESSION_H_
#define MEMRPC_CORE_SESSION_H_

#include <cstdint>

#include "core/protocol.h"
#include "core/shm_layout.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"

namespace memrpc {

enum class QueueKind {
  HighRequest,
  NormalRequest,
  Response,
};

class Session {
 public:
  enum class AttachRole {
    Client,
    Server,
  };

  enum class SessionState : uint32_t {
    Alive = 0,
    Broken = 1,
  };

  struct RingAccess {
    // RingAccess 把某一条 ring 的 cursor/entries 指针打包返回。
    RingCursor* cursor = nullptr;
    void* entries = nullptr;
  };

  Session();
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  StatusCode Attach(const BootstrapHandles& handles, AttachRole role = AttachRole::Client);
  void Reset();

  bool valid() const;
  const SharedMemoryHeader* header() const;
  SharedMemoryHeader* mutable_header();
  const BootstrapHandles& handles() const;
  SessionState state() const;
  void SetState(SessionState state);
  SlotPayload* slot_payload(uint32_t slot_index);
  uint8_t* slot_request_bytes(uint32_t slot_index);
  ResponseSlotPayload* response_slot_payload(uint32_t slot_index);
  uint8_t* response_slot_bytes(uint32_t slot_index);
  void* response_slot_pool_region();

  // Push/Pop 接口封装 ring + eventfd 对应的共享内存协议细节。
  StatusCode PushRequest(QueueKind queue, const RequestRingEntry& entry);
  bool PopRequest(QueueKind queue, RequestRingEntry* entry);
  StatusCode PushResponse(const ResponseRingEntry& entry);
  bool PopResponse(ResponseRingEntry* entry);

 private:
  // 根据 queue 类型解析出实际 ring 的 cursor/mutex/entries 指针。
  RingAccess ResolveRing(QueueKind queue);
  std::size_t mapped_size_ = 0;
  void* mapped_region_ = nullptr;
  BootstrapHandles handles_{};
  SharedMemoryHeader* header_ = nullptr;
  AttachRole attach_role_ = AttachRole::Server;
  bool owns_client_slot_ = false;
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_SESSION_H_
