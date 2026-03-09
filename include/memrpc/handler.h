#ifndef MEMRPC_HANDLER_H_
#define MEMRPC_HANDLER_H_

#include "memrpc/types.h"

namespace memrpc {

class IScanHandler {
 public:
  virtual ~IScanHandler() = default;

  virtual ScanResult HandleScan(const ScanRequest& request) = 0;
};

}  // namespace memrpc

#endif  // MEMRPC_HANDLER_H_
