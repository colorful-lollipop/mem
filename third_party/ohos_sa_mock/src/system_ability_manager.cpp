#include "system_ability.h"

#include <mutex>
#include <unordered_map>

#include "if_system_ability_manager.h"
#include "isystem_ability_load_callback.h"

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

  ErrCode LoadSystemAbility(int32_t systemAbilityId,
                            const sptr<SystemAbilityLoadCallbackStub>& callback) override
  {
    auto object = CheckSystemAbility(systemAbilityId);
    if (callback == nullptr) {
      return ERR_NULL_OBJECT;
    }
    if (object == nullptr) {
      callback->OnLoadSystemAbilityFail(systemAbilityId);
      return ERR_NAME_NOT_FOUND;
    }
    callback->OnLoadSystemAbilitySuccess(systemAbilityId, object);
    return ERR_OK;
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

sptr<ISystemAbilityManager> GetOrCreateManager()
{
  static sptr<ISystemAbilityManager> manager = std::make_shared<LocalSystemAbilityManager>();
  return manager;
}

}  // namespace

SystemAbility::SystemAbility(int32_t systemAbilityId, bool runOnCreate)
    : system_ability_id_(systemAbilityId), run_on_create_(runOnCreate) {}

void SystemAbility::OnStart() {}

void SystemAbility::OnStop() {}

void SystemAbility::OnAddSystemAbility(int32_t systemAbilityId, const std::string& deviceId) {}

void SystemAbility::OnRemoveSystemAbility(int32_t systemAbilityId, const std::string& deviceId) {}

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
  return registry->AddSystemAbility(system_ability_id_, object) == ERR_OK;
}

SystemAbilityManagerClient& SystemAbilityManagerClient::GetInstance()
{
  static SystemAbilityManagerClient client;
  return client;
}

sptr<ISystemAbilityManager> SystemAbilityManagerClient::GetSystemAbilityManager()
{
  return GetOrCreateManager();
}

}  // namespace OHOS
