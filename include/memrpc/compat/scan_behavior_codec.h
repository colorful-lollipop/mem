#ifndef MEMRPC_COMPAT_SCAN_BEHAVIOR_CODEC_H_
#define MEMRPC_COMPAT_SCAN_BEHAVIOR_CODEC_H_

#include <cstdint>
#include <vector>

#include "memrpc/core/types.h"

namespace memrpc {

bool EncodeScanBehaviorRequest(const ScanBehaviorRequest& request, std::vector<uint8_t>* bytes);
bool DecodeScanBehaviorRequest(const std::vector<uint8_t>& bytes, ScanBehaviorRequest* request);

bool EncodeScanBehaviorResult(const ScanBehaviorResult& result, std::vector<uint8_t>* bytes);
bool DecodeScanBehaviorResult(const std::vector<uint8_t>& bytes, ScanBehaviorResult* result);

}  // namespace memrpc

#endif  // MEMRPC_COMPAT_SCAN_BEHAVIOR_CODEC_H_
