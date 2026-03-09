#include <gtest/gtest.h>

#include <fstream>
#include <string>

TEST(BuildConfigTest, ExportCompileCommandsIsEnabled) {
  std::ifstream stream("/root/code/demo/mem/CMakeLists.txt");
  ASSERT_TRUE(stream.is_open());

  std::string content((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("CMAKE_EXPORT_COMPILE_COMMANDS ON"), std::string::npos);
}
