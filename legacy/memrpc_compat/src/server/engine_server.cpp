#include "memrpc/server.h"

#include <memory>

#include "memrpc/compat/scan_codec.h"

namespace memrpc {

struct EngineServer::Impl {
  RpcServer server;
  std::shared_ptr<IScanHandler> handler;
  ServerOptions options{};
  bool has_custom_scan_handler = false;

  void SyncScanHandler() {
    if (has_custom_scan_handler || handler == nullptr) {
      return;
    }
    server.RegisterHandler(Opcode::ScanFile, [this](const RpcServerCall& call, RpcServerReply* reply) {
      if (reply == nullptr) {
        return;
      }
      ScanRequest request;
      if (!DecodeScanRequest(call.payload, &request)) {
        reply->status = StatusCode::ProtocolMismatch;
        ScanResult error_result;
        error_result.status = reply->status;
        error_result.verdict = ScanVerdict::Error;
        error_result.message = "invalid request payload";
        EncodeScanResult(error_result, &reply->payload);
        return;
      }
      request.options.priority = call.priority;
      request.options.queue_timeout_ms = call.queue_timeout_ms;
      request.options.exec_timeout_ms = call.exec_timeout_ms;
      request.options.flags = call.flags;
      ScanResult result = handler->HandleScan(request);
      reply->status = result.status;
      reply->engine_code = result.engine_code;
      reply->detail_code = result.detail_code;
      if (!EncodeScanResult(result, &reply->payload)) {
        reply->status = StatusCode::EngineInternalError;
        ScanResult error_result;
        error_result.status = reply->status;
        error_result.verdict = ScanVerdict::Error;
        error_result.message = "encode response failed";
        EncodeScanResult(error_result, &reply->payload);
      }
    });
  }
};

EngineServer::EngineServer() : impl_(std::make_unique<Impl>()) {}

EngineServer::EngineServer(BootstrapHandles handles,
                           std::shared_ptr<IScanHandler> handler,
                           ServerOptions options)
    : impl_(std::make_unique<Impl>()) {
  impl_->server.SetBootstrapHandles(handles);
  impl_->handler = std::move(handler);
  impl_->options = options;
}

EngineServer::~EngineServer() {
  Stop();
}

void EngineServer::SetBootstrapHandles(BootstrapHandles handles) {
  impl_->server.SetBootstrapHandles(handles);
}

void EngineServer::SetScanHandler(std::shared_ptr<IScanHandler> handler) {
  impl_->handler = std::move(handler);
}

void EngineServer::RegisterHandler(Opcode opcode, RpcHandler handler) {
  if (opcode == Opcode::ScanFile) {
    impl_->has_custom_scan_handler = static_cast<bool>(handler);
  }
  impl_->server.RegisterHandler(opcode, std::move(handler));
}

void EngineServer::SetOptions(ServerOptions options) {
  impl_->options = options;
  impl_->server.SetOptions(options);
}

StatusCode EngineServer::Start() {
  impl_->server.SetOptions(impl_->options);
  impl_->SyncScanHandler();
  return impl_->server.Start();
}

void EngineServer::Run() {
  impl_->server.Run();
}

void EngineServer::Stop() {
  impl_->server.Stop();
}

}  // namespace memrpc
