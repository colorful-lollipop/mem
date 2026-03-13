#ifndef MEMRPC_CORE_SESSION_H_
#define MEMRPC_CORE_SESSION_H_

#include <cstdint>

#include "memrpc/core/protocol.h"
#include "core/shm_layout.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"

namespace MemRpc {

enum class QueueKind : uint8_t {
  HighRequest,
  NormalRequest,
  Response,
};

class Session {
 public:
  enum class AttachRole : uint8_t {
    Client,
    Server,
  };

  enum class SessionState : uint32_t {  // NOLINT(performance-enum-size)
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

  [[nodiscard]] bool Valid() const;
  [[nodiscard]] const SharedMemoryHeader* Header() const;
  SharedMemoryHeader* mutableHeader();
  [[nodiscard]] const BootstrapHandles& Handles() const;
  [[nodiscard]] SessionState State() const;
  void SetState(SessionState state);
  SlotPayload* GetSlotPayload(uint32_t slot_index);
  uint8_t* GetSlotRequestBytes(uint32_t slot_index);
  ResponseSlotPayload* GetResponseSlotPayload(uint32_t slot_index);
  uint8_t* GetResponseSlotBytes(uint32_t slot_index);
  void* GetResponseSlotPoolRegion();

  // Push/Pop 接口封装 ring + eventfd 对应的共享内存协议细节。
  StatusCode PushRequest(QueueKind queue, const RequestRingEntry& entry);
  bool PopRequest(QueueKind queue, RequestRingEntry* entry);
  StatusCode PushResponse(const ResponseRingEntry& entry);
  bool PopResponse(ResponseRingEntry* entry);

 private:
  // 根据 queue 类型解析出实际 ring 的 cursor/mutex/entries 指针。
  RingAccess ResolveRing(QueueKind queue);
  StatusCode MapAndValidateHeader(int shmFd);
  StatusCode RemapWithActualLayout(int shmFd);
  StatusCode TryAcquireClientSlot();
  std::size_t mappedSize_ = 0;
  std::size_t initialMappedSize_ = 0;
  void* mappedRegion_ = nullptr;
  BootstrapHandles handles_{};
  SharedMemoryHeader* header_ = nullptr;
  AttachRole attachRole_ = AttachRole::Server;
  bool ownsClientSlot_ = false;
};

}  // namespace MemRpc

#endif  // MEMRPC_CORE_SESSION_H_
