#include <cstddef>
#include <cstdint>
#include <string>

#include "ves/ves_codec.h"
#include "ves/ves_sample_rules.h"
#include "ves/ves_types.h"

namespace {

std::string SanitizePath(std::string path) {
    // Strip "crash" to avoid abort().
    const std::string kCrash = "crash";
    for (;;) {
        auto pos = path.find(kCrash);
        if (pos == std::string::npos) break;
        path.erase(pos, kCrash.size());
    }

    // Clamp sleep to 0 by replacing "sleep" digits with "sleep0".
    const std::string kSleep = "sleep";
    auto pos = path.find(kSleep);
    if (pos != std::string::npos) {
        size_t i = pos + kSleep.size();
        while (i < path.size() && std::isdigit(static_cast<unsigned char>(path[i]))) {
            path.erase(i, 1);
        }
        path.insert(pos + kSleep.size(), "0");
    }
    return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    virus_executor_service::ScanFileRequest req;
    if (!MemRpc::CodecTraits<virus_executor_service::ScanFileRequest>::Decode(data, size, &req)) {
        return 0;
    }

    req.filePath = SanitizePath(std::move(req.filePath));
    (void)virus_executor_service::EvaluateSamplePath(req.filePath);

    virus_executor_service::ScanFileReply reply;
    reply.code = 0;
    reply.threatLevel = 0;

    std::vector<uint8_t> bytes;
    if (MemRpc::CodecTraits<virus_executor_service::ScanFileReply>::Encode(reply, &bytes)) {
        virus_executor_service::ScanFileReply decoded;
        (void)MemRpc::CodecTraits<virus_executor_service::ScanFileReply>::Decode(bytes.data(), bytes.size(), &decoded);
    }

    return 0;
}
