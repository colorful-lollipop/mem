#ifndef OHOS_SA_MOCK_IREMOTE_OBJECT_H
#define OHOS_SA_MOCK_IREMOTE_OBJECT_H

#include "refbase.h"

namespace OHOS {

class IRemoteBroker;

class IRemoteObject : public RefBase {
 public:
  class DeathRecipient : public RefBase {
   public:
    ~DeathRecipient() override = default;
    virtual void OnRemoteDied(const wptr<IRemoteObject>& remote) = 0;
  };

  ~IRemoteObject() override = default;

  virtual bool AddDeathRecipient(const sptr<DeathRecipient>& recipient);
  virtual bool RemoveDeathRecipient(const sptr<DeathRecipient>& recipient);
  virtual void NotifyRemoteDiedForTest();

  void AttachBroker(const sptr<IRemoteBroker>& broker);
  sptr<IRemoteBroker> GetBroker() const;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_IREMOTE_OBJECT_H
