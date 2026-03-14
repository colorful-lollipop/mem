# VES DFX / Recovery Closure Notes

## Final Layering

- `VesControlProxy`
  - owns VES heartbeat protocol and reply parsing
  - translates `VesHeartbeatReply` into generic `ChannelHealthResult`
  - can publish full VES heartbeat snapshots through an optional async side-channel
- `IBootstrapChannel`
  - exposes `CheckHealth(expectedSessionId)`
  - keeps framework-facing health results generic
- `MemRpc::RpcClient`
  - watchdog owns timeout scan, channel health checks, and idle handling
  - `RequestExternalRecovery()` imports external health failures into the unified restart path
- `VesClient`
  - sets default recovery policy
  - exposes optional VES snapshot subscription for DFX
  - does not maintain a separate heartbeat loop
- supervisor / registry / harness
  - owns engine process reap / respawn
  - does not own replay-safe session recovery

## Semantics

- `CloseSession`
  - intentional close only
  - idle unload / manual shutdown
- `Restart`
  - fault recovery only
  - channel health failure / engine death / exec-timeout
- `EngineDeath` and heartbeat failures
  - reasons, not actions

## Current Validation

- `memrpc_bootstrap_health_check_test`
- `memrpc_rpc_client_external_recovery_test`
- `memrpc_rpc_client_timeout_watchdog_test`
- `virus_executor_service_heartbeat_test`
- `virus_executor_service_health_test`
- `virus_executor_service_health_subscription_test`
- `virus_executor_service_recovery_reason_test`
- `virus_executor_service_policy_test`
