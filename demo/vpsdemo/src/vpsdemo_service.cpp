#include "vpsdemo_service.h"

#include "memrpc/server/handler.h"
#include "vpsdemo_codec.h"
#include "vpsdemo_protocol.h"
#include "vpsdemo_types.h"
#include "virus_protection_service_log.h"

namespace vpsdemo {

void VpsDemoService::Initialize() {
    if (initialized_) {
        return;
    }
    initialized_ = true;
    HLOGI("VpsDemoService initialized");
}

bool VpsDemoService::initialized() const {
    return initialized_;
}

void VpsDemoService::RegisterHandlers(memrpc::RpcServer* server) {
    if (server == nullptr) {
        return;
    }

    // Init handler — calls Initialize() for compatibility with clients
    // that still send InitEngine RPC.
    server->RegisterHandler(
        static_cast<memrpc::Opcode>(DemoOpcode::DemoInit),
        [this](const memrpc::RpcServerCall&, memrpc::RpcServerReply* reply) {
            if (reply == nullptr) return;
            Initialize();
            InitReply result;
            result.code = 0;
            memrpc::EncodeMessage(result, &reply->payload);
            HLOGI("Init: ok");
        });

    // ScanFile handler.
    server->RegisterHandler(
        static_cast<memrpc::Opcode>(DemoOpcode::DemoScanFile),
        [this](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
            if (reply == nullptr) return;
            ScanFileRequest request;
            if (!memrpc::DecodeMessage<ScanFileRequest>(call.payload, &request)) {
                reply->status = memrpc::StatusCode::ProtocolMismatch;
                HLOGE("ScanFile: decode failed");
                return;
            }
            ScanFileReply result;
            if (!initialized_) {
                result.code = -1;
            } else {
                result.code = 0;
                result.threat_level =
                    request.file_path.find("virus") != std::string::npos ? 1 : 0;
            }
            memrpc::EncodeMessage(result, &reply->payload);
            HLOGI("ScanFile(%{public}s): threat=%{public}d",
                  request.file_path.c_str(), result.threat_level);
        });

    // UpdateFeatureLib handler.
    server->RegisterHandler(
        static_cast<memrpc::Opcode>(DemoOpcode::DemoUpdateFeatureLib),
        [this](const memrpc::RpcServerCall&, memrpc::RpcServerReply* reply) {
            if (reply == nullptr) return;
            UpdateFeatureLibReply result;
            result.code = initialized_ ? 0 : -1;
            memrpc::EncodeMessage(result, &reply->payload);
            HLOGI("UpdateFeatureLib: code=%{public}d", result.code);
        });
}

}  // namespace vpsdemo
