#ifndef APPS_VPS_CHILD_VIRUS_ENGINE_SERVICE_H_
#define APPS_VPS_CHILD_VIRUS_ENGINE_SERVICE_H_

#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "apps/vps/child/lib_loader.h"
#include "apps/vps/common/vps_codec.h"
#include "apps/vps/protocol.h"
#include "memrpc/server/rpc_server.h"

namespace OHOS::Security::VirusProtectionService {

class VirusEngineService {
 public:
  VirusEngineService();

  void RegisterHandlers(memrpc::RpcServer* server);

  int32_t Init();
  int32_t DeInit();
  int32_t UpdateFeatureLib();
  int32_t ScanFile(const ScanTask* scanTask, ScanResult* scanResult);
  int32_t ScanBehavior(uint32_t accessToken, const std::string& event, const std::string& bundleName);
  int32_t IsExistAnalysisEngine(uint32_t accessToken);
  int32_t CreateAnalysisEngine(uint32_t accessToken);
  int32_t DestroyAnalysisEngine(uint32_t accessToken);
  void SetBehaviorReportEnabled(bool enabled);
  PollBehaviorEventReply PollBehaviorEvent();

 private:
  using SimpleMethod = int32_t (VirusEngineService::*)();
  using AccessTokenMethod = int32_t (VirusEngineService::*)(uint32_t);

  void RegisterSimpleHandler(memrpc::RpcServer* server, VpsOpcode opcode, SimpleMethod method);
  void RegisterAccessTokenHandler(memrpc::RpcServer* server, VpsOpcode opcode,
                                  AccessTokenMethod method);

  struct PendingBehaviorEvent {
    uint32_t accessToken = 0;
    BehaviorScanResult scanResult;
  };

  bool initialized_ = false;
  bool behavior_reports_enabled_ = false;
  std::shared_mutex engine_mutex_;       // Protects engineLoaders_ and initialized_.
  std::mutex behavior_mutex_;            // Protects behavior_events_ and behavior_reports_enabled_.
  std::mutex analysis_mutex_;            // Protects analysis_tokens_.
  std::unordered_map<VirusEngine, std::unique_ptr<LibLoader>> engineLoaders_;
  std::unordered_set<uint32_t> analysis_tokens_;
  std::queue<PendingBehaviorEvent> behavior_events_;
};

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_CHILD_VIRUS_ENGINE_SERVICE_H_
