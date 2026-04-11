"""
demo.py — Demo Flask app showing how to integrate with LogMonitor.

This script:
  1. Connects to a running LogMonitor server
  2. Starts a background thread that sends a log every second
  3. Serves a /dashboard endpoint showing live stats
  4. Serves a /logs endpoint showing recent error logs

Run:
    pip install -r requirements.txt
    python demo.py

Then visit:
    http://localhost:5000/dashboard
    http://localhost:5000/logs?app=flask-demo
"""

import os
import random
import threading
import time
from typing import Any, Dict

from flask import Flask, jsonify, render_template_string

from logmonitor import LogMonitor, LogMonitorError

# ─── Configuration ───
LM_HOST    = os.environ.get("LOGMONITOR_HOST", "localhost")
LM_PORT    = int(os.environ.get("LOGMONITOR_PORT", "9090"))
LM_API_KEY = os.environ.get("LOGMONITOR_API_KEY", "changeme")
APP_NAME   = "flask-demo"

# ─── LogMonitor client ───
lm = LogMonitor(host=LM_HOST, api_key=LM_API_KEY, port=LM_PORT)

app = Flask(__name__)

# ─── Synthetic log generator ───

SAMPLE_LOGS = [
    ("INFO",     "Request processed successfully in 45ms"),
    ("INFO",     "User authentication succeeded"),
    ("WARN",     "Response time exceeded 200ms threshold"),
    ("WARN",     "Cache miss rate above 80% for past 5 minutes"),
    ("ERROR",    "Database connection pool exhausted"),
    ("ERROR",    "Failed to process payment: timeout"),
    ("ERROR",    "Null pointer dereference in UserController.handleRequest()"),
    ("CRITICAL", "Out of memory: killed process 1234"),
]


def log_generator() -> None:
    """Background thread: sends one log per second to LogMonitor."""
    print(f"[demo] Starting log generator → {LM_HOST}:{LM_PORT}")
    while True:
        try:
            level, message = random.choice(SAMPLE_LOGS)
            lm.ingest(app=APP_NAME, level=level, message=message)
        except LogMonitorError as e:
            print(f"[demo] LogMonitor error: {e}")
        except Exception as e:
            print(f"[demo] Unexpected error: {e}")
        time.sleep(1)


DASHBOARD_HTML = """
<!DOCTYPE html>
<html>
<head>
  <title>LogMonitor Demo Dashboard</title>
  <meta charset="utf-8">
  <meta http-equiv="refresh" content="5">
  <style>
    body { font-family: monospace; background: #1a1a2e; color: #eee; padding: 20px; }
    h1   { color: #00d4ff; }
    h2   { color: #a0c4ff; margin-top: 20px; }
    table{ border-collapse: collapse; width: 100%; }
    th   { background: #16213e; color: #00d4ff; padding: 8px 12px; text-align: left; }
    td   { padding: 6px 12px; border-bottom: 1px solid #333; }
    .error    { color: #ff6b6b; }
    .warn     { color: #ffd166; }
    .info     { color: #06d6a0; }
    .critical { color: #ff0000; font-weight: bold; }
    .badge    { display: inline-block; padding: 2px 8px; border-radius: 3px; font-size: 0.85em; }
    .badge-ok { background: #06d6a0; color: #000; }
    .badge-err{ background: #ff6b6b; color: #000; }
  </style>
</head>
<body>
  <h1>LogMonitor Demo Dashboard</h1>
  <p>Auto-refreshes every 5 seconds &bull;
     Server: <strong>{{ host }}:{{ port }}</strong> &bull;
     Status: <span class="badge {{ 'badge-ok' if healthy else 'badge-err' }}">
               {{ 'ONLINE' if healthy else 'OFFLINE' }}</span>
  </p>

  <h2>Connected Apps</h2>
  <table>
    <tr><th>App</th><th>Total (1h)</th></tr>
    {% for a in apps %}
    <tr><td>{{ a.app }}</td><td>{{ a.total }}</td></tr>
    {% endfor %}
  </table>

  <h2>{{ app_name }} — Stats (last hour)</h2>
  <table>
    <tr><th>Level</th><th>Count</th></tr>
    {% if stats %}
    <tr><td class="info">INFO</td>         <td>{{ stats.counts.INFO }}</td></tr>
    <tr><td class="warn">WARN</td>         <td>{{ stats.counts.WARN }}</td></tr>
    <tr><td class="error">ERROR</td>       <td>{{ stats.counts.ERROR }}</td></tr>
    <tr><td class="critical">CRITICAL</td> <td>{{ stats.counts.CRITICAL }}</td></tr>
    {% endif %}
  </table>

  <h2>Recent Errors (last 5 min)</h2>
  <table>
    <tr><th>Level</th><th>Message</th></tr>
    {% for log in recent_errors %}
    <tr>
      <td class="{{ log.level | lower }}">{{ log.level }}</td>
      <td>{{ log.message }}</td>
    </tr>
    {% endfor %}
    {% if not recent_errors %}
    <tr><td colspan="2">No errors in the last 5 minutes</td></tr>
    {% endif %}
  </table>

  <p style="color:#666; margin-top:30px;">
    Powered by <a href="https://github.com" style="color:#00d4ff">LogMonitor</a>
  </p>
</body>
</html>
"""


@app.route("/dashboard")
def dashboard() -> str:
    """Render a live stats dashboard."""
    healthy = lm.health()
    apps: list = []
    stats: Dict[str, Any] = {}
    recent_errors: list = []

    if healthy:
        try:
            apps = lm.get_apps()
            stats = lm.get_stats(APP_NAME)
            recent_errors = lm.query(APP_NAME, level="ERROR", last_seconds=300)
            recent_errors += lm.query(APP_NAME, level="CRITICAL", last_seconds=300)
            recent_errors = recent_errors[-20:]  # Last 20
        except LogMonitorError:
            pass

    return render_template_string(
        DASHBOARD_HTML,
        host=LM_HOST, port=LM_PORT,
        healthy=healthy,
        apps=apps,
        stats=stats,
        app_name=APP_NAME,
        recent_errors=recent_errors,
    )


@app.route("/logs")
def logs() -> Any:
    """JSON endpoint: recent logs for a given app."""
    try:
        entries = lm.query(APP_NAME, last_seconds=300)
        return jsonify({"app": APP_NAME, "count": len(entries), "entries": entries})
    except LogMonitorError as e:
        return jsonify({"error": str(e)}), 500


@app.route("/health")
def health_check() -> Any:
    """Health check for the demo app itself."""
    lm_ok = lm.health()
    return jsonify({"status": "ok", "logmonitor": lm_ok})


if __name__ == "__main__":
    # Start log generator in background
    gen_thread = threading.Thread(target=log_generator, daemon=True)
    gen_thread.start()

    # Check connectivity
    if lm.health():
        print(f"[demo] Connected to LogMonitor at {LM_HOST}:{LM_PORT}")
    else:
        print(f"[demo] WARNING: Cannot reach LogMonitor at {LM_HOST}:{LM_PORT}")
        print("[demo] Make sure LogMonitor is running before sending logs")

    print("[demo] Dashboard: http://localhost:5000/dashboard")
    print("[demo] Logs API:  http://localhost:5000/logs")

    port = int(os.environ.get("DEMO_PORT", "5001"))
    app.run(host="0.0.0.0", port=port, debug=False)
