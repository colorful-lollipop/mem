#include "iremote_broker_registry.h"

#include <mutex>
#include <unordered_map>

#include "iremote_broker.h"

namespace OHOS {

class BrokerRegistration::Impl {
 public:
  mutable std::mutex mutex;
  std::unordered_map<int32_t, BrokerDelegateCreator> creators;
};

BrokerRegistration& BrokerRegistration::GetInstance()
{
  static BrokerRegistration instance;
  return instance;
}

void BrokerRegistration::Register(int32_t saId, BrokerDelegateCreator creator)
{
  if (impl_ == nullptr) {
    impl_ = std::make_shared<Impl>();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->creators[saId] = std::move(creator);
}

sptr<IRemoteBroker> BrokerRegistration::CreateBroker(
    int32_t saId, const sptr<IRemoteObject>& remote) const
{
  if (impl_ == nullptr || remote == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->creators.find(saId);
  if (it == impl_->creators.end()) {
    return nullptr;
  }
  return it->second(remote);
}

}  // namespace OHOS
