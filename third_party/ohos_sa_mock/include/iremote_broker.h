#ifndef OHOS_SA_MOCK_IREMOTE_BROKER_H
#define OHOS_SA_MOCK_IREMOTE_BROKER_H

#include <memory>

#include "iremote_object.h"

namespace OHOS {

class IRemoteBroker : public virtual RefBase {
 public:
  ~IRemoteBroker() override = default;
  virtual sptr<IRemoteObject> AsObject() = 0;
};

template <typename T>
sptr<T> iface_cast(const sptr<IRemoteObject>& object)
{
  if (object == nullptr) {
    return nullptr;
  }
  auto broker = object->GetBroker();
  return std::dynamic_pointer_cast<T>(broker);
}

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_IREMOTE_BROKER_H
