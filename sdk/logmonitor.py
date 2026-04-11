"""
logmonitor.py — Python SDK for the LogMonitor system.

Usage:
    from logmonitor import LogMonitor

    lm = LogMonitor(host="localhost", api_key="changeme")

    # Check server health
    if lm.health():
        # Send a log entry
        lm.ingest(app="myapp", level="ERROR", message="Disk full")

        # Query recent errors
        errors = lm.query(app="myapp", level="ERROR", last_seconds=300)

        # Get per-level stats
        stats = lm.get_stats(app="myapp")

        # List all connected apps
        apps = lm.get_apps()

        # Get Prometheus-format metrics
        metrics = lm.get_metrics()
"""

import time
from typing import Any, Dict, List, Optional

import requests


class LogMonitorError(Exception):
    """Raised when the LogMonitor server returns an error response."""
    pass


class LogMonitor:
    """Client SDK for the LogMonitor HTTP server (port 9090)."""

    DEFAULT_PORT = 9090
    DEFAULT_TIMEOUT = 5  # seconds

    def __init__(self, host: str, api_key: str, port: int = DEFAULT_PORT,
                 timeout: int = DEFAULT_TIMEOUT) -> None:
        """
        Initialize the LogMonitor client.

        Args:
            host:     Hostname or IP of the LogMonitor server.
            api_key:  API key for authentication (X-API-Key header).
            port:     HTTP port (default 9090).
            timeout:  Request timeout in seconds (default 5).
        """
        self.base_url = f"http://{host}:{port}"
        self.timeout  = timeout
        self._headers = {
            "X-API-Key":    api_key,
            "Content-Type": "application/json",
        }

    # ─── Core endpoints ───

    def ingest(self, app: str, level: str, message: str,
               timestamp: Optional[int] = None) -> Dict[str, Any]:
        """
        Send a single log entry to the monitor.

        Args:
            app:       Application name (e.g., "payment-api").
            level:     Severity level: "INFO", "WARN", "ERROR", or "CRITICAL".
            message:   Log message text.
            timestamp: Optional Unix timestamp (seconds). Uses server time if 0 or None.

        Returns:
            {"status": "accepted", "app": ..., "level": ..., "ingested": N}

        Raises:
            LogMonitorError: If the server returns a non-2xx response.
        """
        payload: Dict[str, Any] = {
            "app":       app,
            "level":     level,
            "message":   message,
            "timestamp": timestamp if timestamp is not None else 0,
        }
        resp = requests.post(
            f"{self.base_url}/ingest",
            json=payload,
            headers=self._headers,
            timeout=self.timeout,
        )
        self._raise_for_status(resp, "ingest")
        return resp.json()

    def query(self, app: str, level: str = "",
              last_seconds: int = 300) -> List[Dict[str, Any]]:
        """
        Query log entries from the TSDB.

        Args:
            app:          Application name to filter by.
            level:        Severity level filter (empty = all levels).
            last_seconds: Time window in seconds (default 300 = last 5 minutes).

        Returns:
            List of log entry dicts:
            [{"app": ..., "level": ..., "message": ..., "timestamp_ns": N}, ...]
        """
        params: Dict[str, Any] = {"app": app, "last": last_seconds}
        if level:
            params["level"] = level

        resp = requests.get(
            f"{self.base_url}/query",
            params=params,
            headers=self._headers,
            timeout=self.timeout,
        )
        self._raise_for_status(resp, "query")
        return resp.json()  # type: ignore[return-value]

    def get_stats(self, app: str, last_seconds: int = 3600) -> Dict[str, Any]:
        """
        Get per-severity-level counts for an application.

        Args:
            app:          Application name.
            last_seconds: Time window in seconds (default 3600 = last hour).

        Returns:
            {
              "app": "myapp",
              "last_seconds": 3600,
              "counts": {"INFO": N, "WARN": N, "ERROR": N, "CRITICAL": N}
            }
        """
        resp = requests.get(
            f"{self.base_url}/stats",
            params={"app": app, "last": last_seconds},
            headers=self._headers,
            timeout=self.timeout,
        )
        self._raise_for_status(resp, "get_stats")
        return resp.json()  # type: ignore[return-value]

    # ─── Phase 5 endpoints ───

    def get_apps(self) -> List[Dict[str, Any]]:
        """
        List all connected applications with metadata.

        Returns:
            [{"app": "myapp", "last_seen_ns": N, "total": N}, ...]
        """
        resp = requests.get(
            f"{self.base_url}/apps",
            headers=self._headers,
            timeout=self.timeout,
        )
        self._raise_for_status(resp, "get_apps")
        return resp.json()  # type: ignore[return-value]

    def get_metrics(self) -> str:
        """
        Get Prometheus-compatible metrics output.

        Returns:
            Text string in Prometheus exposition format.
        """
        resp = requests.get(
            f"{self.base_url}/metrics",
            headers=self._headers,
            timeout=self.timeout,
        )
        self._raise_for_status(resp, "get_metrics")
        return resp.text

    def get_rules(self) -> Dict[str, Any]:
        """
        Get all alert rules with their current state.

        Returns:
            {"rules": [{"id": ..., "app": ..., "state": "INACTIVE"|"FIRING"|"COOLDOWN", ...}]}
        """
        resp = requests.get(
            f"{self.base_url}/rules",
            headers=self._headers,
            timeout=self.timeout,
        )
        self._raise_for_status(resp, "get_rules")
        return resp.json()  # type: ignore[return-value]

    def get_alert_history(self) -> Dict[str, Any]:
        """
        Get the last 100 fired alerts.

        Returns:
            {"history": [{"rule_id": ..., "app": ..., "count": N, "delivered": True/False}, ...]}
        """
        resp = requests.get(
            f"{self.base_url}/alerts/history",
            headers=self._headers,
            timeout=self.timeout,
        )
        self._raise_for_status(resp, "get_alert_history")
        return resp.json()  # type: ignore[return-value]

    # ─── Utility ───

    def health(self) -> bool:
        """
        Check if the LogMonitor server is reachable and healthy.

        Returns:
            True if the server responds with {"status": "ok"}, False otherwise.
        """
        try:
            resp = requests.get(
                f"{self.base_url}/health",
                timeout=2,
            )
            return resp.status_code == 200 and resp.json().get("status") == "ok"
        except Exception:
            return False

    def ingest_batch(self, entries: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        """
        Convenience method to send multiple log entries.

        Args:
            entries: List of dicts with keys: app, level, message, timestamp (optional).

        Returns:
            List of response dicts from individual ingest calls.
        """
        results = []
        for entry in entries:
            result = self.ingest(
                app=entry["app"],
                level=entry["level"],
                message=entry["message"],
                timestamp=entry.get("timestamp"),
            )
            results.append(result)
        return results

    # ─── Internal helpers ───

    @staticmethod
    def _raise_for_status(resp: requests.Response, operation: str) -> None:
        if resp.status_code == 401:
            raise LogMonitorError(
                f"{operation}: Authentication failed — check your API key"
            )
        if resp.status_code == 429:
            raise LogMonitorError(
                f"{operation}: Rate limit exceeded (100 req/min)"
            )
        if resp.status_code >= 400:
            try:
                body = resp.json()
                msg = body.get("error", resp.text)
            except Exception:
                msg = resp.text
            raise LogMonitorError(
                f"{operation}: Server returned {resp.status_code}: {msg}"
            )
