# Phase 10: Synthetic Workload Generation & Telemetry Validation — Task List

> **Status**: In Progress
>
> **Goal**: Build tools that generate realistic XRPL traffic to validate the full Phases 1-9 telemetry stack end-to-end — all spans, attributes, metrics, dashboards, and log-trace correlation — under controlled load.
>
> **Scope**: Python/shell test harness + multi-node docker-compose environment + automated validation scripts + performance benchmarks.
>
> **Branch**: `pratik/otel-phase10-workload-validation` (from `pratik/otel-phase9-metric-gap-fill`)
>
> **Depends on**: Phase 9 (internal metric gap fill) — validates the full metric surface

### Related Plan Documents

| Document                                                             | Relevance                                                       |
| -------------------------------------------------------------------- | --------------------------------------------------------------- |
| [06-implementation-phases.md](./06-implementation-phases.md)         | Phase 10 plan: motivation, architecture, exit criteria (§6.8.3) |
| [09-data-collection-reference.md](./09-data-collection-reference.md) | Defines the full inventory of spans/metrics to validate         |
| [Phase9_taskList.md](./Phase9_taskList.md)                           | Prerequisite — all internal metrics must be emitting            |

### Why This Phase Exists

Before Phases 1-9 can be considered production-ready, we need proof that:

1. All 17 spans fire with correct attributes under real transaction workloads
2. All 255+ StatsD metrics + ~50 Phase 9 metrics appear in Prometheus with non-zero values
3. Log-trace correlation (Phase 8) produces clickable trace_id links in Loki
4. All 12 Grafana dashboards render meaningful data (no empty panels)
5. Performance overhead stays within bounds (< 3% CPU, < 5MB memory)
6. The telemetry stack survives sustained load without data loss or queue backpressure

---

## Task 10.1: Multi-Node Test Harness

**Objective**: Create a docker-compose environment with validator nodes that produces real consensus rounds.

**Implementation notes**:

- Uses a **6-node** validator cluster for full consensus coverage and per-node metric differentiation.
- Nodes run as **local processes** (not in containers) — the Docker Compose stack only hosts telemetry backends.
- `run-full-validation.sh` orchestrates: starts telemetry stack → generates validator keys → starts nodes → runs workloads → validates → cleans up.
- Node config requires:
  - `[signing_support] true` for server-side transaction signing
  - `[ips]` (not `[ips_fixed]`) so peer connections are counted in `Peer_Finder_Active_*` metrics (fixed peers are excluded from these counters by design in `Counts.h`)
  - All trace categories: `trace_rpc=1`, `trace_consensus=1`, `trace_transactions=1`

**Key files**:

- `docker/telemetry/docker-compose.workload.yaml` — OTel Collector, Jaeger, Prometheus, Grafana
- `docker/telemetry/workload/generate-validator-keys.sh` — generates keys and UNL for N nodes
- `docker/telemetry/workload/run-full-validation.sh` — main orchestrator

---

## Task 10.2: RPC Load Generator

**Objective**: Configurable tool that fires all traced RPC commands at controlled rates.

**What to do**:

- Create `docker/telemetry/workload/rpc_load_generator.py`:
  - Connects to one or more rippled WebSocket endpoints
  - Fires all RPC commands that have trace spans: `server_info`, `ledger`, `tx`, `account_info`, `account_lines`, `fee`, `submit`, etc.
  - Configurable parameters: rate (RPS), duration, command distribution weights
  - Injects `traceparent` HTTP headers to test W3C context propagation
  - Logs progress and errors to stdout

- Command distribution should match realistic production ratios:
  - 40% `server_info` / `fee` (health checks)
  - 30% `account_info` / `account_lines` / `account_objects` (wallet queries)
  - 15% `ledger` / `ledger_data` (explorer queries)
  - 10% `tx` / `account_tx` (transaction lookups)
  - 5% `book_offers` / `amm_info` (DEX queries)

**Key files**:

- New: `docker/telemetry/workload/rpc_load_generator.py`
- New: `docker/telemetry/workload/requirements.txt`

