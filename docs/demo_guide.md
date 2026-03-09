# Demo Guide

Build:

```bash
cmake -S . -B build
cmake --build build
```

Run the dual-process demo:

```bash
./build/demo/memrpc_demo_dual_process
```

Expected output shape:

- one normal request returning `kClean`
- one high-priority request returning `kInfected`
- one slow request returning `kExecTimeout`

The demo uses `fork()` so the client and server run in separate processes while sharing the same shared-memory session and `eventfd` handles.

This is only the Linux development model. For HarmonyOS migration, keep the transport core unchanged and replace the bootstrap path with `GetSystemAbility` / `LoadSystemAbility` plus an `init`-managed engine process.
