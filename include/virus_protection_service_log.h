#ifndef VIRUS_PROTECTION_SERVICE_LOG_H_
#define VIRUS_PROTECTION_SERVICE_LOG_H_

#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

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

template <typename... Args>
inline void LogWrite(LogLevel level, const char* file, int line, const char* format, Args&&... args)
{
    LogPrint(level, file, line, format, std::forward<Args>(args)...);
}

}  // namespace MemRpc

#define HILOGD(...) ::MemRpc::LogWrite(::MemRpc::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)

#define HILOGI(...) ::MemRpc::LogWrite(::MemRpc::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)

#define HILOGW(...) ::MemRpc::LogWrite(::MemRpc::LogLevel::Warn, __FILE__, __LINE__, __VA_ARGS__)

#define HILOGE(...) ::MemRpc::LogWrite(::MemRpc::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)

namespace OHOS {
namespace Security {
namespace VirusProtectionService {
namespace MemRpc = ::MemRpc;
}  // namespace VirusProtectionService
}  // namespace Security
}  // namespace OHOS

#endif  // VIRUS_PROTECTION_SERVICE_LOG_H_