---

## Task 10.3: Transaction Submitter

**Objective**: Generate diverse transaction types to exercise `tx.*` and `ledger.*` spans.

**Implementation notes**:

- Uses rippled's **native WebSocket command format** (`{"command": "submit", "secret": ..., "tx_json": {...}}`), not JSON-RPC format.
  - Response structure: `{"status": "success", "result": {"engine_result": "tesSUCCESS", ...}, "type": "response"}`
  - The `ws_request()` helper unwraps `result` so callers read fields directly.
  - Error responses have `"status": "error"` at the top level with `error` and `error_message` fields.
- Pre-funds test accounts from the genesis account (`rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh`).
- Requires `[signing_support] true` in node config for server-side signing.
- Supported transaction types: Payment, OfferCreate, OfferCancel, TrustSet, NFTokenMint, NFTokenCreateOffer, EscrowCreate, EscrowFinish, AMMCreate, AMMDeposit.
- Exercises spans: `tx.process` (local submit), `tx.receive` (peer propagation), `tx.apply` (ledger application).

**Key files**:

- `docker/telemetry/workload/tx_submitter.py`
- `docker/telemetry/workload/test_accounts.json` (test account role definitions)

---

## Task 10.4: Telemetry Validation Suite

**Objective**: Automated scripts that verify all expected telemetry data exists after a workload run.

**Implementation notes**:

- `validate_telemetry.py` runs **73 checks** and produces a JSON report.

  The 73 checks break down as:

  | Category             | Count | Source                                    |
  | -------------------- | ----- | ----------------------------------------- |
  | Service registration | 1     | Jaeger: `rippled` service exists          |
  | Span existence       | 17    | `expected_spans.json` — 17 span types     |
  | Span attributes      | 14    | Spans with `required_attributes`          |
  | Span hierarchies     | 2     | Parent-child relationships (1 skipped)    |
  | Span durations       | 1     | All spans > 0 and < 60 s                  |
  | Metric existence     | 26    | `expected_metrics.json` — 26 metric names |
  | Dashboard loads      | 12    | `expected_metrics.json` — 12 Grafana UIDs |

  **Span validation** (queries Jaeger API):
  - Lists all registered operations as diagnostics
  - Asserts 17 span names from `expected_spans.json` appear in traces
  - Validates required attributes on the 14 spans that define them
  - Validates 2 active parent-child span hierarchies (1 skipped — cross-thread)
  - Asserts all span durations are within bounds (> 0, < 60 s)

  **Metric validation** (queries Prometheus API):
  - Lists all metric names as diagnostics (helps debug naming issues)
  - All 26 metrics in `expected_metrics.json` must have > 0 Prometheus series — absence is a FAIL
  - Uses the Prometheus `/api/v1/series` endpoint (not instant queries) to avoid false negatives from stale gauges — beast::insight StatsD gauges only emit on value changes, so a gauge that stabilizes goes stale in Prometheus after ~5 minutes
  - Validates: 4 SpanMetrics, 6 StatsD gauges, 2 StatsD counters, 3 StatsD histograms, 4 overlay traffic, 7 Phase 9 OTLP metrics (nodestore, cache, txq, rpc_method, object_count, load_factor)

  **Dashboard validation**:
  - Queries Grafana API for each of the 10 dashboard UIDs
  - Asserts dashboards load and have panels

- Output: `validation-report.json` with per-check pass/fail, suitable for CI.

**Key files**:

- `docker/telemetry/workload/validate_telemetry.py`
- `docker/telemetry/workload/expected_spans.json` (span inventory with attributes and hierarchies)
- `docker/telemetry/workload/expected_metrics.json` (metric inventory — all required)

---

## Task 10.5: Performance Benchmark Suite

**Objective**: Measure CPU/memory/latency overhead of the telemetry stack.

**What to do**:

