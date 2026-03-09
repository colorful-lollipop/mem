#include "iremote_object.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "iremote_broker.h"

namespace OHOS {

class IRemoteObject::Impl {
 public:
  std::mutex mutex;
  std::vector<wptr<DeathRecipient>> recipients;
  wptr<IRemoteBroker> broker;
};

bool IRemoteObject::AddDeathRecipient(const sptr<DeathRecipient>& recipient)
{
  if (recipient == nullptr) {
    return false;
  }
  if (impl_ == nullptr) {
    impl_ = std::make_shared<Impl>();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->recipients.push_back(recipient);
  return true;
}

bool IRemoteObject::RemoveDeathRecipient(const sptr<DeathRecipient>& recipient)
{
  if (recipient == nullptr || impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto original_size = impl_->recipients.size();
  impl_->recipients.erase(
      std::remove_if(
          impl_->recipients.begin(),
          impl_->recipients.end(),
          [&](const wptr<DeathRecipient>& current) {
            auto locked = current.lock();
            return locked == nullptr || locked == recipient;
          }),
      impl_->recipients.end());
  return impl_->recipients.size() != original_size;
}

void IRemoteObject::NotifyRemoteDiedForTest()
{
  if (impl_ == nullptr) {
    return;
  }

  std::vector<sptr<DeathRecipient>> recipients;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& weak_recipient : impl_->recipients) {
      auto recipient = weak_recipient.lock();
      if (recipient != nullptr) {
        recipients.push_back(std::move(recipient));
      }
    }
  }

  auto self = std::static_pointer_cast<IRemoteObject>(shared_from_this());
  for (const auto& recipient : recipients) {
    recipient->OnRemoteDied(self);
  }
}

void IRemoteObject::AttachBroker(const sptr<IRemoteBroker>& broker)
{
  if (impl_ == nullptr) {
    impl_ = std::make_shared<Impl>();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->broker = broker;
}

sptr<IRemoteBroker> IRemoteObject::GetBroker() const
{
  if (impl_ == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->broker.lock();
}

}  // namespace OHOS
