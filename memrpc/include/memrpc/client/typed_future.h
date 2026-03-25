#ifndef MEMRPC_CLIENT_TYPED_FUTURE_H_
#define MEMRPC_CLIENT_TYPED_FUTURE_H_

#include <chrono>
#include <functional>
#include <utility>

#include "memrpc/client/rpc_client.h"
#include "memrpc/core/codec.h"

namespace MemRpc {

// TypedFuture<Rep> wraps an RpcFuture and performs owning decode at consumption
// time (Wait/WaitFor/Then). The raw payload stays undecoded until the caller
// consumes the future, keeping dispatcher threads free of decode CPU.
template <typename Rep>
class TypedFuture {
public:
    TypedFuture() = default;
    explicit TypedFuture(RpcFuture future)
        : future_(std::move(future))
    {
    }

    TypedFuture(const TypedFuture&) = default;
    TypedFuture& operator=(const TypedFuture&) = default;
    TypedFuture(TypedFuture&&) noexcept = default;
    TypedFuture& operator=(TypedFuture&&) noexcept = default;

    [[nodiscard]] bool IsReady() const
    {
        return future_.IsReady();
    }

    // Wait blocks until the reply is ready, decodes the payload into *reply, and
    // returns the status. Returns ProtocolMismatch if decode fails.
    StatusCode Wait(Rep* reply)
    {
        if (reply == nullptr) {
            return StatusCode::InvalidArgument;
        }
        RpcReply rpcReply;
        const StatusCode status = future_.WaitAndTake(&rpcReply);
        if (status != StatusCode::Ok) {
            return status;
        }
        return DecodeMessage<Rep>(rpcReply.payload, reply) ? rpcReply.status : StatusCode::ProtocolMismatch;
    }

    // WaitFor blocks up to |timeout|, then decodes. Returns QueueTimeout on
    // deadline expiry.
    StatusCode WaitFor(Rep* reply, std::chrono::milliseconds timeout)
    {
        if (reply == nullptr) {
            return StatusCode::InvalidArgument;
        }
        RpcReply rpcReply;
        const StatusCode status = future_.WaitFor(&rpcReply, timeout);
        if (status != StatusCode::Ok) {
            return status;
        }
        return DecodeMessage<Rep>(rpcReply.payload, reply) ? rpcReply.status : StatusCode::ProtocolMismatch;
    }

    // Then registers a completion callback. Decode happens inside the callback
    // wrapper. Mutually exclusive with Wait/WaitFor on the same future.
    void Then(std::function<void(StatusCode, Rep)> callback, const RpcThenExecutor& executor = {})
    {
        future_.Then(
            [cb = std::move(callback)](RpcReply rpcReply) {
                if (rpcReply.status != StatusCode::Ok) {
                    cb(rpcReply.status, {});
                    return;
                }
                Rep decoded{};
                if (!DecodeMessage<Rep>(rpcReply.payload, &decoded)) {
                    cb(StatusCode::ProtocolMismatch, {});
                    return;
                }
                cb(rpcReply.status, std::move(decoded));
            },
            executor);
    }

    // Access the underlying RpcFuture for low-level use.
    RpcFuture& RawFuture()
    {
        return future_;
    }
    [[nodiscard]] const RpcFuture& RawFuture() const
    {
        return future_;
    }

private:
    RpcFuture future_;
};

}  // namespace MemRpc

#endif  // MEMRPC_CLIENT_TYPED_FUTURE_H_
