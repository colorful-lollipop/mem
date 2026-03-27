#ifndef MEMRPC_CORE_TYPES_H_
#define MEMRPC_CORE_TYPES_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MemRpc {

enum class Priority : uint8_t {
    Normal = 0,
    High = 1,
};

enum class StatusCode : uint8_t {
    Ok = 0,
    QueueFull,
    QueueTimeout,
    ExecTimeout,
    PeerDisconnected,
    ProtocolMismatch,
    EngineInternalError,
    InvalidArgument,
    CrashedDuringExecution,
    CooldownActive,
    ClientClosed,
    PayloadTooLarge,
};

struct RpcEvent {
    // event_domain/event_type 用于应用层自行划分事件命名空间。
    uint32_t eventDomain = 0;
    uint32_t eventType = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> payload;
};

using RpcEventCallback = std::function<void(const RpcEvent&)>;

}  // namespace MemRpc

#endif  // MEMRPC_CORE_TYPES_H_
