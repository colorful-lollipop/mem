#pragma once

#include "memrpc/core/protocol.h"
#include "memrpc/core/types.h"

namespace MemRpc {

inline ReplayHint ClassifyReplayHint(RpcRuntimeState state)
{
    switch (state) {
        case RpcRuntimeState::Admitted:
        case RpcRuntimeState::Queued:
            return ReplayHint::SafeToReplay;
        case RpcRuntimeState::Executing:
        case RpcRuntimeState::Responding:
        case RpcRuntimeState::Ready:
        case RpcRuntimeState::Consumed:
        case RpcRuntimeState::Free:
            return ReplayHint::MaybeExecuted;
        case RpcRuntimeState::Unknown:
            return ReplayHint::Unknown;
    }
    return ReplayHint::Unknown;
}

}  // namespace MemRpc
