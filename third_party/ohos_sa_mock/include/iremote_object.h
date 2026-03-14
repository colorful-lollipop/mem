#ifndef OHOS_SA_MOCK_IREMOTE_OBJECT_H
#define OHOS_SA_MOCK_IREMOTE_OBJECT_H

#include <functional>
#include <string>

#include "mock_ipc_types.h"
#include "refbase.h"

namespace OHOS {

class IRemoteBroker;

using RemoteRequestHandler = std::function<bool(int command, const MockIpcRequest& request,
                                                MockIpcReply* reply)>;

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

  void SetSaId(int32_t saId);
  int32_t GetSaId() const;

  void SetServicePath(const std::string& path);
  std::string GetServicePath() const;

  void SetRequestHandler(RemoteRequestHandler handler);
  bool HandleRequest(int command, const MockIpcRequest& request, MockIpcReply* reply);

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace OHOS

#endif  // OHOS_SA_MOCK_IREMOTE_OBJECT_H
