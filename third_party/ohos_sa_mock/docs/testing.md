# Testing

## 覆盖范围

当前测试只覆盖第一版最关键的兼容能力：

- 头文件能一起编译
- `IRemoteObject` death recipient
- `Publish()` / `GetSystemAbility()` / `CheckSystemAbility()`
- `LoadSystemAbility()` success/fail 回调
- `iface_cast<T>()`

## 运行方式

单独运行子仓库测试：

```bash
cmake -S third_party/ohos_sa_mock -B third_party/ohos_sa_mock/build
cmake --build third_party/ohos_sa_mock/build
ctest --test-dir third_party/ohos_sa_mock/build --output-on-failure
```

如果它是作为父仓库子目录构建，也可以在父仓库 build 目录里只跑相关测试：

```bash
ctest --test-dir build --output-on-failure -R '^ohos_sa_mock_'
```

## Demo 验证

```bash
cmake --build third_party/ohos_sa_mock/build --target ohos_sa_mock_demo
./third_party/ohos_sa_mock/build/ohos_sa_mock_demo
```

预期输出至少包含：

```text
get: pong-from-sa
load: pong-from-sa
death_recipient_called: true
```
