#ifndef OHOS_SA_MOCK_IREMOTE_BROKER_REGISTRY_H
#define OHOS_SA_MOCK_IREMOTE_BROKER_REGISTRY_H

#include <cstdint>
#include <functional>
#include <memory>

#include "iremote_object.h"

namespace OHOS {

class IRemoteBroker;

using BrokerDelegateCreator = std::function<sptr<IRemoteBroker>(const sptr<IRemoteObject>&)>;

class BrokerRegistration {
 public:
  static BrokerRegistration& GetInstance();

  void Register(int32_t saId, BrokerDelegateCreator creator);
  sptr<IRemoteBroker> CreateBroker(int32_t saId, const sptr<IRemoteObject>& remote) const;

 private:
  BrokerRegistration() = default;

  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_IREMOTE_BROKER_REGISTRY_H
