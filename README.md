# LogMonitor

A production-grade C++17 log monitoring system with real-time ingestion, time-series storage, configurable alert rules, async webhook delivery, and a live ncurses dashboard.

## Architecture

![LogMonitor workflow](logmonitor_workflow.svg)

## Features

- **HTTP ingestion** on port 9090 (cpp-httplib, 10 worker threads, API key auth)
- **TCP ingestion** on port 5514 (legacy, for syslog-style senders)
- **Time-series database** — in-memory, 24h auto-eviction, 60s binary persistence
- **Alert rules engine** — threshold-based, hot-reload via `alerts.json`
- **Async delivery** — Slack, Discord, generic webhooks, SMTP email (libcurl)
- **Prometheus metrics** at `GET /metrics`
- **4-panel ncurses dashboard** — Apps, Log Feed, Alert History, Throughput Graph
- **Python SDK** — `sdk/logmonitor.py`

## Quick Start (Docker)

```bash
# Clone and start
git clone https://github.com/Twist-Turn/Real-Time-Log-Monitoring
cd LG-Soft

# Start with docker-compose
API_KEY=mysecretkey docker-compose up -d

# Verify it's running
curl -H "X-API-Key: mysecretkey" http://localhost:9090/health

# Send a test log
curl -X POST http://localhost:9090/ingest \
  -H "X-API-Key: mysecretkey" \
  -H "Content-Type: application/json" \
  -d '{"app":"myapp","level":"ERROR","message":"Database timeout","timestamp":0}'

# Query logs
curl "http://localhost:9090/query?app=myapp&level=ERROR&last=300" \
  -H "X-API-Key: mysecretkey"
```

## Build from Source

```bash
# Prerequisites (Ubuntu 22.04)
sudo apt-get install -y cmake ninja-build libncurses5-dev libcurl4-openssl-dev

# macOS (Homebrew)
brew install cmake ninja ncurses curl

# Configure and build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Run tests
cd build && ctest --output-on-failure

# Run
./build/logmonitor config/config.json
```

## API Reference

All endpoints (except `/health`) require the `X-API-Key` header.
Rate limit: 100 requests/minute per API key (returns HTTP 429 when exceeded).

### POST /ingest

Ingest a log entry.

```bash
curl -X POST http://localhost:9090/ingest \
  -H "X-API-Key: changeme" \
  -H "Content-Type: application/json" \
  -d '{"app":"payment-api","level":"ERROR","message":"timeout after 5000ms","timestamp":1711584000}'
```

**Request body:**

| Field       | Type   | Required | Description                                   |
|-------------|--------|----------|-----------------------------------------------|
| `app`       | string | Yes      | Application name                              |
| `level`     | string | Yes      | `INFO`, `WARN`, `ERROR`, or `CRITICAL`        |
| `message`   | string | Yes      | Log message text                              |
| `timestamp` | int    | No       | Unix timestamp (seconds); 0 = use server time |

**Response:**
```json
{"status": "accepted", "app": "payment-api", "level": "ERROR", "ingested": 42}
```

### GET /health

No authentication required.

```bash
curl http://localhost:9090/health
# {"status":"ok"}
```

### GET /query

Query log entries from the TSDB.

```bash
curl "http://localhost:9090/query?app=payment-api&level=ERROR&last=300" \
  -H "X-API-Key: changeme"
```

| Parameter | Default | Description                 |
|-----------|---------|-----------------------------|
| `app`     | —       | Application name (required) |
| `level`   | all     | Filter by severity level    |
| `last`    | 300     | Time window in seconds      |

**Response:**
```json
[
  {"app":"payment-api","level":"ERROR","message":"timeout...","timestamp_ns":1711584000000000000}
]
```

### GET /stats

Per-level log counts for an application.

```bash
curl "http://localhost:9090/stats?app=payment-api&last=3600" \
  -H "X-API-Key: changeme"
```

**Response:**
```json
{
  "app": "payment-api",
  "last_seconds": 3600,
  "counts": {"INFO": 1200, "WARN": 45, "ERROR": 8, "CRITICAL": 0}
}
```

### GET /apps

List all connected applications.

```bash
curl http://localhost:9090/apps -H "X-API-Key: changeme"
```

**Response:**
```json
[{"app":"payment-api","last_seen_ns":1711584000000000000,"total":1253}]
```

### GET /metrics

Prometheus-compatible text format.

```bash
curl http://localhost:9090/metrics -H "X-API-Key: changeme"
```

**Response:**
```
# HELP log_count Total log entries ingested
# TYPE log_count counter
log_count{app="payment-api",level="ERROR"} 8
log_count{app="payment-api",level="WARN"} 45
```

### GET /rules

Alert rules with current state.

```bash
curl http://localhost:9090/rules -H "X-API-Key: changeme"
```

### GET /alerts/history

Last 100 fired alerts.

```bash
curl http://localhost:9090/alerts/history -H "X-API-Key: changeme"
```

## alerts.json Configuration

Edit `config/alerts.json` to define alert rules. The file is **hot-reloaded** — changes take effect within 10 seconds without restarting.

```json
[
  {
    "id": "high-error-rate",
    "app": "payment-api",
    "level": "ERROR",
    "threshold": 10,
    "window_seconds": 60,
    "notify_url": "https://hooks.slack.com/services/YOUR_WEBHOOK",
    "cooldown_seconds": 300
  }
]
```

| Field              | Description                                             |
|--------------------|---------------------------------------------------------|
| `id`               | Unique rule identifier                                  |
| `app`              | Application name to monitor                             |
| `level`            | Log level to count                                      |
| `threshold`        | Fire alert when `count > threshold` in `window_seconds` |
| `window_seconds`   | Rolling time window for counting                        |
| `notify_url`       | Slack/Discord/SMTP/generic webhook URL                  |
| `cooldown_seconds` | Minimum seconds between repeated firings                |

**Supported webhook formats:**
- `https://hooks.slack.com/...` — Slack Incoming Webhooks
- `https://discord.com/api/webhooks/...` — Discord Webhooks
- `smtp://user:pass@host:port/` — SMTP email
- Any other URL — Generic JSON POST

## Integrate in 2 Minutes

### Python SDK

```python
from sdk.logmonitor import LogMonitor

lm = LogMonitor(host="localhost", api_key="changeme")

lm.ingest(app="myapp", level="ERROR", message="Something went wrong")

errors = lm.query(app="myapp", level="ERROR", last_seconds=300)
stats  = lm.get_stats(app="myapp")
```

### curl

```bash
curl -X POST http://localhost:9090/ingest \
  -H "X-API-Key: changeme" \
  -H "Content-Type: application/json" \
  -d '{"app":"myservice","level":"WARN","message":"High memory usage"}'
```

### Flask Demo

```bash
cd sdk
pip install -r requirements.txt
LOGMONITOR_API_KEY=changeme python demo.py
# Dashboard: http://localhost:5000/dashboard
```

## Dashboard

Run the ncurses dashboard (requires a terminal):

```bash
./build/logmonitor config/config.json
```

| Key      | Action               |
|----------|----------------------|
| `Tab`    | Cycle active panel   |
| `↑` `↓`  | Scroll active panel  |
| `r`      | Reload `alerts.json` |
| `q`      | Quit                 |

## Environment Variables (Docker)

| Variable        | Default    | Description                    |
|-----------------|------------|--------------------------------|
| `API_KEY`       | `changeme` | API key for all HTTP endpoints |
| `MAX_MEMORY_MB` | `512`      | Memory limit hint              |
