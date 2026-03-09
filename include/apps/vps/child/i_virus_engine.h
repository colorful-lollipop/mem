#ifndef APPS_VPS_CHILD_I_VIRUS_ENGINE_H_
#define APPS_VPS_CHILD_I_VIRUS_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>

#include "apps/vps/common/virus_protection_service_define.h"

namespace OHOS::Security::VirusProtectionService {

struct VirusEngineConfig {
  bool allowVirusClear = true;
};

class IVirusEngine {
 public:
  virtual ~IVirusEngine() = default;

  virtual int32_t Init(const VirusEngineConfig* config = nullptr) = 0;
  virtual int32_t DeInit() = 0;
  virtual int32_t ScanFile(const ScanTask* scanTask, ScanResult* scanResult) = 0;
  virtual int32_t ScanBehavior(uint32_t accessToken,
                               const std::string& event,
                               const std::string& bundleName) = 0;
};

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_CHILD_I_VIRUS_ENGINE_H_
