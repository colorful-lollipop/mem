#ifndef OHOS_SA_MOCK_ISAM_BACKEND_H
#define OHOS_SA_MOCK_ISAM_BACKEND_H

#include <cstdint>
#include <string>

namespace OHOS {

class ISamBackend {
 public:
  virtual ~ISamBackend() = default;
  virtual std::string GetServicePath(int32_t saId) = 0;
  virtual std::string LoadService(int32_t saId) = 0;
  virtual bool AddService(int32_t saId, const std::string& servicePath) = 0;
  virtual bool UnloadService(int32_t saId) = 0;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_ISAM_BACKEND_H
