# Porting Guide

Existing in-process code usually looks like:

```cpp
ScanResult result = engine->Scan(request);
```

With `memrpc`, the caller keeps a synchronous shape:

```cpp
memrpc::EngineClient client(bootstrap);
client.Init();

memrpc::ScanResult result;
client.Scan(request, &result);
```

Migration guidance:

- keep business-facing request/result structs close to the old API
- move engine implementation behind `IScanHandler`
- keep process lifecycle and restart policy in the upper layer
- let the transport return explicit status codes like `kQueueTimeout`, `kExecTimeout`, and `kPeerDisconnected`
- on Linux, you can use the fake SA adapter or the `fork()` demo path during development
- on HarmonyOS, replace only the bootstrap implementation so process startup comes from `init` and service lookup comes from `GetSystemAbility` / `LoadSystemAbility`
- when SA reports engine death, the current `Scan()` calls waiting on the old session fail immediately with `kPeerDisconnected`
- the next `Scan()` lazily recreates the session through bootstrap and continues on the new shared-memory handles

Recommended wrapper pattern:

- old facade class owns `memrpc::EngineClient`
- old `Scan()` method translates business request structs into `memrpc::ScanRequest`
- upper layer decides whether to restart the engine process and whether to resubmit a failed scan
