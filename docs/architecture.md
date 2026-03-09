# MemRPC Architecture

`memrpc` is a C++17 shared-memory RPC layer for splitting an in-process engine API into a client process and an engine process.

Core design points:

- one shared-memory region per client/engine session
- one high-priority request ring
- one normal-priority request ring
- one response ring
- fixed-size slot payloads for request and response data
- `eventfd` notification in both directions
- synchronous client API over an asynchronous transport

The client keeps the old blocking `Scan()` shape, but internally:

1. reserves a slot
2. writes the request payload
3. pushes a request entry into the selected ring
4. signals the matching request `eventfd`
5. waits for the dispatcher thread to deliver the response

The server process:

1. drains high-priority requests first
2. dispatches high and normal work into separate worker pools
3. calls the business `IScanHandler`
4. writes the result back to the slot
5. pushes a response entry and signals the response `eventfd`

This layout keeps transport-specific bootstrap logic outside the core runtime. HarmonyOS SA should only be responsible for process startup, service discovery, and descriptor exchange.

Current scope:

- robust shared-memory mutexes recover owner death as `kPeerDisconnected`
- engine death callbacks invalidate the current session immediately
- the next `Scan()` lazily calls `StartEngine()` + `Connect()` to attach a fresh session
- requests already published to the old shared-memory ring are failed as `kPeerDisconnected`
- requests not yet published stay local and are retried by the new `Scan()` attempt
- upper layers are still responsible for final restart policy and any re-scan of requests that may already have reached the old engine
- Linux demo and fake-SA development mode can use `fork()` to stand up the engine side quickly
- HarmonyOS production mode is expected to use `init` + `GetSystemAbility` / `LoadSystemAbility`, with the same shared-memory transport underneath
