#pragma once
/**
 * @file WebDashboard.hpp
 * @brief Embedded HTML/CSS/JS web dashboard served from the HTTP endpoint.
 *
 * Single-page app that polls GET /status every 1.5s and renders a live
 * monitoring dashboard in the browser. Zero external dependencies —
 * all CSS/JS is inline.
 */

#include <string>

namespace logmonitor {

inline const std::string& web_dashboard_html() {
    static const std::string html = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Log Monitor Dashboard</title>
<style>
  :root {
    --bg: #0f1117;
    --surface: #1a1d2e;
    --surface2: #232740;
    --border: #2d3154;
    --text: #e1e4f0;
    --text-dim: #8b90a8;
    --green: #22c55e;
    --yellow: #eab308;
    --red: #ef4444;
    --critical: #dc2626;
    --cyan: #06b6d4;
    --purple: #a855f7;
    --blue: #3b82f6;
  }

  * { margin: 0; padding: 0; box-sizing: border-box; }

  body {
    font-family: 'SF Mono', 'Cascadia Code', 'Fira Code', 'Consolas', monospace;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
    overflow-x: hidden;
  }

  /* Header */
  .header {
    background: linear-gradient(135deg, #1e2140 0%, #162033 100%);
    border-bottom: 1px solid var(--border);
    padding: 16px 24px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    position: sticky;
    top: 0;
    z-index: 100;
    backdrop-filter: blur(10px);
  }

  .header-left {
    display: flex;
    align-items: center;
    gap: 12px;
  }

  .logo {
    width: 32px;
    height: 32px;
    background: linear-gradient(135deg, var(--cyan) 0%, var(--blue) 100%);
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 16px;
    font-weight: bold;
  }

  .header h1 {
    font-size: 18px;
    font-weight: 600;
    letter-spacing: -0.5px;
  }

  .header h1 span {
    color: var(--cyan);
  }

  .status-badge {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 14px;
    border-radius: 20px;
    font-size: 12px;
    font-weight: 500;
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }

  .status-badge.online {
    background: rgba(34, 197, 94, 0.1);
    color: var(--green);
    border: 1px solid rgba(34, 197, 94, 0.2);
  }

  .status-badge.offline {
    background: rgba(239, 68, 68, 0.1);
    color: var(--red);
    border: 1px solid rgba(239, 68, 68, 0.2);
  }

  .status-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    animation: pulse 2s ease-in-out infinite;
  }

  .online .status-dot { background: var(--green); }
  .offline .status-dot { background: var(--red); }

  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.4; }
  }

  /* Main layout */
  .container {
    max-width: 1400px;
    margin: 0 auto;
    padding: 20px;
  }

  /* Metric cards row */
  .metrics {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 16px;
    margin-bottom: 20px;
  }

  .metric-card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 20px;
    transition: border-color 0.2s;
  }

  .metric-card:hover {
    border-color: var(--cyan);
  }

  .metric-label {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 1px;
    color: var(--text-dim);
    margin-bottom: 8px;
  }

  .metric-value {
    font-size: 28px;
    font-weight: 700;
    letter-spacing: -1px;
  }

  .metric-unit {
    font-size: 13px;
    color: var(--text-dim);
    font-weight: 400;
    margin-left: 4px;
  }

  /* Alert severity cards */
  .alerts-row {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
    margin-bottom: 20px;
  }

  .alert-card {
    border-radius: 12px;
    padding: 16px 20px;
    border: 1px solid;
    position: relative;
    overflow: hidden;
  }

  .alert-card::before {
    content: '';
    position: absolute;
    top: 0;
    left: 0;
    right: 0;
    height: 3px;
  }

  .alert-card.info {
    background: rgba(34, 197, 94, 0.05);
    border-color: rgba(34, 197, 94, 0.15);
  }
  .alert-card.info::before { background: var(--green); }
  .alert-card.info .alert-count { color: var(--green); }

  .alert-card.warn {
    background: rgba(234, 179, 8, 0.05);
    border-color: rgba(234, 179, 8, 0.15);
  }
  .alert-card.warn::before { background: var(--yellow); }
  .alert-card.warn .alert-count { color: var(--yellow); }

  .alert-card.error {
    background: rgba(239, 68, 68, 0.05);
    border-color: rgba(239, 68, 68, 0.15);
  }
  .alert-card.error::before { background: var(--red); }
  .alert-card.error .alert-count { color: var(--red); }

  .alert-card.critical {
    background: rgba(220, 38, 38, 0.08);
    border-color: rgba(220, 38, 38, 0.25);
  }
  .alert-card.critical::before { background: var(--critical); }
  .alert-card.critical .alert-count { color: var(--critical); }

  .alert-label {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 1px;
    color: var(--text-dim);
    margin-bottom: 4px;
  }

  .alert-count {
    font-size: 32px;
    font-weight: 700;
    letter-spacing: -1px;
  }

  /* Panels */
  .panels {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 16px;
    margin-bottom: 20px;
  }

  .panel {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 12px;
    overflow: hidden;
  }

  .panel-full {
    grid-column: 1 / -1;
  }

  .panel-header {
    padding: 14px 20px;
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: center;
    justify-content: space-between;
    background: var(--surface2);
  }

  .panel-title {
    font-size: 13px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .panel-badge {
    background: var(--border);
    color: var(--text-dim);
    font-size: 11px;
    padding: 2px 8px;
    border-radius: 10px;
    font-weight: 500;
  }

  .panel-body {
    padding: 0;
    max-height: 400px;
    overflow-y: auto;
  }

  .panel-body::-webkit-scrollbar {
    width: 6px;
  }
  .panel-body::-webkit-scrollbar-track {
    background: transparent;
  }
  .panel-body::-webkit-scrollbar-thumb {
    background: var(--border);
    border-radius: 3px;
  }

  /* File/client rows */
  .source-row {
    display: flex;
    align-items: center;
    padding: 12px 20px;
    border-bottom: 1px solid rgba(45, 49, 84, 0.5);
    transition: background 0.15s;
  }

  .source-row:hover {
    background: rgba(6, 182, 212, 0.03);
  }

  .source-row:last-child {
    border-bottom: none;
  }

  .source-icon {
    width: 32px;
    height: 32px;
    border-radius: 8px;
    display: flex;
    align-items: center;
    justify-content: center;
    font-size: 14px;
    margin-right: 14px;
    flex-shrink: 0;
  }

  .source-icon.file {
    background: rgba(59, 130, 246, 0.1);
    color: var(--blue);
  }

  .source-icon.net {
    background: rgba(168, 85, 247, 0.1);
    color: var(--purple);
  }

  .source-info {
    flex: 1;
    min-width: 0;
  }

  .source-name {
    font-size: 13px;
    font-weight: 500;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .source-meta {
    font-size: 11px;
    color: var(--text-dim);
    margin-top: 2px;
  }

  .source-lines {
    font-size: 14px;
    font-weight: 600;
    color: var(--cyan);
    margin-left: 12px;
    white-space: nowrap;
  }

  /* RAG code locations */
  .rag-row {
    padding: 12px 20px;
    border-bottom: 1px solid rgba(45, 49, 84, 0.5);
    font-size: 13px;
  }

  .rag-row:last-child { border-bottom: none; }

  .rag-file {
    color: var(--red);
    font-weight: 600;
  }

  .rag-func {
    color: var(--yellow);
  }

  .rag-score {
    color: var(--text-dim);
    font-size: 11px;
    float: right;
  }

  /* Throughput chart */
  .chart-container {
    padding: 20px;
    height: 180px;
    position: relative;
  }

  .chart-canvas {
    width: 100%;
    height: 100%;
  }

  /* Empty state */
  .empty-state {
    padding: 30px 20px;
    text-align: center;
    color: var(--text-dim);
    font-size: 13px;
  }

  /* Footer */
  .footer {
    text-align: center;
    padding: 20px;
    color: var(--text-dim);
    font-size: 11px;
    border-top: 1px solid var(--border);
    margin-top: 20px;
  }

  .footer a {
    color: var(--cyan);
    text-decoration: none;
  }

  /* Responsive */
  @media (max-width: 768px) {
    .panels { grid-template-columns: 1fr; }
    .alerts-row { grid-template-columns: repeat(2, 1fr); }
    .metrics { grid-template-columns: repeat(2, 1fr); }
    .header h1 { font-size: 15px; }
  }

  /* Transition for value updates */
  .flash {
    animation: flash-update 0.4s ease;
  }

  @keyframes flash-update {
    0% { color: var(--cyan); }
    100% { color: inherit; }
  }
</style>
</head>
<body>

<!-- Header -->
<div class="header">
  <div class="header-left">
    <div class="logo">LM</div>
    <h1>Log <span>Monitor</span> Dashboard</h1>
  </div>
  <div id="status-badge" class="status-badge offline">
    <div class="status-dot"></div>
    <span id="status-text">Connecting...</span>
  </div>
</div>

<div class="container">

  <!-- Key metrics -->
  <div class="metrics">
    <div class="metric-card">
      <div class="metric-label">Throughput</div>
      <div class="metric-value"><span id="m-throughput">0</span><span class="metric-unit">lines/s</span></div>
    </div>
    <div class="metric-card">
      <div class="metric-label">Total Processed</div>
      <div class="metric-value" id="m-total">0</div>
    </div>
    <div class="metric-card">
      <div class="metric-label">Pattern Matches</div>
      <div class="metric-value" id="m-matched">0</div>
    </div>
    <div class="metric-card">
      <div class="metric-label">Avg Latency</div>
      <div class="metric-value"><span id="m-latency">0</span><span class="metric-unit">&micro;s</span></div>
    </div>
  </div>

  <!-- Alert severity cards -->
  <div class="alerts-row">
    <div class="alert-card info">
      <div class="alert-label">Info</div>
      <div class="alert-count" id="a-info">0</div>
    </div>
    <div class="alert-card warn">
      <div class="alert-label">Warning</div>
      <div class="alert-count" id="a-warn">0</div>
    </div>
    <div class="alert-card error">
      <div class="alert-label">Error</div>
      <div class="alert-count" id="a-error">0</div>
    </div>
    <div class="alert-card critical">
      <div class="alert-label">Critical</div>
      <div class="alert-count" id="a-critical">0</div>
    </div>
  </div>

  <!-- Panels -->
  <div class="panels">

    <!-- Watched Files -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none"><path d="M2 3.5A1.5 1.5 0 013.5 2h2.879a1.5 1.5 0 011.06.44l.622.62a1.5 1.5 0 001.06.44H12.5A1.5 1.5 0 0114 5v7.5a1.5 1.5 0 01-1.5 1.5h-9A1.5 1.5 0 012 12.5v-9z" stroke="currentColor" stroke-width="1.2"/></svg>
          Watched Files
          <span class="panel-badge" id="file-count">0</span>
        </div>
      </div>
      <div class="panel-body" id="files-list">
        <div class="empty-state">No files being watched</div>
      </div>
    </div>

    <!-- Throughput History -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none"><path d="M2 13l3-4 3 2 4-6 2 3" stroke="currentColor" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/></svg>
          Throughput History
        </div>
      </div>
      <div class="chart-container">
        <canvas id="throughput-chart" class="chart-canvas"></canvas>
      </div>
    </div>

    <!-- Code Indexer / RAG -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none"><path d="M5.5 4.5L2 8l3.5 3.5M10.5 4.5L14 8l-3.5 3.5M9 2.5l-2 11" stroke="currentColor" stroke-width="1.2" stroke-linecap="round" stroke-linejoin="round"/></svg>
          RAG Code Locations
        </div>
        <span class="panel-badge" id="rag-badge">-</span>
      </div>
      <div class="panel-body" id="rag-list">
        <div class="empty-state">No error-to-code mappings yet</div>
      </div>
    </div>

    <!-- Network Clients -->
    <div class="panel">
      <div class="panel-header">
        <div class="panel-title">
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none"><circle cx="8" cy="8" r="5.5" stroke="currentColor" stroke-width="1.2"/><path d="M8 2.5v11M2.5 8h11M3.5 4.5C5 6 6.5 7 8 7s3-1 4.5-2.5M3.5 11.5C5 10 6.5 9 8 9s3 1 4.5 2.5" stroke="currentColor" stroke-width="1"/></svg>
          Network Clients
          <span class="panel-badge" id="net-count">0</span>
        </div>
      </div>
      <div class="panel-body" id="net-list">
        <div class="empty-state">No network clients connected</div>
      </div>
    </div>

  </div>
</div>

<div class="footer">
  Log Monitor System v1.0 &mdash; Refreshing every 1.5s &mdash; Built with C++17
</div>

<script>
(function() {
  // Throughput history for chart
  const MAX_HISTORY = 60;
  const throughputHistory = new Array(MAX_HISTORY).fill(0);
  let lastData = null;
  let connected = false;

  function formatNumber(n) {
    if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
    if (n >= 1000) return (n / 1000).toFixed(1) + 'K';
    return String(n);
  }

  function updateValue(id, value) {
    const el = document.getElementById(id);
    if (!el) return;
    const formatted = typeof value === 'number' ? formatNumber(value) : value;
    if (el.textContent !== formatted) {
      el.textContent = formatted;
      el.classList.remove('flash');
      void el.offsetWidth;  // reflow
      el.classList.add('flash');
    }
  }

  function setStatus(online) {
    const badge = document.getElementById('status-badge');
    const text = document.getElementById('status-text');
    if (online) {
      badge.className = 'status-badge online';
      text.textContent = 'Live';
    } else {
      badge.className = 'status-badge offline';
      text.textContent = 'Disconnected';
    }
    connected = online;
  }

  function renderFiles(files) {
    const list = document.getElementById('files-list');
    const count = document.getElementById('file-count');
    if (!files || files.length === 0) {
      list.innerHTML = '<div class="empty-state">No files being watched</div>';
      count.textContent = '0';
      return;
    }
    count.textContent = String(files.length);
    let html = '';
    for (const f of files) {
      const name = f.path.split('/').pop();
      const dir = f.path.substring(0, f.path.lastIndexOf('/'));
      html += '<div class="source-row">'
        + '<div class="source-icon file">F</div>'
        + '<div class="source-info">'
        + '<div class="source-name">' + escapeHtml(name) + '</div>'
        + '<div class="source-meta">' + escapeHtml(dir) + '</div>'
        + '</div>'
        + '<div class="source-lines">' + formatNumber(f.lines_read) + ' lines</div>'
        + '</div>';
    }
    list.innerHTML = html;
  }

  function renderRAG(indexer) {
    const list = document.getElementById('rag-list');
    const badge = document.getElementById('rag-badge');
    if (!indexer) {
      badge.textContent = 'disabled';
      list.innerHTML = '<div class="empty-state">Code indexer not configured</div>';
      return;
    }
    badge.textContent = indexer.files_indexed + ' files';
    const locs = indexer.last_error_locations;
    if (!locs || locs.length === 0) {
      list.innerHTML = '<div class="empty-state">No error-to-code mappings yet</div>';
      return;
    }
    let html = '';
    for (const loc of locs) {
      const fileName = loc.file.split('/').pop();
      html += '<div class="rag-row">'
        + '<span class="rag-score">score: ' + loc.score.toFixed(1) + '</span>'
        + '<span class="rag-file">' + escapeHtml(fileName) + ':' + loc.line + '</span>'
        + ' in <span class="rag-func">' + escapeHtml(loc.function || '(unknown)') + '</span>'
        + '</div>';
    }
    list.innerHTML = html;
  }

  function renderNetClients(data) {
    const list = document.getElementById('net-list');
    const count = document.getElementById('net-count');
    // We don't have a direct clients list in /status yet
    // Show network-related info if available
    count.textContent = '0';
    list.innerHTML = '<div class="empty-state">Network client details available in ncurses dashboard</div>';
  }

  function drawChart(canvas) {
    const ctx = canvas.getContext('2d');
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    canvas.width = rect.width * dpr;
    canvas.height = rect.height * dpr;
    ctx.scale(dpr, dpr);
    const w = rect.width;
    const h = rect.height;

    ctx.clearRect(0, 0, w, h);

    const maxVal = Math.max(10, ...throughputHistory) * 1.1;
    const step = w / (MAX_HISTORY - 1);

    // Grid lines
    ctx.strokeStyle = 'rgba(45, 49, 84, 0.5)';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const y = (h / 4) * i;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
    }

    // Grid labels
    ctx.fillStyle = '#8b90a8';
    ctx.font = '10px monospace';
    ctx.textAlign = 'right';
    for (let i = 0; i <= 4; i++) {
      const y = (h / 4) * i;
      const val = Math.round(maxVal * (1 - i / 4));
      ctx.fillText(formatNumber(val), w - 4, y + 12);
    }

    // Area fill
    ctx.beginPath();
    ctx.moveTo(0, h);
    for (let i = 0; i < MAX_HISTORY; i++) {
      const x = i * step;
      const y = h - (throughputHistory[i] / maxVal) * h;
      if (i === 0) ctx.lineTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.lineTo(w, h);
    ctx.closePath();
    const gradient = ctx.createLinearGradient(0, 0, 0, h);
    gradient.addColorStop(0, 'rgba(6, 182, 212, 0.15)');
    gradient.addColorStop(1, 'rgba(6, 182, 212, 0.0)');
    ctx.fillStyle = gradient;
    ctx.fill();

    // Line
    ctx.beginPath();
    for (let i = 0; i < MAX_HISTORY; i++) {
      const x = i * step;
      const y = h - (throughputHistory[i] / maxVal) * h;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = '#06b6d4';
    ctx.lineWidth = 2;
    ctx.stroke();

    // Current value dot
    const lastX = (MAX_HISTORY - 1) * step;
    const lastY = h - (throughputHistory[MAX_HISTORY - 1] / maxVal) * h;
    ctx.beginPath();
    ctx.arc(lastX, lastY, 4, 0, Math.PI * 2);
    ctx.fillStyle = '#06b6d4';
    ctx.fill();
    ctx.beginPath();
    ctx.arc(lastX, lastY, 7, 0, Math.PI * 2);
    ctx.strokeStyle = 'rgba(6, 182, 212, 0.3)';
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  function escapeHtml(s) {
    const div = document.createElement('div');
    div.textContent = s;
    return div.innerHTML;
  }

  async function fetchStatus() {
    try {
      const resp = await fetch('/status');
      if (!resp.ok) throw new Error('HTTP ' + resp.status);
      const data = await resp.json();
      lastData = data;
      setStatus(true);

      // Metrics
      updateValue('m-throughput', data.lines_per_second || 0);
      updateValue('m-total', data.total_lines || 0);
      updateValue('m-matched', data.total_matched || 0);
      updateValue('m-latency', Math.round(data.avg_latency_us || 0));

      // Alerts
      if (data.alerts) {
        updateValue('a-info', data.alerts.info || 0);
        updateValue('a-warn', data.alerts.warn || 0);
        updateValue('a-error', data.alerts.error || 0);
        updateValue('a-critical', data.alerts.critical || 0);
      }

      // Throughput history
      throughputHistory.push(data.lines_per_second || 0);
      if (throughputHistory.length > MAX_HISTORY) throughputHistory.shift();

      // Files
      renderFiles(data.watched_files);

      // RAG
      renderRAG(data.code_indexer);

      // Network
      renderNetClients(data);

      // Chart
      const canvas = document.getElementById('throughput-chart');
      if (canvas) drawChart(canvas);

    } catch (e) {
      setStatus(false);
    }
  }

  // Initial fetch and polling
  fetchStatus();
  setInterval(fetchStatus, 1500);

  // Redraw chart on resize
  window.addEventListener('resize', function() {
    const canvas = document.getElementById('throughput-chart');
    if (canvas && lastData) drawChart(canvas);
  });
})();
</script>

</body>
</html>
)HTMLPAGE";
    return html;
}

} // namespace logmonitor
