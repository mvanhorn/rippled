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

```
run-full-validation.sh (orchestrator)
  |
  |-- docker-compose.workload.yaml
  |     |-- otel-collector (traces + StatsD)
  |     |-- jaeger (trace search)
  |     |-- tempo (trace storage)
  |     |-- prometheus (metrics)
  |     |-- loki (log aggregation)
  |     |-- grafana (dashboards)
  |
  |-- generate-validator-keys.sh
  |     -> validator-keys.json, validators.txt
  |
  |-- 5x xrpld nodes (local processes, full telemetry)
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

Submits diverse transaction types to exercise the full span and metric surface:

- Payment (XRP transfers)
- OfferCreate / OfferCancel (DEX activity)
- TrustSet (trust line creation)
- NFTokenMint / NFTokenCreateOffer (NFT activity)
- EscrowCreate / EscrowFinish (escrow lifecycle)
- AMMCreate / AMMDeposit (AMM pool operations)

```bash
# Basic usage
python3 tx_submitter.py --endpoint ws://localhost:6006 --tps 5 --duration 120

# Custom mix
python3 tx_submitter.py --endpoint ws://localhost:6006 \
    --weights '{"Payment": 60, "OfferCreate": 20, "TrustSet": 20}'
```

### validate_telemetry.py

Automated validation that all expected telemetry data exists:

- **Span validation**: All 16+ span types with required attributes
- **Metric validation**: SpanMetrics, StatsD, Phase 9 metrics
- **Log-trace correlation**: trace_id/span_id in Loki logs
- **Dashboard validation**: All 10 Grafana dashboards accessible

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

| File                           | Purpose                                         |
| ------------------------------ | ----------------------------------------------- |
| `expected_spans.json`          | Span inventory (names, attributes, hierarchies) |
| `expected_metrics.json`        | Metric inventory (SpanMetrics, StatsD, Phase 9) |
| `test_accounts.json`           | Test account roles (keys generated at runtime)  |
| `xrpld-validator.cfg.template` | Node config template with placeholders          |
| `requirements.txt`             | Python dependencies                             |
