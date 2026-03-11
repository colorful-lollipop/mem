#include "vpsdemo_service.h"

#include "memrpc/server/typed_handler.h"
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

ScanFileReply VpsDemoService::ScanFile(const ScanFileRequest& request) {
    ScanFileReply result;
    if (!initialized_) {
        result.code = -1;
    } else {
        result.code = 0;
        result.threat_level =
            request.file_path.find("virus") != std::string::npos ? 1 : 0;
    }
    HLOGI("ScanFile(%{public}s): threat=%{public}d",
          request.file_path.c_str(), result.threat_level);
    return result;
}

void VpsDemoService::RegisterHandlers(memrpc::RpcServer* server) {
    if (server == nullptr) {
        return;
    }

    memrpc::RegisterTypedHandler<ScanFileRequest, ScanFileReply>(
        server, static_cast<memrpc::Opcode>(DemoOpcode::DemoScanFile),
        [this](const ScanFileRequest& r) { return ScanFile(r); });
}

}  // namespace vpsdemo
