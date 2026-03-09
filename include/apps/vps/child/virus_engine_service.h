#ifndef APPS_VPS_CHILD_VIRUS_ENGINE_SERVICE_H_
#define APPS_VPS_CHILD_VIRUS_ENGINE_SERVICE_H_

#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "apps/vps/child/lib_loader.h"
#include "apps/vps/common/vps_codec.h"
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
  struct PendingBehaviorEvent {
    uint32_t accessToken = 0;
    BehaviorScanResult scanResult;
  };

  bool behavior_reports_enabled_ = false;
  bool initialized_ = false;
  std::mutex mutex_;
  std::unordered_map<VirusEngine, std::unique_ptr<LibLoader>> engineLoaders_;
  std::unordered_set<uint32_t> analysis_tokens_;
  std::queue<PendingBehaviorEvent> behavior_events_;
};

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_CHILD_VIRUS_ENGINE_SERVICE_H_
