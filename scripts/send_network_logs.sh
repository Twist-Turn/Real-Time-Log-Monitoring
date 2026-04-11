#!/usr/bin/env bash
# send_network_logs.sh — Sends test logs over TCP and HTTP to the log monitor.
# Usage: bash scripts/send_network_logs.sh [host] [tcp_port] [http_port]

set -euo pipefail

HOST=${1:-localhost}
TCP_PORT=${2:-5514}
HTTP_PORT=${3:-8080}

echo "=== Network Log Ingestion Test ==="
echo "Target: $HOST (TCP: $TCP_PORT, HTTP: $HTTP_PORT)"
echo ""

# ─── TCP Tests ───
echo "--- TCP Ingestion (port $TCP_PORT) ---"

echo "Sending 5 TCP log lines..."
{
    echo "payment-api|ERROR|NullPointerException at PaymentService.processTransaction(PaymentService.java:87)"
    sleep 0.1
    echo "payment-api|WARN|High latency detected: 2500ms for /api/checkout"
    sleep 0.1
    echo "auth-service|ERROR|Token validation failed for user_12345: expired"
    sleep 0.1
    echo "auth-service|INFO|User login successful: admin@example.com"
    sleep 0.1
    echo "worker-pool|CRITICAL|OOM killer invoked — worker process 8821 terminated"
    sleep 0.2
} | nc -q 1 "$HOST" "$TCP_PORT" 2>/dev/null || \
    echo "  [WARN] TCP connection failed. Is the monitor running?"

echo ""

# ─── HTTP Tests ───
echo "--- HTTP Ingestion (port $HTTP_PORT) ---"

# Test 1: POST /ingest with multiple lines
echo "Sending HTTP POST /ingest (batch)..."
RESPONSE=$(curl -s -w "\n%{http_code}" -X POST "http://$HOST:$HTTP_PORT/ingest" \
    -H "Content-Type: application/json" \
    -d '{
        "service": "web-frontend",
        "lines": [
            "ERROR: Unhandled promise rejection in handleDatabaseError(server.js:4)",
            "WARN: React component re-render limit exceeded (500 renders in 1s)",
            "INFO: Page /dashboard loaded in 342ms",
            "ERROR: auth_middleware.py:handle_request:156 - Token expired for session abc123",
            "CRITICAL: segfault in /usr/src/app/src/pipeline.cpp:287"
        ]
    }' 2>/dev/null) || echo "  [WARN] HTTP POST failed. Is the monitor running?"

if [ -n "$RESPONSE" ]; then
    HTTP_CODE=$(echo "$RESPONSE" | tail -1)
    BODY=$(echo "$RESPONSE" | head -n -1)
    echo "  Status: $HTTP_CODE"
    echo "  Response: $BODY"
fi

echo ""

# Test 2: POST /ingest single line
echo "Sending HTTP POST /ingest (single line)..."
curl -s -X POST "http://$HOST:$HTTP_PORT/ingest" \
    -H "Content-Type: application/json" \
    -d '{"service": "cron-job", "line": "ERROR: Scheduled backup failed: disk full"}' \
    2>/dev/null && echo "" || echo "  [WARN] HTTP POST failed."

echo ""

# Test 3: GET /status
echo "Fetching GET /status..."
curl -s "http://$HOST:$HTTP_PORT/status" 2>/dev/null | python3 -m json.tool 2>/dev/null || \
    echo "  [WARN] Status endpoint unavailable."

echo ""

# Test 4: GET /health
echo "Fetching GET /health..."
curl -s "http://$HOST:$HTTP_PORT/health" 2>/dev/null || \
    echo "  [WARN] Health endpoint unavailable."

echo ""
echo "=== Network test complete ==="
