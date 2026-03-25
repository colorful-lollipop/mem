#ifndef OHOS_SA_MOCK_IREMOTE_PROXY_H
#define OHOS_SA_MOCK_IREMOTE_PROXY_H

#include "iremote_broker.h"

namespace OHOS {

template <typename T>
class IRemoteProxy : public T {
public:
    explicit IRemoteProxy(const sptr<IRemoteObject>& remote)
        : remote_(remote)
    {
    }
    ~IRemoteProxy() override = default;

    [[nodiscard]] sptr<IRemoteObject> Remote() const
    {
        return remote_;
    }

    sptr<IRemoteObject> AsObject() override
    {
        return remote_;
    }

private:
    sptr<IRemoteObject> remote_;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_IREMOTE_PROXY_H
