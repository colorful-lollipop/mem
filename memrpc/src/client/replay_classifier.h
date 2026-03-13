#pragma once

#include "memrpc/core/protocol.h"
#include "memrpc/core/types.h"

namespace MemRpc {

inline RpcRuntimeState ToRpcRuntimeState(SlotRuntimeStateCode state) {
  switch (state) {
    case SlotRuntimeStateCode::Free: return RpcRuntimeState::Free;
    case SlotRuntimeStateCode::Admitted: return RpcRuntimeState::Admitted;
    case SlotRuntimeStateCode::Queued: return RpcRuntimeState::Queued;
    case SlotRuntimeStateCode::Executing: return RpcRuntimeState::Executing;
    case SlotRuntimeStateCode::Responding: return RpcRuntimeState::Responding;
    case SlotRuntimeStateCode::Ready: return RpcRuntimeState::Ready;
    case SlotRuntimeStateCode::Consumed: return RpcRuntimeState::Consumed;
  }
  return RpcRuntimeState::Unknown;
}

inline ReplayHint ClassifyReplayHint(SlotRuntimeStateCode state) {
  switch (state) {
    case SlotRuntimeStateCode::Admitted:
    case SlotRuntimeStateCode::Queued:
      return ReplayHint::SafeToReplay;
    case SlotRuntimeStateCode::Executing:
    case SlotRuntimeStateCode::Responding:
    case SlotRuntimeStateCode::Ready:
    case SlotRuntimeStateCode::Consumed:
    case SlotRuntimeStateCode::Free:
      return ReplayHint::MaybeExecuted;
  }
  return ReplayHint::Unknown;
}

}  // namespace MemRpc
