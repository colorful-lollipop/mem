# ohos_sa_mock

`ohos_sa_mock` 是一个 Linux 下可编译、可做简单联调的 HarmonyOS `SystemAbility` 兼容层。

目标是先把代码组织方式对齐到 OpenHarmony 常见 SA 写法，而不是模拟完整 Binder。

## 当前支持

- `sptr<T>` / `wptr<T>`
- `IRemoteObject`
- `IRemoteObject::DeathRecipient`
- `IRemoteBroker`
- `IRemoteStub<T>`
- `IRemoteProxy<T>`
- `iface_cast<T>()`
- `SystemAbility`
- `ISystemAbilityManager`
- `SystemAbilityManagerClient`
- `SystemAbilityLoadCallbackStub`
- `GetSystemAbility`
- `CheckSystemAbility`
- `LoadSystemAbility`
- `UnloadSystemAbility`

## 当前不支持

- `MessageParcel`
- `SendRequest`
- 真正跨进程 IPC
- 权限系统
- 分布式 SA
- `sa_profile` 解析和自动拉起

## 目录

```text
include/   兼容头文件
src/       Linux mock 实现
tests/     独立单元测试
demo/      最小可运行示例
docs/      额外文档
```

## 单独构建

```bash
cmake -S third_party/ohos_sa_mock -B third_party/ohos_sa_mock/build
cmake --build third_party/ohos_sa_mock/build
ctest --test-dir third_party/ohos_sa_mock/build --output-on-failure
./third_party/ohos_sa_mock/build/ohos_sa_mock_demo
```

## 父仓库集成

父仓库通过子目录依赖 `ohos_sa_mock`，业务代码可以直接：

```cpp
#include "system_ability.h"
#include "iservice_registry.h"
#include "iremote_object.h"
```

## 示例

一个最小 SA 服务可以写成：

```cpp
class IDemoService : public OHOS::IRemoteBroker {
 public:
  ~IDemoService() override = default;
  virtual std::string Ping() = 0;
};

class DemoService : public OHOS::SystemAbility,
                    public OHOS::IRemoteStub<IDemoService> {
 public:
  DemoService() : OHOS::SystemAbility(40110, true) {}
  std::string Ping() override { return "pong"; }
};
```

更多细节见 [docs/testing.md](docs/testing.md)。