- Create `docker/telemetry/workload/benchmark.sh`:
  - **Baseline run**: Start cluster with `[telemetry] enabled=0`, run transaction workload for 5 minutes, record metrics
  - **Telemetry run**: Start cluster with full telemetry enabled, run identical workload, record metrics
  - **Comparison**: Calculate deltas for:
    - CPU usage (per-node average)
    - Memory RSS (per-node peak)
    - RPC p99 latency
    - Transaction throughput (TPS)
    - Consensus round time p95
    - Ledger close time p95

- Output: Markdown table comparing baseline vs. telemetry, with pass/fail against targets:
  - CPU overhead < 3%
  - Memory overhead < 5MB
  - RPC latency impact < 2ms p99
  - Throughput impact < 5%
  - Consensus impact < 1%

- Store results in `docker/telemetry/workload/benchmark-results/` for historical tracking.

**Key files**:

- New: `docker/telemetry/workload/benchmark.sh`
- New: `docker/telemetry/workload/collect_system_metrics.sh`

---

## Task 10.6: CI Integration

**Objective**: Wire the validation suite into CI for regression detection.

**What to do**:

- Create a CI workflow (GitHub Actions or equivalent) that:
  1. Builds rippled with `-DXRPL_ENABLE_TELEMETRY=ON`
  2. Starts the multi-node workload harness
  3. Runs the RPC load generator + transaction submitter for 2 minutes
  4. Runs the validation suite
  5. Runs the benchmark suite
  6. Fails the build if any validation check fails or benchmark exceeds thresholds
  7. Archives the validation report and benchmark results as artifacts

- This should be a separate workflow (not part of the main CI), triggered manually or on telemetry-related branch changes.

**Key files**:

- New: `.github/workflows/telemetry-validation.yml`
- New: `docker/telemetry/workload/run-full-validation.sh` (orchestrator script)

---

## Task 10.7: Documentation

**Objective**: Document the workload tools and validation process.

**What to do**:

- Create `docker/telemetry/workload/README.md`:
  - Quick start guide for running workload harness
  - Configuration options for load generator and tx submitter
  - How to read validation reports
  - How to run benchmarks and interpret results

- Update `docs/telemetry-runbook.md`:
  - Add "Validating Telemetry Stack" section
  - Add "Performance Benchmarking" section

- Update `OpenTelemetryPlan/09-data-collection-reference.md`:
  - Add "Validation" section with expected metric/span counts

---

## What "All 73 Checks" Means — Complete Enumeration

The validation suite (`validate_telemetry.py`) runs exactly **73 checks** grouped into 7 categories. Every item below is validated by name in CI. Nothing is optional — failure of any single check fails the entire suite.

### 1. Service Registration (1 check)

| #   | Check                              | Backend                |
| --- | ---------------------------------- | ---------------------- |
| 1   | `rippled` service exists in Jaeger | Jaeger `/api/services` |

### 2. Span Existence (17 checks)

Each span name must appear at least once in Jaeger traces for the `rippled` service.

| #   | Span Name                   | Category    | Config Flag            |
| --- | --------------------------- | ----------- | ---------------------- |
| 2   | `rpc.request`               | RPC         | `trace_rpc=1`          |
| 3   | `rpc.process`               | RPC         | `trace_rpc=1`          |
| 4   | `rpc.ws_message`            | RPC         | `trace_rpc=1`          |
| 5   | `rpc.command.*`             | RPC         | `trace_rpc=1`          |
| 6   | `tx.process`                | Transaction | `trace_transactions=1` |
| 7   | `tx.receive`                | Transaction | `trace_transactions=1` |
| 8   | `tx.apply`                  | Transaction | `trace_transactions=1` |
| 9   | `consensus.proposal.send`   | Consensus   | `trace_consensus=1`    |
| 10  | `consensus.ledger_close`    | Consensus   | `trace_consensus=1`    |
| 11  | `consensus.accept`          | Consensus   | `trace_consensus=1`    |
| 12  | `consensus.validation.send` | Consensus   | `trace_consensus=1`    |
| 13  | `consensus.accept.apply`    | Consensus   | `trace_consensus=1`    |
| 14  | `ledger.build`              | Ledger      | `trace_ledger=1`       |
| 15  | `ledger.validate`           | Ledger      | `trace_ledger=1`       |
| 16  | `ledger.store`              | Ledger      | `trace_ledger=1`       |
| 17  | `peer.proposal.receive`     | Peer        | `trace_peer=1`         |
| 18  | `peer.validation.receive`   | Peer        | `trace_peer=1`         |

