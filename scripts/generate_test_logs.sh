#!/usr/bin/env bash
# generate_test_logs.sh — Generates fake log traffic for local file watching.
# Usage: bash scripts/generate_test_logs.sh [duration_seconds]

set -euo pipefail

DURATION=${1:-30}
LOG_DIR="/tmp/test_logs"
FILES=("$LOG_DIR/app.log" "$LOG_DIR/system.log" "$LOG_DIR/access.log")

SEVERITIES=("INFO" "INFO" "INFO" "WARN" "ERROR" "CRITICAL")
MESSAGES_INFO=(
    "Request processed successfully"
    "User authenticated"
    "Cache hit for key session_abc"
    "Health check passed"
    "Connection pool: 12/50 active"
    "Background job completed in 245ms"
)
MESSAGES_WARN=(
    "High memory usage: 85%"
    "Slow query detected: 2.3s"
    "Deprecated API endpoint called: /v1/users"
    "Connection pool nearly full: 48/50"
    "Retry attempt 3 of 5 for external API"
)
MESSAGES_ERROR=(
    "ERROR: NullPointerException at UserService.processPayment(UserService.java:42)"
    "ERROR: Database connection failed: timeout after 5000ms"
    "ERROR: auth_middleware.py:handle_request:156 - Token expired"
    "Exception in handleDatabaseError(server.js:4): ECONNREFUSED"
    "ERROR: Failed to write to /var/log/app.log: Permission denied"
)
MESSAGES_CRITICAL=(
    "CRITICAL: OOM killer invoked — process 4521 terminated"
    "CRITICAL: segfault in /usr/src/app/src/pipeline.cpp:287"
    "CRITICAL: Disk /dev/sda1 100% full — system entering read-only mode"
    "CRITICAL: All database replicas unreachable"
)

# Create log directories and files
mkdir -p "$LOG_DIR"
for f in "${FILES[@]}"; do
    : > "$f"
done

echo "Generating test logs to $LOG_DIR for ${DURATION}s..."
echo "Files: ${FILES[*]}"
echo "Press Ctrl+C to stop."

END=$((SECONDS + DURATION))
LINE_COUNT=0

while [ $SECONDS -lt $END ]; do
    # Pick a random file
    FILE=${FILES[$((RANDOM % ${#FILES[@]}))]}

    # Pick severity (weighted towards INFO)
    SEV=${SEVERITIES[$((RANDOM % ${#SEVERITIES[@]}))]}

    # Pick message based on severity
    TIMESTAMP=$(date "+%Y-%m-%d %H:%M:%S.%3N")
    case $SEV in
        INFO)
            MSG=${MESSAGES_INFO[$((RANDOM % ${#MESSAGES_INFO[@]}))]}
            echo "[$TIMESTAMP] INFO: $MSG" >> "$FILE"
            ;;
        WARN)
            MSG=${MESSAGES_WARN[$((RANDOM % ${#MESSAGES_WARN[@]}))]}
            echo "[$TIMESTAMP] WARN: $MSG" >> "$FILE"
            ;;
        ERROR)
            MSG=${MESSAGES_ERROR[$((RANDOM % ${#MESSAGES_ERROR[@]}))]}
            echo "[$TIMESTAMP] $MSG" >> "$FILE"
            ;;
        CRITICAL)
            MSG=${MESSAGES_CRITICAL[$((RANDOM % ${#MESSAGES_CRITICAL[@]}))]}
            echo "[$TIMESTAMP] $MSG" >> "$FILE"
            ;;
    esac

    LINE_COUNT=$((LINE_COUNT + 1))

    # Random delay: 10-100ms
    sleep 0.0$((RANDOM % 9 + 1))
done

echo "Done. Generated $LINE_COUNT log lines."
