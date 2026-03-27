#include "memrpc/client/dev_bootstrap.h"

#include <mutex>

#include "memrpc/server/dev_session_host.h"

namespace MemRpc {

struct DevBootstrapChannel::Impl {
    mutable std::mutex mutex;
    std::shared_ptr<DevSessionHost> sessionHost;
    EngineDeathCallback death_callback;
};

DevBootstrapChannel::DevBootstrapChannel(DevBootstrapConfig config)
    : impl_(std::make_shared<Impl>())
{
    impl_->sessionHost = std::make_shared<DevSessionHost>(std::move(config));
}

DevBootstrapChannel::~DevBootstrapChannel() = default;

StatusCode DevBootstrapChannel::OpenSession(BootstrapHandles& handles)
{
    return impl_->sessionHost->OpenSession(handles);
}

StatusCode DevBootstrapChannel::CloseSession()
{
    return impl_->sessionHost->CloseSession();
}

ChannelHealthResult DevBootstrapChannel::CheckHealth(uint64_t expectedSessionId)
{
    (void)expectedSessionId;
    return {};
}

void DevBootstrapChannel::SetEngineDeathCallback(EngineDeathCallback callback)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->death_callback = std::move(callback);
}

BootstrapHandles DevBootstrapChannel::serverHandles() const
{
    return impl_->sessionHost->serverHandles();
}

void DevBootstrapChannel::SimulateEngineDeathForTest(uint64_t session_id)
{
    EngineDeathCallback callback;
    uint64_t dead_session_id = session_id == 0 ? impl_->sessionHost->sessionId() : session_id;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        dead_session_id = session_id == 0 ? impl_->sessionHost->sessionId() : session_id;
        if (session_id == 0 || session_id == impl_->sessionHost->sessionId()) {
            (void)impl_->sessionHost->CloseSession();
        }
        callback = impl_->death_callback;
    }
    if (callback) {
        callback(dead_session_id);
    }
}

}  // namespace MemRpc
