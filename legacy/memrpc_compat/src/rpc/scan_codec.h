#ifndef MEMRPC_RPC_SCAN_CODEC_H_
#define MEMRPC_RPC_SCAN_CODEC_H_

#include <cstdint>
#include <vector>

#include "memrpc/types.h"

namespace memrpc {

bool EncodeScanRequest(const ScanRequest& request, std::vector<uint8_t>* bytes);
bool DecodeScanRequest(const std::vector<uint8_t>& bytes, ScanRequest* request);

bool EncodeScanResult(const ScanResult& result, std::vector<uint8_t>* bytes);
bool DecodeScanResult(const std::vector<uint8_t>& bytes, ScanResult* result);

}  // namespace memrpc

#endif  // MEMRPC_RPC_SCAN_CODEC_H_
