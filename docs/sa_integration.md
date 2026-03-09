# HarmonyOS SA Integration

`memrpc` keeps SA-specific logic in the bootstrap layer.

Platform split:

- Linux development/demo: `memrpc::SaBootstrapChannel` can be treated as a fake SA adapter backed by the POSIX demo bootstrap. It uses the same shared-memory and `eventfd` transport and is compatible with a `fork()`-based demo process model.
- HarmonyOS production: the engine process is expected to be started by `init`, not by `fork()`. Service discovery and wake-up should be done through `GetSystemAbility` and `LoadSystemAbility`.

Expected SA responsibilities:

- start or locate the engine service
- exchange the shared-memory file descriptor
- exchange the request/response `eventfd` descriptors
- report session replacement after engine restart

Integration pattern:

1. implement `memrpc::IBootstrapChannel`
2. create the shared-memory session on the side that owns process startup
3. transfer `BootstrapHandles` through SA
4. feed the received handles into `EngineClient` and `EngineServer`

The transport core should not depend on SA headers. Keep SA glue in a small adapter library so Linux demo builds remain simple.

Suggested HarmonyOS mapping:

1. client side calls `GetSystemAbility` to obtain the service proxy
2. client side calls `LoadSystemAbility` when the engine service must be started or reloaded
3. the SA layer exchanges `BootstrapHandles`
4. the SA layer reports engine death through the bootstrap death callback
5. `EngineClient` lazily reconnects on the next `Scan()` by calling `StartEngine()` + `Connect()` again

This repository currently includes `memrpc::SaBootstrapChannel` as a Linux fake-SA adapter for development. It is intentionally shaped so that a real HarmonyOS implementation can replace only the bootstrap internals without changing the transport core or business-facing client/server APIs.
