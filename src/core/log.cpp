#include "virus_protection_service_log.h"

#include <cstdio>
#include <vector>

namespace memrpc {
namespace {

constexpr const char* kLogTag = "MemRpc";

char LevelChar(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return 'D';
    case LogLevel::Info:
      return 'I';
    case LogLevel::Warn:
      return 'W';
    case LogLevel::Error:
      return 'E';
  }
  return 'U';
}

}  // namespace

std::string NormalizeLogFormat(std::string_view format) {
  std::string normalized;
  normalized.reserve(format.size());

  for (size_t i = 0; i < format.size(); ++i) {
    if (format.compare(i, 9, "%{public}") == 0) {
      normalized.push_back('%');
      i += 8;
      continue;
    }
    if (format.compare(i, 10, "%{private}") == 0) {
      normalized.push_back('%');
      i += 9;
      continue;
    }
    normalized.push_back(format[i]);
  }

  return normalized;
}

void LogVPrint(LogLevel level, const char* file, int line, const char* format, va_list args) {
  if (format == nullptr) {
    return;
  }

  const std::string normalized = NormalizeLogFormat(format);
  va_list copied_args;
  va_copy(copied_args, args);
  const int required = std::vsnprintf(nullptr, 0, normalized.c_str(), copied_args);
  va_end(copied_args);
  if (required < 0) {
    return;
  }

  std::vector<char> buffer(static_cast<size_t>(required) + 1u, '\0');
  va_copy(copied_args, args);
  std::vsnprintf(buffer.data(), buffer.size(), normalized.c_str(), copied_args);
  va_end(copied_args);

  std::fprintf(stderr, "[%c][%s][%s:%d] %s\n", LevelChar(level), kLogTag, file, line,
               buffer.data());
}

void LogPrint(LogLevel level, const char* file, int line, const char* format, ...) {
  va_list args;
  va_start(args, format);
  LogVPrint(level, file, line, format, args);
  va_end(args);
}

}  // namespace memrpc
