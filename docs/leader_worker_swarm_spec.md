# Leader-Worker Swarm Spec (Design Only)

## 1. Scope

This document defines a design-only specification for distributed security checks after remote updates.
No firmware implementation is included in this phase.

## 2. Goals

- Trigger and control security checks via Telegram.
- Keep ESP32-S3 memory usage predictable under low-RAM constraints.
- Execute one job at a time with deterministic timeout and bounded output.
- Preserve security via allowlists and strict command indirection (`tool_id`).

## 3. Non-Goals

- No arbitrary `cmd` execution from user input.
- No direct multi-session SSH fan-out from a single ESP32.
- No feature implementation in this phase.

## 4. High-Level Architecture

- `Leader ESP32`
- Receives Telegram requests.
- Validates admin/user permissions.
- Validates request against allowlist config.
- Enforces single active job lock.
- Dispatches job to one `Worker ESP32`.
- Returns final result to Telegram.

- `Worker ESP32`
- Accepts one assigned job.
- Tracks run state and heartbeat.
- Delegates SSH execution to external executor service.
- Returns normalized result payload.

- `External Executor` (Linux/Container recommended)
- Performs actual SSH session and remote command execution.
- Enforces timeout, output cap, and process termination.
- Returns final `{status, exit_code, stdout, stderr, truncated, duration_ms}`.

## 5. Configuration Model (Allowlist)

Configuration file remains JSON for parser simplicity and reliability on ESP32.

```json
{
  "revision": 13,
  "hosts": [
    {"host_id":"web1","ip":"203.0.113.10","port":22,"user":"secchk","key_id":"k1","enabled":true}
  ],
  "tools": [
    {
      "tool_id":"health_basic",
      "command_template":"check_health.sh --profile {profile}",
      "allowed_args":{"profile":["quick","full"]},
      "max_timeout_ms":30000,
      "enabled":true
    }
  ],
  "bindings": [
    {"tool_id":"health_basic","host_ids":["web1"]}
  ]
}
```

Validation gates:

- `host_id` exists and enabled.
- `tool_id` exists and enabled.
- `tool_id -> host_id` binding exists.
- args match tool schema.
- `timeout_ms` clamped to tool max.

## 6. Request Contract

External request contract (from Telegram command parser to Leader internal queue):

```json
{
  "request_id":"uuid",
  "host_id":"web1",
  "tool_id":"health_basic",
  "timeout_ms":15000,
  "args":{"profile":"quick"}
}
```

Command input policy:

- Do not accept raw `cmd` from user.
- Only `tool_id` plus validated args.

## 7. Execution Constraints

- Single active job globally (`IDLE` or `RUNNING`).
- Timeout is mandatory.
- On timeout, force terminate and return `status=TIMEOUT`.
- Combined stdout/stderr cap: `10KB`.
- If exceeded, drop oldest bytes (ring-buffer behavior), set `truncated=true`.
- Always run non-PTY style command execution equivalent to `ssh -T`.
- On finish, free and zeroize all job-local resources.

## 8. Runtime State Machine

- `IDLE`
- `RUNNING`
- `FINISHING`
- `CLEANUP`
- back to `IDLE`

State expectations:

- `RUNNING`: only `/status` and `/cancel` are accepted as control commands.
- other Telegram requests get busy notice and are not queued.

Busy notice message:

- "현재 보안 점검 작업 실행 중입니다. 중단 요청(/cancel) 없으면 완료 후 다시 요청해 주세요."

## 9. Leader-Worker Messaging (Proposed)

Transport can be MQTT QoS1 or lightweight internal bus over existing channel.

- `job.submit`
```json
{"type":"job.submit","job_id":"uuid","worker_id":"w1","payload":{...},"lease_ms":20000}
```

- `job.accepted`
```json
{"type":"job.accepted","job_id":"uuid","worker_id":"w1","ts":1730000000}
```

- `job.heartbeat`
```json
{"type":"job.heartbeat","job_id":"uuid","worker_id":"w1","elapsed_ms":5000}
```

- `job.result`
```json
{
  "type":"job.result",
  "job_id":"uuid",
  "worker_id":"w1",
  "status":"OK|TIMEOUT|CANCELLED|SSH_ERROR|AUTH_ERROR|INTERNAL_ERROR",
  "exit_code":0,
  "duration_ms":1234,
  "stdout":"...",
  "stderr":"...",
  "truncated":false
}
```

- `job.cancel`
```json
{"type":"job.cancel","job_id":"uuid","reason":"user_request"}
```

## 10. Telegram Operations

- `/run <host_id> <tool_id> [timeout_sec] [k=v ...]`
- `/status`
- `/cancel`
- `/allowlist_export`
- `/allowlist_validate` (JSON file upload)
- `/allowlist_apply` (JSON file upload)

Allowlist update flow:

1. Export current file and revision.
2. Edit offline.
3. Validate only.
4. Apply atomically (`.tmp` + rename).
5. Log revision and diff summary.

## 11. Security Requirements

- Admin-only allowlist mutation commands.
- `key_id` indirection only (no PEM upload).
- Host key pinning at executor side.
- Full audit log (`who`, `when`, `host_id`, `tool_id`, `status`, `duration`).
- Reject unknown fields in job payload to reduce parser ambiguity.

## 12. Open Items

- Select transport: MQTT vs existing internal channel.
- Decide worker assignment strategy: static vs least-busy.
- Define retry policy for worker crash mid-job.
- Define retention period for job/audit logs.

## 13. TODO Status For This Phase

- [x] Leader-worker swarm architecture and contracts documented.
- [x] Runtime constraints agreed (single job, timeout, 10KB cap, cleanup, busy handling).
- [ ] Firmware implementation.
- [ ] Executor implementation.
- [ ] Telegram command integration.
