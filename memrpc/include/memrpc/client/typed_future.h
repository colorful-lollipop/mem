#ifndef MEMRPC_CLIENT_TYPED_FUTURE_H_
#define MEMRPC_CLIENT_TYPED_FUTURE_H_

#include <chrono>
#include <utility>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/codec.h"

namespace MemRpc {

// TypedFuture<Rep> wraps an RpcFuture and performs owning decode at consumption
// time (Wait). The raw payload stays undecoded until the caller
// consumes the future, keeping dispatcher threads free of decode CPU.
template <typename Rep>
class TypedFuture {
public:
    TypedFuture() = default;
    explicit TypedFuture(RpcFuture future)
        : future_(std::move(future))
    {
    }

    TypedFuture(const TypedFuture&) = delete;
    TypedFuture& operator=(const TypedFuture&) = delete;
    TypedFuture(TypedFuture&&) noexcept = default;
    TypedFuture& operator=(TypedFuture&&) noexcept = default;

    [[nodiscard]] bool IsReady() const
    {
        return future_.IsReady();
    }

    // Wait blocks until the reply is ready, consumes the future, and decodes
    // the payload into *reply. Returns ProtocolMismatch if decode fails.
    StatusCode Wait(Rep* reply) &&
    {
        if (reply == nullptr) {
            return StatusCode::InvalidArgument;
        }
        RpcReply rpcReply;
        const StatusCode status = std::move(future_).Wait(&rpcReply);
        if (status != StatusCode::Ok) {
            return status;
        }
        return DecodeMessage<Rep>(rpcReply.payload, reply) ? rpcReply.status : StatusCode::ProtocolMismatch;
    }
    StatusCode Wait(Rep* reply) & = delete;
    StatusCode Wait(Rep* reply) const& = delete;

    // Access the underlying RpcFuture for low-level use.
    RpcFuture&& RawFuture() &&
    {
        return std::move(future_);
    }

private:
    RpcFuture future_;
};

}  // namespace MemRpc

#endif  // MEMRPC_CLIENT_TYPED_FUTURE_H_
