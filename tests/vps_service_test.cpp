#include <gtest/gtest.h>

#include "apps/vps/child/virus_engine_service.h"

namespace OHOS::Security::VirusProtectionService {
namespace {

ScanTask BuildScanTask(const std::string& path) {
  ScanTask task;
  task.bundleName = "com.demo.app";
  task.scanTaskType = ScanTaskType::CUSTOM;
  auto file = std::make_shared<BasicFileInfo>();
  file->filePath = path;
  file->fileHash = "hash";
  task.fileInfos.push_back(file);
  return task;
}

}  // namespace

TEST(VpsServiceTest, InitAndUpdateFeatureLibSucceed) {
  VirusEngineService service;
  EXPECT_EQ(service.Init(), SUCCESS);
  EXPECT_EQ(service.UpdateFeatureLib(), SUCCESS);
  EXPECT_EQ(service.DeInit(), SUCCESS);
}

TEST(VpsServiceTest, ScanFileUsesFakeMultiEngineService) {
  VirusEngineService service;
  ASSERT_EQ(service.Init(), SUCCESS);

  ScanTask task = BuildScanTask("/tmp/demo-virus.bin");
  ScanResult result;
  EXPECT_EQ(service.ScanFile(&task, &result), SUCCESS);
  EXPECT_EQ(result.bundleName, task.bundleName);
  EXPECT_EQ(result.fileInfos.size(), task.fileInfos.size());
  EXPECT_EQ(result.engineResults.size(), static_cast<size_t>(VirusEngine::COUNT));
  EXPECT_EQ(result.threatLevel, ThreatLevel::HIGH_RISK);
}

TEST(VpsServiceTest, AnalysisEngineLifecycleTracksAccessToken) {
  VirusEngineService service;
  ASSERT_EQ(service.Init(), SUCCESS);

  EXPECT_EQ(service.IsExistAnalysisEngine(100u), FAILED);
  EXPECT_EQ(service.CreateAnalysisEngine(100u), SUCCESS);
  EXPECT_EQ(service.IsExistAnalysisEngine(100u), SUCCESS);
  EXPECT_EQ(service.DestroyAnalysisEngine(100u), SUCCESS);
  EXPECT_EQ(service.IsExistAnalysisEngine(100u), FAILED);
}

TEST(VpsServiceTest, ScanBehaviorProducesBroadcastEventWhenEnabled) {
  VirusEngineService service;
  ASSERT_EQ(service.Init(), SUCCESS);
  service.SetBehaviorReportEnabled(true);

  EXPECT_EQ(service.ScanBehavior(42u, "startup event", "com.demo.app"), SUCCESS);

  PollBehaviorEventReply reply = service.PollBehaviorEvent();
  EXPECT_EQ(reply.result, SUCCESS);
  EXPECT_EQ(reply.accessToken, 42u);
  EXPECT_EQ(reply.scanResult.bundleName, "com.demo.app");
  EXPECT_NE(reply.scanResult.ruleName, "");
}

}  // namespace OHOS::Security::VirusProtectionService
