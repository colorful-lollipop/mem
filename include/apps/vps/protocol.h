#ifndef APPS_VPS_PROTOCOL_H_
#define APPS_VPS_PROTOCOL_H_

#include <cstdint>

namespace OHOS::Security::VirusProtectionService {

enum class VpsOpcode : uint16_t {
    VpsInit = 100,
    VpsDeInit = 101,
    VpsScanFile = 102,
    VpsScanBehavior = 103,
    VpsIsExistAnalysisEngine = 104,
    VpsCreateAnalysisEngine = 105,
    VpsDestroyAnalysisEngine = 106,
    VpsUpdateFeatureLib = 107,
};

inline constexpr uint32_t VPS_MAX_FILE_PATH_SIZE = 1024u;
inline constexpr uint32_t VPS_MAX_MESSAGE_SIZE = 512u;

struct ScanFileRequestPayload {
    uint32_t file_path_length = 0;
    char file_path[VPS_MAX_FILE_PATH_SIZE]{};
};

struct ScanFileResponsePayload {
    uint32_t verdict = 0;
    uint32_t message_length = 0;
    char message[VPS_MAX_MESSAGE_SIZE]{};
};

}  // namespace OHOS::Security::VirusProtectionService

#endif  // APPS_VPS_PROTOCOL_H_
