#include <gtest/gtest.h>

#include "apps/vps/parent/virus_engine_manager.h"

namespace OHOS::Security::VirusProtectionService {
namespace {

ScanTask BuildScanTask(const std::string& path) {
  ScanTask task;
  task.bundleName = "com.demo.app";
  task.scanTaskType = ScanTaskType::CUSTOM;
  task.accountId = 123;
  auto file = std::make_shared<BasicFileInfo>();
  file->filePath = path;
  file->fileHash = "hash";
  task.fileInfos.push_back(file);
  return task;
}

}  // namespace

TEST(VpsManagerIntegrationTest, SyncFacadeCoversCoreOperations) {
  auto& manager = VirusEngineManager::GetInstance();
  ASSERT_EQ(manager.Init(), SUCCESS);

  ScanTask task = BuildScanTask("/tmp/facade-virus.bin");
  ScanResult result;
  manager.ScanFile(&task, &result);
  EXPECT_EQ(result.bundleName, task.bundleName);
  EXPECT_EQ(result.threatLevel, ThreatLevel::HIGH_RISK);
  EXPECT_EQ(result.fileInfos.size(), task.fileInfos.size());

  EXPECT_EQ(manager.ScanBehavior(88u, "startup event", "com.demo.app"), SUCCESS);
  EXPECT_EQ(manager.IsExistAnalysisEngine(88u), FAILED);
  EXPECT_EQ(manager.CreateAnalysisEngine(88u), SUCCESS);
  EXPECT_EQ(manager.IsExistAnalysisEngine(88u), SUCCESS);
  EXPECT_EQ(manager.DestroyAnalysisEngine(88u), SUCCESS);
  EXPECT_EQ(manager.IsExistAnalysisEngine(88u), FAILED);
  EXPECT_EQ(manager.UpdateFeatureLib(), SUCCESS);

  manager.DeInit();
}

}  // namespace OHOS::Security::VirusProtectionService
