#include "memrpc/client.h"

#include <memory>
#include <utility>

#include "memrpc/compat/scan_behavior_codec.h"
#include "memrpc/compat/scan_codec.h"

namespace memrpc {

struct EngineClient::Impl {
  explicit Impl(std::shared_ptr<IBootstrapChannel> bootstrap_channel)
      : client(std::move(bootstrap_channel)) {}

  RpcClient client;
};

EngineClient::EngineClient(std::shared_ptr<IBootstrapChannel> bootstrap)
    : impl_(std::make_unique<Impl>(std::move(bootstrap))) {}

EngineClient::~EngineClient() {
  Shutdown();
}

void EngineClient::SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
  impl_->client.SetBootstrapChannel(std::move(bootstrap));
}

StatusCode EngineClient::Init() {
  return impl_->client.Init();
}

StatusCode EngineClient::Scan(const ScanRequest& request, ScanResult* result) {
  if (result == nullptr) {
    return StatusCode::InvalidArgument;
  }
  if (request.file_path.empty() || request.file_path.size() >= kMaxFilePathSize) {
    return StatusCode::InvalidArgument;
  }
  std::vector<uint8_t> request_bytes;
  if (!EncodeScanRequest(request, &request_bytes)) {
    return StatusCode::InvalidArgument;
  }
  RpcCall call;
  call.opcode = Opcode::ScanFile;
  call.priority = request.options.priority;
  call.queue_timeout_ms = request.options.queue_timeout_ms;
  call.exec_timeout_ms = request.options.exec_timeout_ms;
  call.flags = request.options.flags;
  call.payload = std::move(request_bytes);
  RpcReply reply;
  const StatusCode status = impl_->client.InvokeSync(call, &reply);
  result->status = reply.status;
  result->engine_code = reply.engine_code;
  result->detail_code = reply.detail_code;
  if (!DecodeScanResult(reply.payload, result)) {
    result->status = status == StatusCode::Ok ? StatusCode::ProtocolMismatch : status;
    result->verdict = ScanVerdict::Error;
    result->message = "invalid response payload";
    return result->status;
  }
  if (status != StatusCode::Ok) {
    result->status = status;
  }
  return result->status;
}

StatusCode EngineClient::ScanBehavior(const ScanBehaviorRequest& request,
                                      ScanBehaviorResult* result) {
  if (result == nullptr || request.behavior_text.empty()) {
    return StatusCode::InvalidArgument;
  }

  std::vector<uint8_t> request_bytes;
  if (!EncodeScanBehaviorRequest(request, &request_bytes)) {
    return StatusCode::InvalidArgument;
  }

  RpcCall call;
  call.opcode = Opcode::ScanBehavior;
  call.priority = request.options.priority;
  call.queue_timeout_ms = request.options.queue_timeout_ms;
  call.exec_timeout_ms = request.options.exec_timeout_ms;
  call.flags = request.options.flags;
  call.payload = std::move(request_bytes);
  RpcReply reply;
  const StatusCode status = impl_->client.InvokeSync(call, &reply);
  result->status = reply.status;
  result->engine_code = reply.engine_code;
  result->detail_code = reply.detail_code;
  if (!DecodeScanBehaviorResult(reply.payload, result)) {
    result->status = status == StatusCode::Ok ? StatusCode::ProtocolMismatch : status;
    result->verdict = ScanVerdict::Error;
    result->message = "invalid response payload";
    return result->status;
  }
  if (status != StatusCode::Ok) {
    result->status = status;
  }
  return result->status;
}

void EngineClient::Shutdown() {
  impl_->client.Shutdown();
}

}  // namespace memrpc
