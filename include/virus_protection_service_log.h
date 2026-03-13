#ifndef VIRUS_PROTECTION_SERVICE_LOG_H_
#define VIRUS_PROTECTION_SERVICE_LOG_H_

#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>

namespace MemRpc {

enum class LogLevel : uint8_t {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
};

std::string NormalizeLogFormat(std::string_view format);
void LogPrint(LogLevel level, const char* file, int line, const char* format, ...);
void LogVPrint(LogLevel level, const char* file, int line, const char* format, va_list args);

}  // namespace MemRpc

#define HILOGD(format, ...)                                                                    \
  ::MemRpc::LogPrint(                                                                         \
      ::MemRpc::LogLevel::Debug, __FILE__, __LINE__,                                          \
      format, ##__VA_ARGS__)

#define HILOGI(format, ...)                                                                   \
  ::MemRpc::LogPrint(                                                                        \
      ::MemRpc::LogLevel::Info, __FILE__, __LINE__,                                         \
      format, ##__VA_ARGS__)

#define HILOGW(format, ...)                                                                   \
  ::MemRpc::LogPrint(                                                                        \
      ::MemRpc::LogLevel::Warn, __FILE__, __LINE__,                                         \
      format, ##__VA_ARGS__)

#define HILOGE(format, ...)                                                                    \
  ::MemRpc::LogPrint(                                                                         \
      ::MemRpc::LogLevel::Error, __FILE__, __LINE__,                                          \
      format, ##__VA_ARGS__)

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // VIRUS_PROTECTION_SERVICE_LOG_H_
