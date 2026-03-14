#ifndef OHOS_SA_MOCK_IREMOTE_STUB_H
#define OHOS_SA_MOCK_IREMOTE_STUB_H

#include "iremote_broker.h"
#include "iremote_object.h"
#include "mock_ipc_types.h"

namespace OHOS {

template <typename T>
class IRemoteStub : public T {
 public:
  ~IRemoteStub() override
  {
    if (remote_ != nullptr) {
      remote_->SetRequestHandler({});
    }
  }

  virtual bool OnRemoteRequest(int command, MockIpcReply* reply)
  {
    static_cast<void>(command);
    static_cast<void>(reply);
    return false;
  }

  sptr<IRemoteObject> AsObject() override
  {
    if (remote_ == nullptr) {
      remote_ = std::make_shared<IRemoteObject>();
      auto broker =
          std::dynamic_pointer_cast<IRemoteBroker>(RefBase::shared_from_this());
      auto self =
          std::dynamic_pointer_cast<IRemoteStub<T>>(RefBase::shared_from_this());
      wptr<IRemoteStub<T>> weak_self = self;
      remote_->AttachBroker(broker);
      remote_->SetRequestHandler([weak_self](int cmd, MockIpcReply* reply) {
        auto self = weak_self.lock();
        if (self == nullptr) {
          return false;
        }
        return self->OnRemoteRequest(cmd, reply);
      });
    }
    return remote_;
  }

 private:
  sptr<IRemoteObject> remote_;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_IREMOTE_STUB_H
