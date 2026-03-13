#include "system_ability.h"

#include <mutex>
#include <unordered_map>

#include "if_system_ability_manager.h"
#include "iremote_stub.h"
#include "isam_backend.h"

namespace OHOS {

namespace {

class LocalSystemAbilityManager : public IRemoteStub<ISystemAbilityManager> {
 public:
  ErrCode AddSystemAbility(int32_t systemAbilityId,
                           const sptr<IRemoteObject>& object) override
  {
    if (object == nullptr) {
      return ERR_NULL_OBJECT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    abilities_[systemAbilityId] = object;
    return ERR_OK;
  }

  sptr<IRemoteObject> GetSystemAbility(int32_t systemAbilityId) override
  {
    return CheckSystemAbility(systemAbilityId);
  }

  sptr<IRemoteObject> CheckSystemAbility(int32_t systemAbilityId) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = abilities_.find(systemAbilityId);
    if (it == abilities_.end()) {
      return nullptr;
    }
    return it->second;
  }

  sptr<IRemoteObject> LoadSystemAbility(int32_t systemAbilityId,
                                         int32_t /*timeout*/) override
  {
    return CheckSystemAbility(systemAbilityId);
  }

  ErrCode UnloadSystemAbility(int32_t systemAbilityId) override
  {
    sptr<IRemoteObject> object;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = abilities_.find(systemAbilityId);
      if (it == abilities_.end()) {
        return ERR_NAME_NOT_FOUND;
      }
      object = it->second;
      abilities_.erase(it);
    }
    if (object != nullptr) {
      object->NotifyRemoteDiedForTest();
    }
    return ERR_OK;
  }

 private:
  std::mutex mutex_;
  std::unordered_map<int32_t, sptr<IRemoteObject>> abilities_;
};

class RemoteSystemAbilityManager : public IRemoteStub<ISystemAbilityManager> {
 public:
  explicit RemoteSystemAbilityManager(std::shared_ptr<ISamBackend> backend)
      : backend_(std::move(backend)) {}

  ErrCode AddSystemAbility(int32_t systemAbilityId,
                           const sptr<IRemoteObject>& object) override
  {
    if (object == nullptr) {
      return ERR_NULL_OBJECT;
    }
    std::string servicePath = object->GetServicePath();
    if (!backend_->AddService(systemAbilityId, servicePath)) {
      return ERR_INVALID_VALUE;
    }
    return ERR_OK;
  }

  sptr<IRemoteObject> GetSystemAbility(int32_t systemAbilityId) override
  {
    return CheckSystemAbility(systemAbilityId);
  }

  sptr<IRemoteObject> CheckSystemAbility(int32_t systemAbilityId) override
  {
    std::string path = backend_->GetServicePath(systemAbilityId);
    if (path.empty()) {
      return nullptr;
    }
    auto remote = std::make_shared<IRemoteObject>();
    remote->SetSaId(systemAbilityId);
    remote->SetServicePath(path);
    return remote;
  }

  sptr<IRemoteObject> LoadSystemAbility(int32_t systemAbilityId,
                                         int32_t /*timeout*/) override
  {
    std::string path = backend_->LoadService(systemAbilityId);
    if (path.empty()) {
      return nullptr;
    }
    auto remote = std::make_shared<IRemoteObject>();
    remote->SetSaId(systemAbilityId);
    remote->SetServicePath(path);
    return remote;
  }

  ErrCode UnloadSystemAbility(int32_t systemAbilityId) override
  {
    if (!backend_->UnloadService(systemAbilityId)) {
      return ERR_NAME_NOT_FOUND;
    }
    return ERR_OK;
  }

 private:
  std::shared_ptr<ISamBackend> backend_;
};

sptr<ISystemAbilityManager> GetOrCreateLocalManager()
{
  static sptr<ISystemAbilityManager> manager = std::make_shared<LocalSystemAbilityManager>();
  return manager;
}

}  // namespace

SystemAbility::SystemAbility(int32_t systemAbilityId, bool runOnCreate)
    : system_ability_id_(systemAbilityId), run_on_create_(runOnCreate) {}

SystemAbility::~SystemAbility()
{
  transport_.Stop();
}

void SystemAbility::OnStart() {}

void SystemAbility::OnStop() {}

void SystemAbility::OnAddSystemAbility(int32_t systemAbilityId, const std::string& deviceId)
{
  static_cast<void>(systemAbilityId);
  static_cast<void>(deviceId);
}

void SystemAbility::OnRemoveSystemAbility(int32_t systemAbilityId, const std::string& deviceId)
{
  static_cast<void>(systemAbilityId);
  static_cast<void>(deviceId);
}

int32_t SystemAbility::GetSystemAbilityId() const
{
  return system_ability_id_;
}

bool SystemAbility::IsRunOnCreate() const
{
  return run_on_create_;
}

bool SystemAbility::Publish(const sptr<IRemoteObject>& object)
{
  auto registry = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
  if (registry == nullptr || object == nullptr) {
    return false;
  }
  if (registry->AddSystemAbility(system_ability_id_, object) != ERR_OK) {
    return false;
  }

  // Framework auto-starts mock transport if a service path is set.
  const std::string path = object->GetServicePath();
  if (!path.empty()) {
    transport_.Start(path, [object](int cmd, MockIpcReply* reply) {
      return object->HandleRequest(cmd, reply);
    });
  }
  return true;
}

SystemAbilityManagerClient& SystemAbilityManagerClient::GetInstance()
{
  static SystemAbilityManagerClient client;
  return client;
}

sptr<ISystemAbilityManager> SystemAbilityManagerClient::GetSystemAbilityManager()
{
  if (backend_ != nullptr) {
    return std::make_shared<RemoteSystemAbilityManager>(backend_);
  }
  return GetOrCreateLocalManager();
}

void SystemAbilityManagerClient::SetBackend(const std::shared_ptr<ISamBackend>& backend)
{
  backend_ = backend;
}

}  // namespace OHOS
