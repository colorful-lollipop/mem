#include "apps/vps/parent/virus_engine_manager.h"

#include <algorithm>
#include <utility>

namespace OHOS::Security::VirusProtectionService {

namespace {

memrpc::RpcCall MakeCall(memrpc::Opcode opcode, std::vector<uint8_t> payload = {}) {
  memrpc::RpcCall call;
  call.opcode = opcode;
  call.payload = std::move(payload);
  return call;
}

}  // namespace

VirusEngineManager& VirusEngineManager::GetInstance() {
  static VirusEngineManager instance;
  return instance;
}

VirusEngineManager::VirusEngineManager() = default;

VirusEngineManager::~VirusEngineManager() {
  DeInit();
  if (client_ != nullptr) {
    client_->Shutdown();
  }
  if (server_ != nullptr) {
    server_->Stop();
  }
}

int32_t VirusEngineManager::EnsureRuntime() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (bootstrap_ != nullptr && client_ != nullptr && server_ != nullptr && service_ != nullptr) {
    return SUCCESS;
  }

  bootstrap_ = std::make_shared<memrpc::SaBootstrapChannel>();
  if (bootstrap_->StartEngine() != memrpc::StatusCode::Ok) {
    return FAILED;
  }

  service_ = std::make_unique<VirusEngineService>();
  server_ = std::make_unique<memrpc::RpcServer>(bootstrap_->server_handles());
  service_->RegisterHandlers(server_.get());
  if (server_->Start() != memrpc::StatusCode::Ok) {
    return FAILED;
  }

  client_ = std::make_unique<memrpc::RpcClient>(bootstrap_);
  if (client_->Init() != memrpc::StatusCode::Ok) {
    return FAILED;
  }
  return SUCCESS;
}

int32_t VirusEngineManager::InvokeInt32(memrpc::Opcode opcode,
                                        const std::vector<uint8_t>& requestBytes,
                                        int32_t* resultCode) {
  if (resultCode == nullptr || EnsureRuntime() != SUCCESS) {
    return FAILED;
  }
  memrpc::RpcReply reply;
  if (client_->InvokeSync(MakeCall(opcode, requestBytes), &reply) != memrpc::StatusCode::Ok ||
      !DecodeInt32Result(reply.payload, resultCode)) {
    return FAILED;
  }
  return *resultCode;
}

int32_t VirusEngineManager::InvokeScanFile(const ScanTask& task, ScanResult* result) {
  if (result == nullptr || EnsureRuntime() != SUCCESS) {
    return FAILED;
  }
  std::vector<uint8_t> requestBytes;
  if (!EncodeScanTask(task, &requestBytes)) {
    return FAILED;
  }

  memrpc::RpcReply reply;
  if (client_->InvokeSync(MakeCall(memrpc::Opcode::VpsScanFile, requestBytes), &reply) !=
      memrpc::StatusCode::Ok) {
    return FAILED;
  }

  int32_t resultCode = FAILED;
  if (!DecodeScanFileReply(reply.payload, &resultCode, result)) {
    return FAILED;
  }
  return resultCode;
}

int32_t VirusEngineManager::Init() {
  int32_t resultCode = FAILED;
  const int32_t result = InvokeInt32(memrpc::Opcode::VpsInit, {}, &resultCode);
  if (result == SUCCESS) {
    is_initialized_ = true;
  }
  return result;
}

void VirusEngineManager::DeInit() {
  int32_t resultCode = FAILED;
  if (client_ != nullptr) {
    InvokeInt32(memrpc::Opcode::VpsDeInit, {}, &resultCode);
  }
  is_initialized_ = false;
}

void VirusEngineManager::ScanFile(const ScanTask* scanTask, ScanResult* scanResult) {
  if (scanTask == nullptr || scanResult == nullptr) {
    return;
  }
  (void)InvokeScanFile(*scanTask, scanResult);
}

int32_t VirusEngineManager::ScanBehavior(uint32_t accessToken,
                                         const std::string& event,
                                         const std::string& bundleName) {
  ScanBehaviorRequest request;
  request.accessToken = accessToken;
  request.event = event;
  request.bundleName = bundleName;
  std::vector<uint8_t> requestBytes;
  int32_t resultCode = FAILED;
  return EncodeScanBehaviorRequest(request, &requestBytes)
             ? InvokeInt32(memrpc::Opcode::VpsScanBehavior, requestBytes, &resultCode)
             : FAILED;
}

int32_t VirusEngineManager::IsExistAnalysisEngine(uint32_t accessToken) {
  AccessTokenRequest request{accessToken};
  std::vector<uint8_t> requestBytes;
  int32_t resultCode = FAILED;
  return EncodeAccessTokenRequest(request, &requestBytes)
             ? InvokeInt32(memrpc::Opcode::VpsIsExistAnalysisEngine, requestBytes, &resultCode)
             : FAILED;
}

int32_t VirusEngineManager::CreateAnalysisEngine(uint32_t accessToken) {
  AccessTokenRequest request{accessToken};
  std::vector<uint8_t> requestBytes;
  int32_t resultCode = FAILED;
  return EncodeAccessTokenRequest(request, &requestBytes)
             ? InvokeInt32(memrpc::Opcode::VpsCreateAnalysisEngine, requestBytes, &resultCode)
             : FAILED;
}

int32_t VirusEngineManager::DestroyAnalysisEngine(uint32_t accessToken) {
  AccessTokenRequest request{accessToken};
  std::vector<uint8_t> requestBytes;
  int32_t resultCode = FAILED;
  return EncodeAccessTokenRequest(request, &requestBytes)
             ? InvokeInt32(memrpc::Opcode::VpsDestroyAnalysisEngine, requestBytes, &resultCode)
             : FAILED;
}

int32_t VirusEngineManager::RegisterScanResultListener(
    std::shared_ptr<ScanResultListener>& listener) {
  if (listener == nullptr) {
    return FAILED;
  }
  std::lock_guard<std::mutex> lock(listenerMutex_);
  listeners_.push_back(listener);
  reportsEnabled_ = true;
  return SUCCESS;
}

int32_t VirusEngineManager::UnRegisterScanResultListener(
    std::shared_ptr<ScanResultListener>& listener) {
  std::lock_guard<std::mutex> lock(listenerMutex_);
  listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
  reportsEnabled_ = !listeners_.empty();
  return SUCCESS;
}

int32_t VirusEngineManager::UpdateFeatureLib() {
  int32_t resultCode = FAILED;
  return InvokeInt32(memrpc::Opcode::VpsUpdateFeatureLib, {}, &resultCode);
}

}  // namespace OHOS::Security::VirusProtectionService