### 3. Span Attribute Validation (14 checks)

14 of the 17 spans define `required_attributes`. Each check asserts all listed attributes are present on at least one instance of that span.

| #   | Span Name                   | Required Attributes                                                                                |
| --- | --------------------------- | -------------------------------------------------------------------------------------------------- |
| 19  | `rpc.command.*`             | `xrpl.rpc.command`, `xrpl.rpc.version`, `xrpl.rpc.role`, `xrpl.rpc.status`, `xrpl.rpc.duration_ms` |
| 20  | `tx.process`                | `xrpl.tx.hash`, `xrpl.tx.local`, `xrpl.tx.path`                                                    |
| 21  | `tx.receive`                | `xrpl.peer.id`, `xrpl.tx.hash`, `xrpl.tx.suppressed`, `xrpl.tx.status`                             |
| 22  | `tx.apply`                  | `xrpl.ledger.seq`, `xrpl.ledger.tx_count`, `xrpl.ledger.tx_failed`                                 |
| 23  | `consensus.proposal.send`   | `xrpl.consensus.round`                                                                             |
| 24  | `consensus.ledger_close`    | `xrpl.consensus.ledger.seq`, `xrpl.consensus.mode`                                                 |
| 25  | `consensus.accept`          | `xrpl.consensus.proposers`                                                                         |
| 26  | `consensus.validation.send` | `xrpl.consensus.ledger.seq`, `xrpl.consensus.proposing`                                            |
| 27  | `consensus.accept.apply`    | `xrpl.consensus.close_time`, `xrpl.consensus.ledger.seq`                                           |
| 28  | `ledger.build`              | `xrpl.ledger.seq`, `xrpl.ledger.tx_count`, `xrpl.ledger.tx_failed`                                 |
| 29  | `ledger.validate`           | `xrpl.ledger.seq`, `xrpl.ledger.validations`                                                       |
| 30  | `ledger.store`              | `xrpl.ledger.seq`                                                                                  |
| 31  | `peer.proposal.receive`     | `xrpl.peer.id`, `xrpl.peer.proposal.trusted`                                                       |
| 32  | `peer.validation.receive`   | `xrpl.peer.id`, `xrpl.peer.validation.trusted`                                                     |

### 4. Span Parent-Child Hierarchies (2 checks)

| #   | Parent         | Child           | Status  | Notes                                                                                                                        |
| --- | -------------- | --------------- | ------- | ---------------------------------------------------------------------------------------------------------------------------- |
| 33  | `rpc.process`  | `rpc.command.*` | Active  | Same thread — always valid                                                                                                   |
| 34  | `ledger.build` | `tx.apply`      | Active  | Same thread — always valid                                                                                                   |
| --  | `rpc.request`  | `rpc.process`   | Skipped | Cross-thread: `onRequest` posts to JobQueue coroutine. Span context not propagated across thread boundary. Requires C++ fix. |

### 5. Span Duration Bounds (1 check)

| #   | Check                          | Criteria                                 |
| --- | ------------------------------ | ---------------------------------------- |
| 35  | All spans have valid durations | Every span duration > 0 and < 60 seconds |

### 6. Metric Existence (26 checks)

Each metric name must have > 0 series in Prometheus (queried via `/api/v1/series` to avoid stale-gauge false negatives).

