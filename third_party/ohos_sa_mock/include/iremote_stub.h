#ifndef OHOS_SA_MOCK_IREMOTE_STUB_H
#define OHOS_SA_MOCK_IREMOTE_STUB_H

#include "iremote_broker.h"
#include "iremote_object.h"

namespace OHOS {

template <typename T>
class IRemoteStub : public T {
 public:
  ~IRemoteStub() override = default;

  sptr<IRemoteObject> AsObject() override
  {
    if (remote_ == nullptr) {
      remote_ = std::make_shared<IRemoteObject>();
      auto broker =
          std::dynamic_pointer_cast<IRemoteBroker>(RefBase::shared_from_this());
      remote_->AttachBroker(broker);
    }
    return remote_;
  }

 private:
  sptr<IRemoteObject> remote_;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_IREMOTE_STUB_H
