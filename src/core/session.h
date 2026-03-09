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
  enum class SessionState : uint32_t {
    Alive = 0,
    Broken = 1,
  };

  struct RingAccess {
    RingCursor* cursor = nullptr;
    pthread_mutex_t* mutex = nullptr;
    void* entries = nullptr;
  };

  Session();
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  StatusCode Attach(const BootstrapHandles& handles);
  void Reset();

  bool valid() const;
  const SharedMemoryHeader* header() const;
  SharedMemoryHeader* mutable_header();
  const BootstrapHandles& handles() const;
  SessionState state() const;
  void SetState(SessionState state);
  SlotPayload* slot_payload(uint32_t slot_index);
  uint8_t* slot_request_bytes(uint32_t slot_index);

  StatusCode PushRequest(QueueKind queue, const RequestRingEntry& entry);
  bool PopRequest(QueueKind queue, RequestRingEntry* entry);
  StatusCode PushResponse(const ResponseRingEntry& entry);
  bool PopResponse(ResponseRingEntry* entry);

 private:
  RingAccess ResolveRing(QueueKind queue);
  std::size_t mapped_size_ = 0;
  void* mapped_region_ = nullptr;
  BootstrapHandles handles_{};
  SharedMemoryHeader* header_ = nullptr;
};

}  // namespace memrpc

#endif  // MEMRPC_CORE_SESSION_H_