| #   | Metric Name                                        | Category         | Source                               |
| --- | -------------------------------------------------- | ---------------- | ------------------------------------ |
| 36  | `traces_span_metrics_calls_total`                  | SpanMetrics      | OTel Collector spanmetrics connector |
| 37  | `traces_span_metrics_duration_milliseconds_bucket` | SpanMetrics      | OTel Collector spanmetrics connector |
| 38  | `traces_span_metrics_duration_milliseconds_count`  | SpanMetrics      | OTel Collector spanmetrics connector |
| 39  | `traces_span_metrics_duration_milliseconds_sum`    | SpanMetrics      | OTel Collector spanmetrics connector |
| 40  | `rippled_LedgerMaster_Validated_Ledger_Age`        | StatsD Gauge     | beast::insight via StatsD UDP        |
| 41  | `rippled_LedgerMaster_Published_Ledger_Age`        | StatsD Gauge     | beast::insight via StatsD UDP        |
| 42  | `rippled_State_Accounting_Full_duration`           | StatsD Gauge     | beast::insight via StatsD UDP        |
| 43  | `rippled_Peer_Finder_Active_Inbound_Peers`         | StatsD Gauge     | beast::insight via StatsD UDP        |
| 44  | `rippled_Peer_Finder_Active_Outbound_Peers`        | StatsD Gauge     | beast::insight via StatsD UDP        |
| 45  | `rippled_jobq_job_count`                           | StatsD Gauge     | beast::insight via StatsD UDP        |
| 46  | `rippled_rpc_requests_total`                       | StatsD Counter   | beast::insight via StatsD UDP        |
| 47  | `rippled_ledger_fetches_total`                     | StatsD Counter   | beast::insight via StatsD UDP        |
| 48  | `rippled_rpc_time`                                 | StatsD Histogram | beast::insight via StatsD UDP        |
| 49  | `rippled_rpc_size`                                 | StatsD Histogram | beast::insight via StatsD UDP        |
| 50  | `rippled_ios_latency`                              | StatsD Histogram | beast::insight via StatsD UDP        |
| 51  | `rippled_total_Bytes_In`                           | Overlay Traffic  | beast::insight via StatsD UDP        |
| 52  | `rippled_total_Bytes_Out`                          | Overlay Traffic  | beast::insight via StatsD UDP        |
| 53  | `rippled_total_Messages_In`                        | Overlay Traffic  | beast::insight via StatsD UDP        |
| 54  | `rippled_total_Messages_Out`                       | Overlay Traffic  | beast::insight via StatsD UDP        |
| 55  | `rippled_nodestore_state`                          | Phase 9 OTLP     | MetricsRegistry via OTLP             |
| 56  | `rippled_cache_metrics`                            | Phase 9 OTLP     | MetricsRegistry via OTLP             |
| 57  | `rippled_txq_metrics`                              | Phase 9 OTLP     | MetricsRegistry via OTLP             |
| 58  | `rippled_rpc_method_started_total`                 | Phase 9 OTLP     | MetricsRegistry via OTLP             |
| 59  | `rippled_rpc_method_finished_total`                | Phase 9 OTLP     | MetricsRegistry via OTLP             |
| 60  | `rippled_object_count`                             | Phase 9 OTLP     | MetricsRegistry via OTLP             |
| 61  | `rippled_load_factor_metrics`                      | Phase 9 OTLP     | MetricsRegistry via OTLP             |

### 7. Dashboard Loads (12 checks)

Each Grafana dashboard must load successfully and contain at least one panel.

| #   | Dashboard UID                   | Dashboard Name                          |
| --- | ------------------------------- | --------------------------------------- |
| 62  | `rippled-rpc-perf`              | RPC Performance (OTel)                  |
| 63  | `rippled-transactions`          | Transaction Overview                    |
| 64  | `rippled-consensus`             | Consensus Health                        |
| 65  | `rippled-ledger-ops`            | Ledger Operations                       |
| 66  | `rippled-peer-net`              | Peer Network                            |
| 67  | `rippled-fee-market`            | Fee Market & TxQ                        |
| 68  | `rippled-job-queue`             | Job Queue Analysis                      |
| 69  | `rippled-system-node-health`    | Node Health (System Metrics)            |
| 70  | `rippled-system-network`        | Network Traffic (System Metrics)        |
| 71  | `rippled-system-rpc`            | RPC & Pathfinding (System Metrics)      |
| 72  | `rippled-system-overlay-detail` | Overlay Traffic Detail (System Metrics) |
| 73  | `rippled-system-ledger-sync`    | Ledger Data & Sync (System Metrics)     |

