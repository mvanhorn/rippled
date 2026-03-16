# Telemetry Workload Tools

Synthetic workload generation and validation tools for rippled's OpenTelemetry telemetry stack. These tools validate that all spans, metrics, dashboards, and log-trace correlation work end-to-end under controlled load.

## Quick Start

```bash
# Build rippled with telemetry enabled
conan install . --build=missing -o telemetry=True
cmake --preset default -Dtelemetry=ON
cmake --build --preset default

# Run full validation (starts everything, runs load, validates)
docker/telemetry/workload/run-full-validation.sh --xrpld .build/xrpld

# Cleanup when done
docker/telemetry/workload/run-full-validation.sh --cleanup
```

## Architecture

The validation suite runs a 2-node rippled cluster as local processes alongside
a Docker Compose telemetry stack. The 2-node setup is sufficient for exercising
consensus, peer-to-peer spans (proposals, validations), and all metric pipelines,
while keeping CI resource usage manageable.

```
run-full-validation.sh (orchestrator)
  |
  |-- docker-compose.workload.yaml
  |     |-- otel-collector (traces via OTLP + StatsD receiver)
  |     |-- jaeger (trace search API)
  |     |-- prometheus (metrics scraping)
  |     |-- grafana (dashboards, provisioned automatically)
  |
  |-- generate-validator-keys.sh
  |     -> validator-keys.json, validators.txt
  |
  |-- 2x xrpld nodes (local processes, full telemetry)
  |     - Each node: [telemetry] enabled=1, trace_rpc/consensus/transactions
  |     - [signing_support] true (server-side signing for tx_submitter)
  |     - Peer discovery via [ips] (not [ips_fixed]) for active peer counts
  |
  |-- rpc_load_generator.py (WebSocket RPC traffic)
  |-- tx_submitter.py (transaction diversity)
  |
  |-- validate_telemetry.py (pass/fail checks)
  |     -> validation-report.json
  |
  |-- benchmark.sh (baseline vs telemetry comparison)
        -> benchmark-report-*.md
```

## Tools Reference

### run-full-validation.sh

Orchestrates the complete validation pipeline. Starts the telemetry stack, starts a multi-node rippled cluster, generates load, and validates the results.

```bash
# Full validation with defaults
./run-full-validation.sh --xrpld /path/to/xrpld

# Custom load parameters
./run-full-validation.sh --xrpld /path/to/xrpld \
    --rpc-rate 100 --rpc-duration 300 \
    --tx-tps 10 --tx-duration 300

# Include performance benchmarks
./run-full-validation.sh --xrpld /path/to/xrpld --with-benchmark

# Skip Loki checks (if Phase 8 not deployed)
./run-full-validation.sh --xrpld /path/to/xrpld --skip-loki
```

### rpc_load_generator.py

Generates RPC traffic matching realistic production distribution:

- 40% health checks (server_info, fee)
- 30% wallet queries (account_info, account_lines, account_objects)
- 15% explorer queries (ledger, ledger_data)
- 10% transaction lookups (tx, account_tx)
- 5% DEX queries (book_offers, amm_info)

```bash
# Basic usage
python3 rpc_load_generator.py --endpoints ws://localhost:6006 --rate 50 --duration 120

# Multiple endpoints (round-robin)
python3 rpc_load_generator.py \
    --endpoints ws://localhost:6006 ws://localhost:6007 \
    --rate 100 --duration 300

# Custom weights
python3 rpc_load_generator.py --endpoints ws://localhost:6006 \
    --weights '{"server_info": 80, "account_info": 20}'
```

### tx_submitter.py

Submits diverse transaction types to exercise the full span and metric surface.
Uses rippled's **native WebSocket command format** (`{"command": ...}`) rather
than JSON-RPC format. The response payload is inside the `"result"` key, with
`"status"` at the top level.

Supported transaction types:

- Payment (XRP transfers) — exercises `tx.process`, `tx.receive`, `tx.apply`
- OfferCreate / OfferCancel (DEX activity)
- TrustSet (trust line creation)
- NFTokenMint / NFTokenCreateOffer (NFT activity)
- EscrowCreate / EscrowFinish (escrow lifecycle)
- AMMCreate / AMMDeposit (AMM pool operations)

Requires `[signing_support] true` in the node config for server-side signing.

