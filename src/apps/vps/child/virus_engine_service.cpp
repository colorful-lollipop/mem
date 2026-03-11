#include "apps/vps/child/virus_engine_service.h"

#include <array>
#include <utility>

#include "apps/vps/protocol.h"

namespace OHOS::Security::VirusProtectionService {

namespace {

constexpr std::array<std::pair<VirusEngine, const char*>, 5> kEngineLibraries = {{
    {VirusEngine::CSPL_STATIC_ENGINE, "libcspl_static_engine_utils.z.so"},
    {VirusEngine::CSPL_DYNAMIC_ENGINE, "libcspl_dynamic_engine_utils.z.so"},
    {VirusEngine::TRUSTONE_STATIC_ENGINE, "libtrustone_static_engine_utils.z.so"},
    {VirusEngine::QOWL_STATIC_ENGINE, "libqowl_static_engine_utils.z.so"},
    {VirusEngine::ATCORE_STATIC_ENGINE, "libatcore_static_engine_utils.z.so"},
}};

ThreatLevel MaxThreat(ThreatLevel left, ThreatLevel right) {
  return static_cast<int32_t>(left) >= static_cast<int32_t>(right) ? left : right;
}

}  // namespace

VirusEngineService::VirusEngineService() = default;

void VirusEngineService::RegisterSimpleHandler(memrpc::RpcServer* server, VpsOpcode opcode,
                                               SimpleMethod method) {
  server->RegisterHandler(static_cast<memrpc::Opcode>(opcode),
                          [this, method](const memrpc::RpcServerCall&, memrpc::RpcServerReply* reply) {
                            if (reply != nullptr) {
                              EncodeInt32Result((this->*method)(), &reply->payload);
                            }
                          });
}

void VirusEngineService::RegisterAccessTokenHandler(memrpc::RpcServer* server, VpsOpcode opcode,
                                                     AccessTokenMethod method) {
  server->RegisterHandler(
      static_cast<memrpc::Opcode>(opcode),
      [this, method](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
        if (reply == nullptr) {
          return;
        }
        AccessTokenRequest request;
        if (!DecodeAccessTokenRequest(call.payload, &request)) {
          reply->status = memrpc::StatusCode::ProtocolMismatch;
          return;
        }
        EncodeInt32Result((this->*method)(request.accessToken), &reply->payload);
      });
}

void VirusEngineService::RegisterHandlers(memrpc::RpcServer* server) {
  if (server == nullptr) {
    return;
  }
  RegisterSimpleHandler(server, VpsOpcode::VpsInit, &VirusEngineService::Init);
  RegisterSimpleHandler(server, VpsOpcode::VpsDeInit, &VirusEngineService::DeInit);
  RegisterSimpleHandler(server, VpsOpcode::VpsUpdateFeatureLib,
                        &VirusEngineService::UpdateFeatureLib);
  server->RegisterHandler(static_cast<memrpc::Opcode>(VpsOpcode::VpsScanFile),
                          [this](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                            if (reply == nullptr) {
                              return;
                            }
                            ScanTask task;
                            ScanResult result;
                            if (!DecodeScanTask(call.payload, &task)) {
                              reply->status = memrpc::StatusCode::ProtocolMismatch;
                              return;
                            }
                            EncodeScanFileReply(ScanFile(&task, &result), result, &reply->payload);
                          });
  server->RegisterHandler(static_cast<memrpc::Opcode>(VpsOpcode::VpsScanBehavior),
                          [this](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                            if (reply == nullptr) {
                              return;
                            }
                            ScanBehaviorRequest request;
                            if (!DecodeScanBehaviorRequest(call.payload, &request)) {
                              reply->status = memrpc::StatusCode::ProtocolMismatch;
                              return;
                            }
                            EncodeInt32Result(
                                ScanBehavior(request.accessToken, request.event, request.bundleName),
                                &reply->payload);
                          });
  RegisterAccessTokenHandler(server, VpsOpcode::VpsIsExistAnalysisEngine,
                             &VirusEngineService::IsExistAnalysisEngine);
  RegisterAccessTokenHandler(server, VpsOpcode::VpsCreateAnalysisEngine,
                             &VirusEngineService::CreateAnalysisEngine);
  RegisterAccessTokenHandler(server, VpsOpcode::VpsDestroyAnalysisEngine,
                             &VirusEngineService::DestroyAnalysisEngine);
}

int32_t VirusEngineService::Init() {
  std::unique_lock<std::shared_mutex> lock(engine_mutex_);
  if (initialized_) {
    return SUCCESS;
  }
  VirusEngineConfig config;
  for (const auto& [engine, lib] : kEngineLibraries) {
    auto loader = std::make_unique<LibLoader>(lib);
    if (loader->CreateVirusEngine(&config) == SUCCESS) {
      engineLoaders_[engine] = std::move(loader);
    }
  }
  initialized_ = !engineLoaders_.empty();
  return initialized_ ? SUCCESS : FAILED;
}

int32_t VirusEngineService::DeInit() {
  std::unique_lock<std::shared_mutex> lock(engine_mutex_);
  for (auto& [_, loader] : engineLoaders_) {
    if (loader != nullptr) {
      loader->DestroyVirusEngine();
    }
  }
  engineLoaders_.clear();
  {
    std::lock_guard<std::mutex> alock(analysis_mutex_);
    analysis_tokens_.clear();
  }
  {
    std::lock_guard<std::mutex> block(behavior_mutex_);
    while (!behavior_events_.empty()) {
      behavior_events_.pop();
    }
  }
  initialized_ = false;
  return SUCCESS;
}

int32_t VirusEngineService::UpdateFeatureLib() {
  return Init();
}

int32_t VirusEngineService::ScanFile(const ScanTask* scanTask, ScanResult* scanResult) {
  std::shared_lock<std::shared_mutex> lock(engine_mutex_);
  if (!initialized_ || scanTask == nullptr || scanResult == nullptr) {
    return FAILED;
  }

  scanResult->bundleName = scanTask->bundleName;
  scanResult->bundleInfo = scanTask->bundleInfo;
  scanResult->fileInfos = scanTask->fileInfos;
  scanResult->scanTaskType = scanTask->scanTaskType;
  scanResult->accountId = scanTask->accountId;
  scanResult->threatLevel = ThreatLevel::NO_RISK;
  scanResult->engineResults.assign(static_cast<size_t>(VirusEngine::COUNT), EngineResult{});

  for (auto& [engine, loader] : engineLoaders_) {
    if (loader == nullptr || loader->GetVirusEngine() == nullptr) {
      continue;
    }
    if (loader->GetVirusEngine()->ScanFile(scanTask, scanResult) != SUCCESS) {
      continue;
    }
    const size_t index = static_cast<size_t>(engine);
    if (index < scanResult->engineResults.size()) {
      scanResult->threatLevel =
          MaxThreat(scanResult->threatLevel, scanResult->engineResults[index].level);
    }
  }
  return SUCCESS;
}

int32_t VirusEngineService::ScanBehavior(uint32_t accessToken,
                                         const std::string& event,
                                         const std::string& bundleName) {
  std::shared_lock<std::shared_mutex> lock(engine_mutex_);
  auto it = engineLoaders_.find(VirusEngine::CSPL_DYNAMIC_ENGINE);
  if (!initialized_ || it == engineLoaders_.end() || it->second == nullptr ||
      it->second->GetVirusEngine() == nullptr) {
    return FAILED;
  }
  if (it->second->GetVirusEngine()->ScanBehavior(accessToken, event, bundleName) != SUCCESS) {
    return FAILED;
  }
  std::lock_guard<std::mutex> block(behavior_mutex_);
  if (behavior_reports_enabled_ && !event.empty()) {
    PendingBehaviorEvent pending;
    pending.accessToken = accessToken;
    pending.scanResult.eventId = "event-" + std::to_string(accessToken);
    pending.scanResult.time = "2026-03-09T00:00:00Z";
    pending.scanResult.ruleName =
        event.find("startup") != std::string::npos ? "startup_persist" : "behavior_detected";
    pending.scanResult.bundleName = bundleName;
    behavior_events_.push(std::move(pending));
  }
  return SUCCESS;
}

int32_t VirusEngineService::IsExistAnalysisEngine(uint32_t accessToken) {
  std::lock_guard<std::mutex> lock(analysis_mutex_);
  return analysis_tokens_.count(accessToken) == 0u ? FAILED : SUCCESS;
}

int32_t VirusEngineService::CreateAnalysisEngine(uint32_t accessToken) {
  std::lock_guard<std::mutex> lock(analysis_mutex_);
  analysis_tokens_.insert(accessToken);
  return SUCCESS;
}

int32_t VirusEngineService::DestroyAnalysisEngine(uint32_t accessToken) {
  std::lock_guard<std::mutex> lock(analysis_mutex_);
  analysis_tokens_.erase(accessToken);
  return SUCCESS;
}

void VirusEngineService::SetBehaviorReportEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(behavior_mutex_);
  behavior_reports_enabled_ = enabled;
}

PollBehaviorEventReply VirusEngineService::PollBehaviorEvent() {
  std::lock_guard<std::mutex> lock(behavior_mutex_);
  PollBehaviorEventReply reply;
  if (behavior_events_.empty()) {
    return reply;
  }
  reply.result = SUCCESS;
  reply.accessToken = behavior_events_.front().accessToken;
  reply.scanResult = std::move(behavior_events_.front().scanResult);
  behavior_events_.pop();
  return reply;
}

}  // namespace OHOS::Security::VirusProtectionService
