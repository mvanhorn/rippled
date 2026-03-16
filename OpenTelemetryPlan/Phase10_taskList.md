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

1. All 16 spans fire with correct attributes under real transaction workloads
2. All 255+ StatsD metrics + ~50 Phase 9 metrics appear in Prometheus with non-zero values
3. Log-trace correlation (Phase 8) produces clickable trace_id links in Loki
4. All 10 Grafana dashboards render meaningful data (no empty panels)
5. Performance overhead stays within bounds (< 3% CPU, < 5MB memory)
6. The telemetry stack survives sustained load without data loss or queue backpressure

---

## Task 10.1: Multi-Node Test Harness

**Objective**: Create a docker-compose environment with validator nodes that produces real consensus rounds.

**Implementation notes**:

- Uses a **2-node** validator cluster (sufficient for consensus + peer spans, minimizes CI resources).
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

- `validate_telemetry.py` runs all checks and produces a JSON report.

  **Span validation** (queries Jaeger API):
  - Lists all registered operations as diagnostics
  - Asserts span names from `expected_spans.json` appear in traces
  - Validates required attributes per span type
  - Validates parent-child span hierarchies
  - Asserts all span durations are within bounds (> 0)

  **Metric validation** (queries Prometheus API):
  - Lists all metric names as diagnostics (helps debug naming issues)
  - Every metric in `expected_metrics.json` must have > 0 Prometheus series — absence is a FAIL
  - Validates: SpanMetrics, StatsD gauges/counters/histograms, overlay traffic, Phase 9 OTLP metrics (nodestore, cache, txq, rpc_method, object_count, load_factor)

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

## Exit Criteria

- [x] 2-node validator cluster starts and reaches consensus
- [x] RPC load generator fires all traced RPC commands at configurable rates
- [x] Transaction submitter generates 10 transaction types at configurable TPS
- [ ] Validation suite confirms all spans, attributes, and metrics pass
- [ ] All 10 Grafana dashboards render data
- [ ] Benchmark shows < 3% CPU overhead, < 5MB memory overhead
- [x] CI workflow runs validation on telemetry branch changes
- [x] Validation report output is CI-parseable (JSON with exit codes)
