#include <gtest/gtest.h>

#include <fstream>
#include <string>

#define MEMRPC_SOURCE_PATH(rel) MEMRPC_SOURCE_DIR rel
#define MEMRPC_REPO_PATH(rel) MEMRPC_REPO_ROOT rel

TEST(BuildConfigTest, ExportCompileCommandsIsEnabled) {
  std::ifstream stream(MEMRPC_REPO_PATH("/CMakeLists.txt"));
  ASSERT_TRUE(stream.is_open());

  std::string content((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("CMAKE_EXPORT_COMPILE_COMMANDS ON"), std::string::npos);
}

TEST(BuildConfigTest, CMakeExplicitlyRequiresCpp17) {
  std::ifstream stream(MEMRPC_REPO_PATH("/CMakeLists.txt"));
  ASSERT_TRUE(stream.is_open());

  std::string content((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("CMAKE_CXX_STANDARD 17"), std::string::npos);
  EXPECT_NE(content.find("CMAKE_CXX_STANDARD_REQUIRED ON"), std::string::npos);
}

TEST(BuildConfigTest, ParentRepoDoesNotDependOnOhosSaMock) {
  const char* kFiles[] = {
      MEMRPC_REPO_PATH("/CMakeLists.txt"),
      MEMRPC_SOURCE_PATH("/CMakeLists.txt"),
      MEMRPC_SOURCE_PATH("/src/CMakeLists.txt"),
      MEMRPC_REPO_PATH("/BUILD.gn"),
  };

  for (const char* path : kFiles) {
    std::ifstream stream(path);
    ASSERT_TRUE(stream.is_open()) << path;

    std::string content((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("ohos_sa_mock"), std::string::npos) << path;
    EXPECT_EQ(content.find("third_party/ohos_sa_mock"), std::string::npos) << path;
  }
}

TEST(BuildConfigTest, SourceBuildOnlyKeepsCoreMemrpcAndVpsdemo) {
  struct FileExpectation {
    const char* path;
    const char* forbidden[8];
  };

  const FileExpectation expectations[] = {
      {MEMRPC_SOURCE_PATH("/src/CMakeLists.txt"),
       {"client/engine_client.cpp",
        "server/engine_server.cpp",
        "memrpc/compat/scan_codec.cpp",
        "memrpc/compat/scan_behavior_codec.cpp",
        "src/rpc/",
        "vps_demo",
        "apps/vps/",
        nullptr}},
      {MEMRPC_REPO_PATH("/CMakeLists.txt"),
       {"add_subdirectory(src)", "add_subdirectory(tests)", nullptr}},
      {MEMRPC_REPO_PATH("/demo/CMakeLists.txt"),
       {"memrpc_minirpc_demo", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
  };

  for (const auto& expectation : expectations) {
    std::ifstream stream(expectation.path);
    ASSERT_TRUE(stream.is_open()) << expectation.path;

    std::string content((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
    for (const char* token : expectation.forbidden) {
      if (token == nullptr) {
        break;
      }
      EXPECT_EQ(content.find(token), std::string::npos) << expectation.path << " still references "
                                                         << token;
    }
  }
}

TEST(BuildConfigTest, TestsAreSplitBetweenFrameworkAndVpsdemoDirectories) {
  std::ifstream root_stream(MEMRPC_REPO_PATH("/CMakeLists.txt"));
  ASSERT_TRUE(root_stream.is_open());

  std::string root_content((std::istreambuf_iterator<char>(root_stream)),
                           std::istreambuf_iterator<char>());
  EXPECT_NE(root_content.find("add_subdirectory(memrpc)"), std::string::npos);
  EXPECT_EQ(root_content.find("add_subdirectory(src)"), std::string::npos);
  EXPECT_EQ(root_content.find("add_subdirectory(tests)"), std::string::npos);

  std::ifstream memrpc_stream(MEMRPC_SOURCE_PATH("/tests/CMakeLists.txt"));
  ASSERT_TRUE(memrpc_stream.is_open());

  std::string memrpc_content((std::istreambuf_iterator<char>(memrpc_stream)),
                             std::istreambuf_iterator<char>());
  EXPECT_NE(memrpc_content.find("memrpc_smoke_test"), std::string::npos);
  EXPECT_NE(memrpc_content.find("memrpc_session_test"), std::string::npos);

  std::ifstream vpsdemo_stream(MEMRPC_REPO_PATH("/demo/vpsdemo/CMakeLists.txt"));
  ASSERT_TRUE(vpsdemo_stream.is_open());

  std::string vpsdemo_content((std::istreambuf_iterator<char>(vpsdemo_stream)),
                              std::istreambuf_iterator<char>());
  EXPECT_NE(vpsdemo_content.find("vpsdemo_testkit_client_test"), std::string::npos);
  EXPECT_NE(vpsdemo_content.find("vpsdemo_testkit_dfx_test"), std::string::npos);
}

TEST(BuildConfigTest, TopLevelMemrpcForwardingHeadersAreNotPartOfMainline) {
  const char* kLegacyHeaders[] = {
      MEMRPC_SOURCE_PATH("/include/memrpc/bootstrap.h"),
      MEMRPC_SOURCE_PATH("/include/memrpc/types.h"),
      MEMRPC_SOURCE_PATH("/include/memrpc/client.h"),
      MEMRPC_SOURCE_PATH("/include/memrpc/demo_bootstrap.h"),
      MEMRPC_SOURCE_PATH("/include/memrpc/sa_bootstrap.h"),
      MEMRPC_SOURCE_PATH("/include/memrpc/handler.h"),
      MEMRPC_SOURCE_PATH("/include/memrpc/rpc_client.h"),
      MEMRPC_SOURCE_PATH("/include/memrpc/rpc_server.h"),
      MEMRPC_SOURCE_PATH("/include/memrpc/server.h"),
      nullptr,
  };

  for (const char** path = kLegacyHeaders; *path != nullptr; ++path) {
    std::ifstream stream(*path);
    EXPECT_FALSE(stream.is_open()) << *path << " should be removed from mainline";
  }
}
