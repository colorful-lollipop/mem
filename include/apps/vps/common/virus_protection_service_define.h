#ifndef APPS_VPS_COMMON_VIRUS_PROTECTION_SERVICE_DEFINE_H_
#define APPS_VPS_COMMON_VIRUS_PROTECTION_SERVICE_DEFINE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace OHOS::Security::VirusProtectionService {

enum ErrorCode : int32_t {
  SUCCESS = 0,
  FAILED = 1,
  NOT_FOUND = 6,
  INVALID_OPERATION = 16,
  INVALID_INPUT = 18,
  SCAN_ENGINE_INTERNAL_ERR = 50001,
};

enum class ScanTaskType : int32_t {
  NONE = 0,
  DEEP = 1,
  QUICK = 2,
  CUSTOM = 3,
  IDLE = 4,
  REAL_TIME = 5,
  EVENT_DRIVEN = 6,
};

enum class VirusEngine : int32_t {
  CSPL_STATIC_ENGINE = 0,
  CSPL_DYNAMIC_ENGINE = 1,
  TRUSTONE_STATIC_ENGINE = 2,
  QOWL_STATIC_ENGINE = 3,
  ATCORE_STATIC_ENGINE = 4,
  COUNT = 5,
};

enum class ThreatLevel : int32_t {
  NONE = 0,
  NO_RISK = 1,
  LOW_RISK = 2,
  MEDIUM_RISK = 3,
  HIGH_RISK = 4,
  CLEAR_RISK = 5,
  CRASH_RISK = 6,
};

struct BasicFileInfo {
  std::string filePath;
  std::string fileHash;
  std::string subFilePath;
  std::string subFileHash;
  int64_t inode = 0;
  int64_t mtime = 0;
  uint64_t fileSize = 0;
};

struct BundleInfo {
  std::string bundleName;
  std::string appDistributionType;
  std::string versionName;
  std::string label;
  bool isEncrypted = false;
};

struct ScanTask {
  std::string bundleName;
  std::shared_ptr<BundleInfo> bundleInfo;
  std::vector<std::shared_ptr<BasicFileInfo>> fileInfos;
  ScanTaskType scanTaskType = ScanTaskType::NONE;
  int32_t accountId = 100;
};

struct EngineResult {
  std::string virusName;
  std::string virusType;
  std::string errorMsg;
  int32_t errorCode = SUCCESS;
  ThreatLevel level = ThreatLevel::NONE;
};

struct ScanResult {
  std::string bundleName;
  std::shared_ptr<BundleInfo> bundleInfo;
  std::vector<std::shared_ptr<BasicFileInfo>> fileInfos;
  std::string bakPath;
  ThreatLevel threatLevel = ThreatLevel::NONE;
  ScanTaskType scanTaskType = ScanTaskType::NONE;
  int32_t accountId = 100;
  std::vector<EngineResult> engineResults{
      std::vector<EngineResult>(static_cast<int32_t>(VirusEngine::COUNT))};
};

struct BehaviorScanResult {
  std::string eventId;
  std::string time;
  std::string ruleName;
  std::string bundleName;
};

class ScanResultListener {
 public:
  virtual ~ScanResultListener() = default;
  virtual void OnReport(uint32_t accessToken, BehaviorScanResult& scanResult) {}
};

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_COMMON_VIRUS_PROTECTION_SERVICE_DEFINE_H_
