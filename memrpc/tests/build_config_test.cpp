#include <gtest/gtest.h>

#include <fstream>
#include <string>

#define MEMRPC_SOURCE_PATH(rel) MEMRPC_SOURCE_DIR rel
#define MEMRPC_REPO_PATH(rel) MEMRPC_REPO_ROOT rel

TEST(BuildConfigTest, ExportCompileCommandsIsEnabled)
{
    std::ifstream stream(MEMRPC_REPO_PATH("/CMakeLists.txt"));
    ASSERT_TRUE(stream.is_open());

    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("CMAKE_EXPORT_COMPILE_COMMANDS ON"), std::string::npos);
}

TEST(BuildConfigTest, CMakeExplicitlyRequiresCpp17)
{
    std::ifstream stream(MEMRPC_REPO_PATH("/CMakeLists.txt"));
    ASSERT_TRUE(stream.is_open());

    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("CMAKE_CXX_STANDARD 17"), std::string::npos);
    EXPECT_NE(content.find("CMAKE_CXX_STANDARD_REQUIRED ON"), std::string::npos);
}
