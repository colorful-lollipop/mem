#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "apps/vps/common/vps_codec.h"

namespace OHOS::Security::VirusProtectionService {
namespace {

ScanTask BuildScanTask() {
  ScanTask task;
  task.bundleName = "com.demo.app";
  task.scanTaskType = ScanTaskType::CUSTOM;
  task.accountId = 200;

  task.bundleInfo = std::make_shared<BundleInfo>();
  task.bundleInfo->bundleName = task.bundleName;
  task.bundleInfo->versionName = "1.0.1";
  task.bundleInfo->label = "Demo";
  task.bundleInfo->isEncrypted = true;

  auto file = std::make_shared<BasicFileInfo>();
  file->filePath = "/tmp/eicar.bin";
  file->fileHash = "hash-a";
  file->subFilePath = "inner/file";
  file->subFileHash = "hash-b";
  file->inode = 11;
  file->mtime = 22;
  file->fileSize = 33;
  task.fileInfos.push_back(file);
  return task;
}

ScanResult BuildScanResult() {
  ScanResult result;
  result.bundleName = "com.demo.app";
  result.scanTaskType = ScanTaskType::REAL_TIME;
  result.accountId = 201;
  result.threatLevel = ThreatLevel::HIGH_RISK;
  result.bakPath = "/tmp/quarantine.bak";
  result.bundleInfo = std::make_shared<BundleInfo>();
  result.bundleInfo->bundleName = "com.demo.app";
  result.bundleInfo->versionName = "2.0.0";

  auto file = std::make_shared<BasicFileInfo>();
  file->filePath = "/tmp/infected.apk";
  file->fileHash = "hash-z";
  result.fileInfos.push_back(file);

  result.engineResults[static_cast<size_t>(VirusEngine::CSPL_STATIC_ENGINE)].virusName = "EICAR";
  result.engineResults[static_cast<size_t>(VirusEngine::CSPL_STATIC_ENGINE)].virusType = "test";
  result.engineResults[static_cast<size_t>(VirusEngine::CSPL_STATIC_ENGINE)].errorMsg = "ok";
  result.engineResults[static_cast<size_t>(VirusEngine::CSPL_STATIC_ENGINE)].errorCode = SUCCESS;
  result.engineResults[static_cast<size_t>(VirusEngine::CSPL_STATIC_ENGINE)].level =
      ThreatLevel::HIGH_RISK;
  return result;
}

}  // namespace

TEST(VpsCodecTest, ScanTaskRoundTrips) {
  const ScanTask task = BuildScanTask();
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeScanTask(task, &bytes));

  ScanTask decoded;
  ASSERT_TRUE(DecodeScanTask(bytes, &decoded));
  EXPECT_EQ(decoded.bundleName, task.bundleName);
  ASSERT_NE(decoded.bundleInfo, nullptr);
  EXPECT_EQ(decoded.bundleInfo->versionName, task.bundleInfo->versionName);
  ASSERT_EQ(decoded.fileInfos.size(), 1u);
  ASSERT_NE(decoded.fileInfos.front(), nullptr);
  EXPECT_EQ(decoded.fileInfos.front()->filePath, task.fileInfos.front()->filePath);
  EXPECT_EQ(decoded.scanTaskType, task.scanTaskType);
  EXPECT_EQ(decoded.accountId, task.accountId);
}

TEST(VpsCodecTest, ScanResultRoundTrips) {
  const ScanResult result = BuildScanResult();
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeScanResult(result, &bytes));

  ScanResult decoded;
  ASSERT_TRUE(DecodeScanResult(bytes, &decoded));
  EXPECT_EQ(decoded.bundleName, result.bundleName);
  EXPECT_EQ(decoded.bakPath, result.bakPath);
  EXPECT_EQ(decoded.threatLevel, result.threatLevel);
  ASSERT_EQ(decoded.engineResults.size(), result.engineResults.size());
  EXPECT_EQ(decoded.engineResults[0].virusName, result.engineResults[0].virusName);
}

TEST(VpsCodecTest, BehaviorScanResultRoundTrips) {
  BehaviorScanResult result;
  result.eventId = "evt-1";
  result.time = "2026-03-09T10:00:00Z";
  result.ruleName = "startup_persist";
  result.bundleName = "com.demo.app";

  std::vector<uint8_t> bytes;
  ASSERT_TRUE(EncodeBehaviorScanResult(result, &bytes));

  BehaviorScanResult decoded;
  ASSERT_TRUE(DecodeBehaviorScanResult(bytes, &decoded));
  EXPECT_EQ(decoded.eventId, result.eventId);
  EXPECT_EQ(decoded.time, result.time);
  EXPECT_EQ(decoded.ruleName, result.ruleName);
  EXPECT_EQ(decoded.bundleName, result.bundleName);
}

}  // namespace OHOS::Security::VirusProtectionService
