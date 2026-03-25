#include "iremote_object.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "iremote_broker.h"

namespace OHOS {

class IRemoteObject::Impl {
public:
    std::mutex mutex;
    bool is_dead = false;
    std::vector<wptr<DeathRecipient>> recipients;
    wptr<IRemoteBroker> broker;
    int32_t sa_id = -1;
    std::string service_path;
    RemoteRequestHandler request_handler;
};

IRemoteObject::IRemoteObject()
    : impl_(std::make_shared<Impl>())
{
}

bool IRemoteObject::IsObjectDead() const
{
    if (impl_ == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->is_dead;
}

bool IRemoteObject::AddDeathRecipient(const sptr<DeathRecipient>& recipient)
{
    if (recipient == nullptr) {
        return false;
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
    impl_->recipients.erase(std::remove_if(impl_->recipients.begin(),
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
        impl_->is_dead = true;
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

void IRemoteObject::SetSaId(int32_t saId)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sa_id = saId;
}

int32_t IRemoteObject::GetSaId() const
{
    if (impl_ == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sa_id;
}

void IRemoteObject::SetServicePath(const std::string& path)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->service_path = path;
}

std::string IRemoteObject::GetServicePath() const
{
    if (impl_ == nullptr) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->service_path;
}

void IRemoteObject::SetRequestHandler(RemoteRequestHandler handler)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->request_handler = std::move(handler);
}

bool IRemoteObject::HandleRequest(int command, const MockIpcRequest& request, MockIpcReply* reply)
{
    if (impl_ == nullptr) {
        return false;
    }
    RemoteRequestHandler handler;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        handler = impl_->request_handler;
    }
    if (!handler) {
        return false;
    }
    return handler(command, request, reply);
}

}  // namespace OHOS
