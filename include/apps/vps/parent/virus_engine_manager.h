#ifndef APPS_VPS_PARENT_VIRUS_ENGINE_MANAGER_H_
#define APPS_VPS_PARENT_VIRUS_ENGINE_MANAGER_H_

#include <memory>
#include <mutex>
#include <vector>

#include "apps/vps/child/virus_engine_service.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/client/sa_bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace OHOS::Security::VirusProtectionService {

class VirusEngineManager {
 public:
  static VirusEngineManager& GetInstance();

  int32_t Init();
  void DeInit();
  void ScanFile(const ScanTask* scanTask, ScanResult* scanResult);
  int32_t ScanBehavior(uint32_t accessToken, const std::string& event, const std::string& bundleName);
  int32_t IsExistAnalysisEngine(uint32_t accessToken);
  int32_t CreateAnalysisEngine(uint32_t accessToken);
  int32_t DestroyAnalysisEngine(uint32_t accessToken);
  int32_t RegisterScanResultListener(std::shared_ptr<ScanResultListener>& listener);
  int32_t UnRegisterScanResultListener(std::shared_ptr<ScanResultListener>& listener);
  int32_t UpdateFeatureLib();
  ~VirusEngineManager();

 private:
  VirusEngineManager();
  VirusEngineManager(const VirusEngineManager&) = delete;
  VirusEngineManager& operator=(const VirusEngineManager&) = delete;

  int32_t EnsureRuntime();
  int32_t InvokeInt32(memrpc::Opcode opcode,
                      std::vector<uint8_t> requestBytes,
                      int32_t* resultCode);
  int32_t InvokeScanFile(const ScanTask& task, ScanResult* result);

  std::mutex mutex_;
  std::mutex listenerMutex_;
  std::vector<std::shared_ptr<ScanResultListener>> listeners_;
  bool is_initialized_ = false;
  bool reportsEnabled_ = false;

  std::shared_ptr<memrpc::SaBootstrapChannel> bootstrap_;
  std::unique_ptr<memrpc::RpcClient> client_;
  std::unique_ptr<memrpc::RpcServer> server_;
  std::unique_ptr<VirusEngineService> service_;
};

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_PARENT_VIRUS_ENGINE_MANAGER_H_
