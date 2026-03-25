#ifndef OHOS_SA_MOCK_ISYSTEM_ABILITY_LOAD_CALLBACK_H
#define OHOS_SA_MOCK_ISYSTEM_ABILITY_LOAD_CALLBACK_H

#include "iremote_stub.h"

namespace OHOS {

class ISystemAbilityLoadCallback : public IRemoteBroker {
public:
    ~ISystemAbilityLoadCallback() override = default;

    virtual void OnLoadSystemAbilitySuccess(int32_t systemAbilityId, const sptr<IRemoteObject>& object) = 0;
    virtual void OnLoadSystemAbilityFail(int32_t systemAbilityId) = 0;
};

class SystemAbilityLoadCallbackStub : public IRemoteStub<ISystemAbilityLoadCallback> {
public:
    ~SystemAbilityLoadCallbackStub() override = default;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_ISYSTEM_ABILITY_LOAD_CALLBACK_H
