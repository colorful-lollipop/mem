#include <cstddef>
#include <cstdint>
#include <string>

#include "vpsdemo_codec.h"
#include "vpsdemo_sample_rules.h"
#include "vpsdemo_types.h"

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
    vpsdemo::ScanFileRequest req;
    if (!memrpc::CodecTraits<vpsdemo::ScanFileRequest>::Decode(data, size, &req)) {
        return 0;
    }

    req.file_path = SanitizePath(std::move(req.file_path));
    (void)vpsdemo::EvaluateSamplePath(req.file_path);

    vpsdemo::ScanFileReply reply;
    reply.code = 0;
    reply.threat_level = 0;

    std::vector<uint8_t> bytes;
    if (memrpc::CodecTraits<vpsdemo::ScanFileReply>::Encode(reply, &bytes)) {
        vpsdemo::ScanFileReply decoded;
        (void)memrpc::CodecTraits<vpsdemo::ScanFileReply>::Decode(bytes.data(), bytes.size(), &decoded);
    }

    return 0;
}