```bash
# Basic usage
python3 tx_submitter.py --endpoint ws://localhost:6006 --tps 5 --duration 120

# Custom mix
python3 tx_submitter.py --endpoint ws://localhost:6006 \
    --weights '{"Payment": 60, "OfferCreate": 20, "TrustSet": 20}'
```

### validate_telemetry.py

Automated validation that all expected telemetry data exists:

- **Span validation**: All span types from `expected_spans.json` with required attributes and parent-child hierarchies
- **Metric validation**: SpanMetrics, StatsD gauges/counters/histograms, Phase 9 OTLP metrics from `expected_metrics.json`
- **Log-trace correlation**: trace_id/span_id in Loki logs (optional, requires Loki)
- **Dashboard validation**: All 10 Grafana dashboards load with panels

Metrics in `expected_metrics.json` support two tiers:

- `"metrics"`: Required — absence causes a FAIL
- `"optional_metrics"`: Environment-dependent — absence produces a PASS with a warning (e.g., `ios_latency` only fires when I/O thread latency >= 10ms)

```bash
# Run all validations
python3 validate_telemetry.py --report /tmp/report.json

# Skip Loki checks
python3 validate_telemetry.py --skip-loki --report /tmp/report.json
```

### benchmark.sh

Compares baseline (no telemetry) vs telemetry-enabled performance:

```bash
./benchmark.sh --xrpld /path/to/xrpld --duration 300
```

Thresholds (configurable via environment):

| Metric            | Threshold | Env Variable                |
| ----------------- | --------- | --------------------------- |
| CPU overhead      | < 3%      | BENCH_CPU_OVERHEAD_PCT      |
| Memory overhead   | < 5MB     | BENCH_MEM_OVERHEAD_MB       |
| RPC p99 latency   | < 2ms     | BENCH_RPC_LATENCY_IMPACT_MS |
| Throughput impact | < 5%      | BENCH_TPS_IMPACT_PCT        |
| Consensus impact  | < 1%      | BENCH_CONSENSUS_IMPACT_PCT  |

## Reading Validation Reports

The validation report (`validation-report.json`) is structured as:

```json
{
  "summary": {
    "total": 45,
    "passed": 42,
    "failed": 3,
    "all_passed": false
  },
  "checks": [
    {
      "name": "span.rpc.request",
      "category": "span",
      "passed": true,
      "message": "rpc.request: 15 traces found",
      "details": { "trace_count": 15 }
    }
  ]
}
```

Categories:

- **span**: Span type existence and attribute validation
- **metric**: Prometheus metric existence
- **log**: Log-trace correlation checks
- **dashboard**: Grafana dashboard accessibility

## CI Integration

The validation runs as a GitHub Actions workflow (`.github/workflows/telemetry-validation.yml`):

- Triggered manually or on pushes to telemetry branches
- Builds rippled, starts the full stack, runs load, validates
- Uploads reports as artifacts
- Posts summary to PR

## Configuration Files

| File                    | Purpose                                                       |
| ----------------------- | ------------------------------------------------------------- |
| `expected_spans.json`   | Span inventory (names, attributes, hierarchies, config flags) |
| `expected_metrics.json` | Metric inventory with `metrics` and `optional_metrics` lists  |
| `test_accounts.json`    | Test account roles (keys generated at runtime)                |
| `requirements.txt`      | Python dependencies                                           |

### expected_metrics.json Format

```json
{
  "category_name": {
    "description": "Human-readable description.",
    "metrics": ["required_metric_1", "required_metric_2"],
    "optional_metrics": ["env_dependent_metric"],
    "optional_note": "Explanation of why these metrics may not fire."
  }
}
```

### expected_spans.json Format

Each span entry defines its name, category, parent (for hierarchy validation),
required attributes, and the `config_flag` that must be enabled:

```json
{
  "name": "rpc.request",
  "category": "rpc",
  "parent": null,
  "required_attributes": ["rpc.method", "rpc.grpc.status_code"],
  "config_flag": "trace_rpc"
}
```

## Node Configuration Notes

The orchestrator (`run-full-validation.sh`) generates node configs with:

- `[telemetry] enabled=1` with all trace categories (`trace_rpc`, `trace_consensus`, `trace_transactions`)
- `[signing_support] true` — required for `tx_submitter.py` to submit signed transactions via WebSocket
- `[ips]` (not `[ips_fixed]`) — ensures peer connections are counted in `Peer_Finder_Active_Inbound/Outbound_Peers` metrics (fixed peers are excluded from these counters by design)