---

## Current Status: What Is Working vs. What Is Not

### Working (validated in CI run 23144741908 — 73/73 PASS)

1. **All 17 spans fire** with correct attributes under real workload (RPC + transaction + consensus)
2. **All 26 metrics exist** in Prometheus with non-zero series counts and per-node `exported_instance` labels (Node-1 through Node-6)
3. **All 12 Grafana dashboards** load and render panels (including Phase 9 Fee Market & TxQ, Job Queue Analysis)
4. **All 14 span attribute checks** pass, including `tx.receive` (fixed: default attributes on span creation)
5. **Both parent-child hierarchies** validate (`rpc.process` -> `rpc.command.*`, `ledger.build` -> `tx.apply`)
6. **All span durations** are within bounds (> 0, < 60 s)
7. **RPC load generator** fires 11 command types with < 50% error rate (native WS format)
8. **Transaction submitter** generates 10 transaction types at configurable TPS
9. **6-node validator cluster** starts and reaches consensus; all nodes emit distinct per-node metrics
10. **CI workflow** (`telemetry-validation.yml`) runs on push to `pratik/otel-phase10-*` branches and on `workflow_dispatch`
11. **Validation report** is JSON with exit codes, suitable for CI gating
12. **MetricsRegistry metrics** carry `rippled_` prefix and Resource attributes (`service.name`, `service.instance.id`) for Prometheus per-node filtering
13. **beast::insight OTel metrics** carry `service_instance_id` from `[insight]` config for per-node `exported_instance` labels

### Not Working / Not Available in CI / Not Implemented Yet

1. **Performance benchmark suite** (`benchmark.sh`, `collect_system_metrics.sh`) — **not implemented**. Task 10.5 is not started. The exit criterion "Benchmark shows < 3% CPU overhead, < 5MB memory overhead" is **not met**.
2. **`rpc.request` -> `rpc.process` parent-child hierarchy** — **skipped** (not validated). Cross-thread span context propagation is broken: `onRequest` posts a coroutine to the JobQueue for `processRequest`, but the span context is not forwarded through the `std::function` lambda. Requires a C++ fix to capture and inject the parent span into the coroutine.
3. **Log-trace correlation validation** (Phase 8 Loki `trace_id` links) — **not included** in the 71 checks. The validation suite does not query Loki. This was listed in "Why This Phase Exists" item 3 but is not covered by the current validation.
4. **Full StatsD metric coverage** — the validation checks 26 representative metrics, not the full 255+ beast::insight StatsD metrics. Covering all 255+ would require a complete metric enumeration and significantly longer workload runs to trigger every code path.
5. **Sustained load / backpressure testing** — listed in "Why This Phase Exists" item 6 ("telemetry stack survives sustained load without data loss") but **not implemented**. The current workload runs for ~2 minutes, not long enough to test queue saturation.
6. **`docs/telemetry-runbook.md` updates** — Task 10.7 mentions adding "Validating Telemetry Stack" and "Performance Benchmarking" sections. The runbook has **not been updated**.
7. **`09-data-collection-reference.md` updates** — Task 10.7 mentions adding a "Validation" section with expected metric/span counts. This has **not been updated**.

---

## Exit Criteria

- [x] 6-node validator cluster starts and reaches consensus with per-node metric differentiation
- [x] RPC load generator fires all traced RPC commands at configurable rates
- [x] Transaction submitter generates 10 transaction types at configurable TPS
- [x] Validation suite confirms all spans, attributes, and metrics pass (73/73 checks)
- [x] All 12 Grafana dashboards render data with `$node` filter showing Node-1 through Node-6
- [ ] Benchmark shows < 3% CPU overhead, < 5MB memory overhead
- [x] CI workflow runs validation on telemetry branch changes
- [x] Validation report output is CI-parseable (JSON with exit codes)
