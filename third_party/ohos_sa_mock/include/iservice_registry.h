#ifndef OHOS_SA_MOCK_ISERVICE_REGISTRY_H
#define OHOS_SA_MOCK_ISERVICE_REGISTRY_H

#include <memory>

#include "if_system_ability_manager.h"
#include "isam_backend.h"

namespace OHOS {

class SystemAbilityManagerClient {
public:
    static SystemAbilityManagerClient& GetInstance();

    sptr<ISystemAbilityManager> GetSystemAbilityManager();

    void SetBackend(const std::shared_ptr<ISamBackend>& backend);

private:
    SystemAbilityManagerClient() = default;

    std::shared_ptr<ISamBackend> backend_;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_ISERVICE_REGISTRY_H
