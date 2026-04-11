/**
 * @file WebUI.hpp
 * @brief Modern web dashboard embedded as C++ string literals.
 *
 * Uses Tailwind CSS (CDN), Chart.js (CDN), and Inter font (Google Fonts).
 * All HTML/CSS/JS is returned from in-memory functions — no filesystem access
 * at runtime.
 *
 * Routes served:
 *   GET /               → login_html()
 *   GET /app            → app_html()
 *   GET /static/app.css → app_css()
 *   GET /static/app.js  → app_js()
 */
#pragma once

#include <string>

namespace logmonitor {
namespace webui {

// ─── Custom CSS (Tailwind handles layout; this covers custom components) ──────

inline std::string app_css() {
    return R"css(
@import url('https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap');

*, *::before, *::after { box-sizing: border-box; }
html, body { height: 100%; margin: 0; padding: 0; }
body { font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif; }

/* ── Tab panels ── */
.tab-panel { display: none; }
.tab-panel.active { display: block; }

/* ── Sidebar nav active state ── */
.nav-item {
    display: flex; align-items: center; gap: 10px;
    padding: 9px 12px; border-radius: 8px;
    color: #94a3b8; font-size: 13.5px; font-weight: 500;
    cursor: pointer; text-decoration: none; margin-bottom: 2px;
    transition: background 0.15s, color 0.15s;
    border: none; background: transparent; width: 100%; text-align: left;
}
.nav-item:hover { background: rgba(255,255,255,0.07); color: #e2e8f0; }
.nav-item.active { background: rgba(249,115,22,0.18); color: #fb923c; }
.nav-item.active svg { color: #fb923c; }

/* ── Project selector (custom arrow) ── */
#project-select {
    width: 100%;
    background: rgba(255,255,255,0.07);
    border: 1px solid rgba(255,255,255,0.1);
    color: #e2e8f0; padding: 7px 28px 7px 10px;
    border-radius: 7px; font-size: 13px; font-family: inherit;
    cursor: pointer; outline: none; appearance: none;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 24 24' fill='none' stroke='%2394a3b8' stroke-width='2.5'%3E%3Cpath d='m6 9 6 6 6-6'/%3E%3C/svg%3E");
    background-repeat: no-repeat; background-position: right 8px center;
    transition: border-color 0.15s;
}
#project-select option { background: #1e293b; color: #e2e8f0; }
#project-select:focus { border-color: #f97316; }

/* ── Level pill badges ── */
.pill {
    display: inline-block; padding: 2px 9px; border-radius: 99px;
    font-size: 10.5px; font-weight: 600; letter-spacing: 0.05em; text-transform: uppercase;
}
.pill-INFO     { background: #f0fdf4; color: #15803d; border: 1px solid #bbf7d0; }
.pill-WARN     { background: #fefce8; color: #a16207; border: 1px solid #fde047; }
.pill-ERROR    { background: #fff1f2; color: #be123c; border: 1px solid #fecdd3; }
.pill-CRITICAL { background: #faf5ff; color: #7e22ce; border: 1px solid #e9d5ff; }

/* ── Alert state badges ── */
.state-badge {
    display: inline-flex; align-items: center; gap: 5px;
    padding: 3px 10px; border-radius: 99px; font-size: 11px; font-weight: 600;
}
.state-INACTIVE { background: #f1f5f9; color: #64748b; }
.state-FIRING   { background: #fff1f2; color: #be123c; }
.state-COOLDOWN { background: #fefce8; color: #a16207; }
.state-dot { width: 6px; height: 6px; border-radius: 50%; display: inline-block; flex-shrink: 0; }
.dot-INACTIVE { background: #94a3b8; }
.dot-FIRING   { background: #f43f5e; animation: blink 1s ease-in-out infinite; }
.dot-COOLDOWN { background: #eab308; }
@keyframes blink { 0%,100%{opacity:1;} 50%{opacity:0.3;} }

/* ── App name badge ── */
.app-tag {
    display: inline-block; padding: 1px 7px; border-radius: 5px;
    background: #f1f5f9; color: #475569; font-size: 12px; font-weight: 500;
}

/* ── Timestamp cell ── */
.ts-cell {
    font-family: 'JetBrains Mono', 'Fira Code', 'Courier New', monospace;
    font-size: 11.5px; color: #94a3b8; white-space: nowrap;
}

/* ── Code block ── */
.code-block {
    background: #0f172a; color: #e2e8f0; border-radius: 10px;
    padding: 16px; font-family: 'JetBrains Mono', 'Fira Code', monospace;
    font-size: 12px; overflow-x: auto; white-space: pre; line-height: 1.65;
}

/* ── Spinner ── */
.spinner {
    width: 18px; height: 18px;
    border: 2px solid #e2e8f0; border-top-color: #f97316;
    border-radius: 50%; animation: spin 0.7s linear infinite; flex-shrink: 0;
}
@keyframes spin { to { transform: rotate(360deg); } }

/* ── Login page ── */
.login-bg {
    min-height: 100vh;
    background: linear-gradient(135deg, #0f172a 0%, #1a1040 55%, #0f172a 100%);
    display: flex; align-items: center; justify-content: center; padding: 24px;
    position: relative;
}
.login-bg::before {
    content: ''; position: fixed; inset: 0;
    background-image: radial-gradient(rgba(255,255,255,0.035) 1px, transparent 1px);
    background-size: 28px 28px; pointer-events: none;
}
.login-card {
    background: white; border-radius: 18px; padding: 36px;
    box-shadow: 0 32px 64px rgba(0,0,0,0.45), 0 0 0 1px rgba(255,255,255,0.06);
    width: 100%; max-width: 400px; position: relative; z-index: 1;
}
.login-tab {
    flex: 1; background: transparent; border: none;
    padding: 9px 0; font-size: 14px; font-weight: 500;
    cursor: pointer; color: #94a3b8; font-family: inherit;
    border-bottom: 2px solid transparent; margin-bottom: -2px;
    transition: color 0.15s;
}
.login-tab.active { color: #f97316; border-bottom-color: #f97316; }
.form-input {
    width: 100%; padding: 10px 13px; border: 1.5px solid #e2e8f0;
    border-radius: 9px; font-size: 14px; font-family: inherit;
    color: #1e293b; outline: none; transition: border-color 0.15s;
    margin-top: 6px;
}
.form-input:focus { border-color: #f97316; box-shadow: 0 0 0 3px rgba(249,115,22,0.1); }
.form-btn {
    width: 100%; padding: 11px; margin-top: 6px;
    background: linear-gradient(135deg, #f97316, #ea580c);
    color: white; border: none; border-radius: 9px;
    font-size: 14px; font-weight: 600; cursor: pointer; font-family: inherit;
    transition: opacity 0.15s, transform 0.1s;
    box-shadow: 0 4px 12px rgba(249,115,22,0.35);
}
.form-btn:hover { opacity: 0.92; }
.form-btn:active { transform: translateY(1px); }

/* ── Scrollbar ── */
::-webkit-scrollbar { width: 5px; height: 5px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: #cbd5e1; border-radius: 3px; }
::-webkit-scrollbar-thumb:hover { background: #94a3b8; }

/* ── Table hover ── */
tbody tr:hover td { background: #fafbff !important; }
)css";
}

// ─── JavaScript ───────────────────────────────────────────────────────────────

inline std::string app_js() {
    return R"js(
'use strict';

// ── Auth ──────────────────────────────────────────────────────────────────────
function getToken()  { return localStorage.getItem('lm_token');  }
function getEmail()  { return localStorage.getItem('lm_email');  }
function setSession(token, user_id, email) {
    localStorage.setItem('lm_token',   token);
    localStorage.setItem('lm_user_id', user_id);
    localStorage.setItem('lm_email',   email);
}
function clearSession() {
    ['lm_token','lm_user_id','lm_email'].forEach(k => localStorage.removeItem(k));
}

// ── API wrapper ───────────────────────────────────────────────────────────────
async function api(path, opts = {}) {
    opts.headers = Object.assign({}, opts.headers || {});
    const tok = getToken();
    if (tok) opts.headers['Authorization'] = 'Bearer ' + tok;
    if (opts.body && !opts.headers['Content-Type'])
        opts.headers['Content-Type'] = 'application/json';
    const r = await fetch(path, opts);
    if (r.status === 401 && path !== '/auth/login' && path !== '/auth/register') {
        clearSession(); location.href = '/'; return null;
    }
    const ct = r.headers.get('content-type') || '';
    if (ct.includes('text/plain')) return { _text: await r.text(), _status: r.status };
    try { const j = await r.json(); j._status = r.status; return j; }
    catch(_) { return { _status: r.status }; }
}

// ── Project state ─────────────────────────────────────────────────────────────
let projects      = [];
let currentProject = null;
let metricsChart  = null;

function currentProjectId() {
    const sel = document.getElementById('project-select');
    return sel ? sel.value : '';
}

// ── Tab switching ─────────────────────────────────────────────────────────────
function showTab(name) {
    document.querySelectorAll('.tab-panel').forEach(s => s.classList.remove('active'));
    document.querySelectorAll('[data-tab]').forEach(a => a.classList.remove('active'));
    const panel = document.getElementById('tab-' + name);
    if (panel) panel.classList.add('active');
    document.querySelectorAll('[data-tab="' + name + '"]').forEach(a => a.classList.add('active'));
    if (name === 'logs')     { loadAppsForFilter(); loadLogs(); }
    if (name === 'metrics')  loadMetrics();
    if (name === 'alerts')   loadAlerts();
    if (name === 'settings') loadSettings();
}

// ── Projects ──────────────────────────────────────────────────────────────────
async function loadProjects() {
    const data = await api('/api/projects');
    if (!data || !Array.isArray(data)) return;
    projects = data;

    const sel = document.getElementById('project-select');
    sel.innerHTML = '';
    if (projects.length === 0) {
        sel.innerHTML = '<option value="">No projects</option>';
        currentProject = null;
        showTab('no-project');
        return;
    }
    projects.forEach(p => {
        const opt = document.createElement('option');
        opt.value = p.id; opt.textContent = p.name;
        sel.appendChild(opt);
    });
    const saved = localStorage.getItem('lm_proj');
    if (saved && projects.find(p => p.id === saved)) sel.value = saved;
    currentProject = projects.find(p => p.id === sel.value) || projects[0];
    sel.value = currentProject.id;
    localStorage.setItem('lm_proj', currentProject.id);
    document.getElementById('tab-no-project').classList.remove('active');
    showTab('logs');
}

async function createProject(inputId) {
    inputId = inputId || 'new-proj-name';
    const nameInput = document.getElementById(inputId);
    const name = (nameInput ? nameInput.value : '').trim();
    if (!name) { alert('Enter a project name'); return; }
    const data = await api('/api/projects', { method: 'POST', body: JSON.stringify({ name }) });
    if (!data || data.error) { alert(data && data.error ? data.error : 'Failed'); return; }
    if (nameInput) nameInput.value = '';
    await loadProjects();
    const sel = document.getElementById('project-select');
    sel.value = data.id;
    currentProject = data;
    showTab('settings');
}

async function deleteCurrentProject() {
    const pid = currentProjectId();
    if (!pid) return;
    if (!confirm('Delete project "' + (currentProject ? currentProject.name : pid) + '"?\nThis cannot be undone.')) return;
    await api('/api/projects/' + pid, { method: 'DELETE' });
    await loadProjects();
}

// ── Logs tab ──────────────────────────────────────────────────────────────────
function loadingRow(cols, msg) {
    return '<tr><td colspan="' + cols + '" style="padding:36px;text-align:center">'
        + '<div style="display:flex;align-items:center;justify-content:center;gap:10px;color:#94a3b8;font-size:13px">'
        + '<div class="spinner"></div>' + (msg || 'Loading…') + '</div></td></tr>';
}
function emptyRow(cols, msg) {
    return '<tr><td colspan="' + cols + '" style="padding:48px;text-align:center;color:#cbd5e1;font-size:14px">'
        + msg + '</td></tr>';
}

async function loadLogs() {
    const pid = currentProjectId();
    if (!pid) return;
    const app   = document.getElementById('log-app-filter').value;
    const level = document.getElementById('log-level-filter').value;
    const range = document.getElementById('log-range-select').value;
    const params = new URLSearchParams({ last: range });
    if (app)   params.set('app',   app);
    if (level) params.set('level', level);

    const tbody = document.getElementById('logs-tbody');
    tbody.innerHTML = loadingRow(4);

    const data = await api('/api/projects/' + pid + '/query?' + params.toString());
    if (!data || !Array.isArray(data)) { tbody.innerHTML = emptyRow(4, 'No data'); return; }
    if (data.length === 0) { tbody.innerHTML = emptyRow(4, 'No logs match your filters'); return; }

    tbody.innerHTML = data.map(e => {
        const ts  = new Date(Math.floor(e.timestamp_ns / 1e6)).toISOString().replace('T',' ').slice(0,23);
        const lvl = esc(e.level || 'INFO');
        return '<tr>'
            + '<td class="ts-cell" style="padding:9px 14px">' + ts + '</td>'
            + '<td style="padding:9px 14px"><span class="app-tag">' + esc(e.app||'') + '</span></td>'
            + '<td style="padding:9px 14px"><span class="pill pill-' + lvl + '">' + lvl + '</span></td>'
            + '<td style="padding:9px 14px;font-size:13px;color:#374151">' + esc(e.message||'') + '</td>'
            + '</tr>';
    }).join('');
}

async function loadAppsForFilter() {
    const pid = currentProjectId();
    if (!pid) return;
    const data = await api('/api/projects/' + pid + '/apps');
    const sel = document.getElementById('log-app-filter');
    if (!sel || !Array.isArray(data)) return;
    const cur = sel.value;
    sel.innerHTML = '<option value="">All Apps</option>';
    data.forEach(a => {
        const opt = document.createElement('option');
        opt.value = a.app;
        opt.textContent = a.app + ' (' + (a.total || 0) + ')';
        sel.appendChild(opt);
    });
    if (cur) sel.value = cur;
}

// ── Metrics tab ───────────────────────────────────────────────────────────────
async function loadMetrics() {
    const pid = currentProjectId();
    if (!pid) return;

    const tbody = document.getElementById('metrics-tbody');
    tbody.innerHTML = loadingRow(6);

    const apps = await api('/api/projects/' + pid + '/apps');
    if (!apps || !Array.isArray(apps) || apps.length === 0) {
        tbody.innerHTML = emptyRow(6, 'No apps have sent logs yet');
        ['stat-info','stat-warn','stat-error','stat-critical'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.textContent = '0';
        });
        return;
    }

    const rows = await Promise.all(apps.map(async a => {
        const stats = await api('/api/projects/' + pid + '/stats?app=' + encodeURIComponent(a.app) + '&last=3600');
        const c = (stats && stats.counts) ? stats.counts : {};
        return { app: a.app, info: c.INFO||0, warn: c.WARN||0, error: c.ERROR||0, crit: c.CRITICAL||0 };
    }));

    // Stat cards
    const totals = rows.reduce((a,r) => ({ info:a.info+r.info, warn:a.warn+r.warn, error:a.error+r.error, crit:a.crit+r.crit }), {info:0,warn:0,error:0,crit:0});
    const si = document.getElementById('stat-info');     if (si) si.textContent = totals.info;
    const sw = document.getElementById('stat-warn');     if (sw) sw.textContent = totals.warn;
    const se = document.getElementById('stat-error');    if (se) se.textContent = totals.error;
    const sc = document.getElementById('stat-critical'); if (sc) sc.textContent = totals.crit;

    // Chart.js stacked bar
    const canvas = document.getElementById('metrics-chart');
    if (canvas && window.Chart) {
        if (metricsChart) { metricsChart.destroy(); metricsChart = null; }
        metricsChart = new Chart(canvas.getContext('2d'), {
            type: 'bar',
            data: {
                labels: rows.map(r => r.app),
                datasets: [
                    { label:'INFO',     data: rows.map(r=>r.info), backgroundColor:'#86efac', borderRadius:3 },
                    { label:'WARN',     data: rows.map(r=>r.warn), backgroundColor:'#fde68a', borderRadius:3 },
                    { label:'ERROR',    data: rows.map(r=>r.error), backgroundColor:'#fca5a5', borderRadius:3 },
                    { label:'CRITICAL', data: rows.map(r=>r.crit), backgroundColor:'#d8b4fe', borderRadius:3 },
                ]
            },
            options: {
                responsive: true, maintainAspectRatio: false,
                plugins: {
                    legend: {
                        position: 'top',
                        labels: { font:{ size:12, family:'Inter' }, boxWidth:12, padding:16, usePointStyle:true, pointStyle:'circle' }
                    },
                    tooltip: { mode:'index', intersect:false }
                },
                scales: {
                    x: { stacked:true, grid:{ display:false }, ticks:{ font:{ size:12, family:'Inter' }, color:'#64748b' }, border:{ display:false } },
                    y: { stacked:true, grid:{ color:'#f1f5f9' }, ticks:{ font:{ size:12, family:'Inter' }, color:'#64748b' }, border:{ display:false } }
                }
            }
        });
    }

    // Table
    tbody.innerHTML = rows.map(r => {
        const total = r.info + r.warn + r.error + r.crit;
        return '<tr>'
            + '<td style="padding:10px 16px;font-weight:500;font-size:13px">' + esc(r.app) + '</td>'
            + '<td style="padding:10px 16px;color:#15803d;font-weight:500;font-size:13px">' + r.info + '</td>'
            + '<td style="padding:10px 16px;color:#a16207;font-weight:500;font-size:13px">' + r.warn + '</td>'
            + '<td style="padding:10px 16px;color:#be123c;font-weight:500;font-size:13px">' + r.error + '</td>'
            + '<td style="padding:10px 16px;color:#7e22ce;font-weight:500;font-size:13px">' + r.crit + '</td>'
            + '<td style="padding:10px 16px;font-weight:700;font-size:13px;color:#0f172a">' + total + '</td>'
            + '</tr>';
    }).join('');

    // Prometheus endpoint
    const promEl = document.getElementById('prom-endpoint');
    if (promEl) promEl.textContent = window.location.origin + '/api/projects/' + pid + '/metrics';
}

function copyPromEndpoint() {
    const el = document.getElementById('prom-endpoint');
    if (el) { navigator.clipboard.writeText(el.textContent); showCopied('copy-prom-btn', 'Copied!'); }
}

// ── Alerts tab ────────────────────────────────────────────────────────────────
async function loadAlerts() {
    const pid = currentProjectId();
    if (!pid) return;
    const tbody = document.getElementById('alerts-tbody');
    tbody.innerHTML = loadingRow(6);
    const data = await api('/api/projects/' + pid + '/rules');
    const rules = (data && Array.isArray(data.rules)) ? data.rules : (Array.isArray(data) ? data : []);
    if (rules.length === 0) { tbody.innerHTML = emptyRow(6, 'No alert rules configured'); return; }
    tbody.innerHTML = rules.map(r => {
        const state = (r.state || 'INACTIVE').toUpperCase();
        return '<tr>'
            + '<td style="padding:10px 16px;font-size:13px"><code style="background:#f1f5f9;padding:2px 6px;border-radius:4px;font-size:11.5px">' + esc(r.id||'') + '</code></td>'
            + '<td style="padding:10px 16px;font-size:13px">' + esc(r.app||'') + '</td>'
            + '<td style="padding:10px 16px"><span class="pill pill-' + esc(r.level||'INFO') + '">' + esc(r.level||'') + '</span></td>'
            + '<td style="padding:10px 16px;font-size:13px;color:#374151">' + esc(String(r.threshold||'')) + '<span style="color:#94a3b8"> / ' + esc(String(r.window_seconds||'')) + 's</span></td>'
            + '<td style="padding:10px 16px;font-size:13px;color:#374151">' + esc(String(r.cooldown_seconds||'')) + 's</td>'
            + '<td style="padding:10px 16px"><span class="state-badge state-' + state + '"><span class="state-dot dot-' + state + '"></span>' + state + '</span></td>'
            + '</tr>';
    }).join('');
}

// ── Settings tab ──────────────────────────────────────────────────────────────
async function loadSettings() {
    const pid = currentProjectId();
    if (!pid) return;
    const proj = projects.find(p => p.id === pid);
    if (!proj) return;
    const el = n => document.getElementById(n);
    if (el('settings-name'))    el('settings-name').textContent    = proj.name;
    if (el('settings-id'))      el('settings-id').textContent      = proj.id;
    if (el('settings-apikey'))  el('settings-apikey').textContent  = proj.api_key || '—';
    if (el('settings-snippet')) el('settings-snippet').textContent =
        'curl -X POST ' + window.location.origin + '/ingest \\\n'
        + '  -H "X-API-Key: ' + proj.api_key + '" \\\n'
        + '  -H "Content-Type: application/json" \\\n'
        + '  -d \'{"app":"myapp","level":"ERROR","message":"disk full","timestamp":0}\'';
}

async function copyApiKey() {
    const el = document.getElementById('settings-apikey');
    if (el) { navigator.clipboard.writeText(el.textContent); showCopied('copy-key-btn','Copied!'); }
}

async function rotateApiKey() {
    const pid = currentProjectId();
    if (!pid || !confirm('Rotate API key?\nThe old key stops working immediately.')) return;
    const data = await api('/api/projects/' + pid + '/rotate-key', { method: 'POST' });
    if (!data || data.error) { alert(data && data.error ? data.error : 'Failed'); return; }
    const proj = projects.find(p => p.id === pid);
    if (proj) proj.api_key = data.api_key;
    loadSettings();
}

// ── Helpers ───────────────────────────────────────────────────────────────────
function esc(s) {
    return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function showCopied(btnId, msg) {
    const btn = document.getElementById(btnId);
    if (!btn) return;
    const orig = btn.textContent;
    btn.textContent = msg;
    setTimeout(() => { btn.textContent = orig; }, 1500);
}

// ── Init ──────────────────────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', function() {
    if (!getToken()) { location.href = '/'; return; }

    const emailEl = document.getElementById('user-email');
    if (emailEl) emailEl.textContent = getEmail() || '';

    const sel = document.getElementById('project-select');
    if (sel) sel.addEventListener('change', function() {
        currentProject = projects.find(p => p.id === sel.value) || null;
        localStorage.setItem('lm_proj', sel.value);
        showTab('logs');
    });

    document.querySelectorAll('[data-tab]').forEach(a => {
        a.addEventListener('click', function(e) {
            e.preventDefault(); showTab(this.dataset.tab);
        });
    });

    const logout = document.getElementById('btn-logout');
    if (logout) logout.addEventListener('click', function() { clearSession(); location.href = '/'; });

    const execBtn = document.getElementById('log-execute');
    if (execBtn) execBtn.addEventListener('click', function() { loadAppsForFilter(); loadLogs(); });

    loadProjects();
});
)js";
}

// ─── Login page ───────────────────────────────────────────────────────────────

inline std::string login_html() {
    return R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>LogMonitor — Sign In</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
  <link rel="stylesheet" href="/static/app.css">
  <style>
    body { background: #0f172a; }
    .login-label { display:block; font-size:12px; font-weight:600; color:#374151; margin-bottom:0; }
    .error-msg { font-size:12px; color:#be123c; margin-top:10px; min-height:18px; }
    .success-msg { font-size:12px; color:#15803d; margin-top:10px; }
    .field { margin-bottom:14px; }
  </style>
</head>
<body>
<div class="login-bg">
  <div style="width:100%;max-width:400px;position:relative;z-index:1">

    <!-- Logo -->
    <div style="text-align:center;margin-bottom:28px">
      <div style="font-size:30px;font-weight:700;color:white;letter-spacing:-0.5px">
        ◉ Log<span style="color:#f97316">Monitor</span>
      </div>
      <p style="font-size:13px;color:#64748b;margin-top:6px">Unified log observability platform</p>
    </div>

    <!-- Card -->
    <div class="login-card">
      <!-- Tab switcher -->
      <div style="display:flex;border-bottom:2px solid #f1f5f9;margin-bottom:24px">
        <button class="login-tab active" id="tab-signin" onclick="switchTab('signin')">Sign In</button>
        <button class="login-tab"        id="tab-signup" onclick="switchTab('signup')">Create Account</button>
      </div>

      <!-- Sign In -->
      <div id="form-signin">
        <div class="field">
          <label class="login-label">Email address</label>
          <input class="form-input" type="email" id="signin-email" placeholder="you@example.com" autocomplete="email">
        </div>
        <div class="field">
          <label class="login-label">Password</label>
          <input class="form-input" type="password" id="signin-password" placeholder="••••••••" autocomplete="current-password">
        </div>
        <button class="form-btn" onclick="doSignin()">Sign In</button>
        <div id="signin-error" class="error-msg"></div>
      </div>

      <!-- Sign Up -->
      <div id="form-signup" style="display:none">
        <div class="field">
          <label class="login-label">Email address</label>
          <input class="form-input" type="email" id="signup-email" placeholder="you@example.com" autocomplete="email">
        </div>
        <div class="field">
          <label class="login-label" style="display:flex;justify-content:space-between;align-items:center">
            Password <span style="font-weight:400;color:#94a3b8;font-size:11px">min 8 characters</span>
          </label>
          <input class="form-input" type="password" id="signup-password" placeholder="••••••••" autocomplete="new-password">
        </div>
        <button class="form-btn" onclick="doSignup()">Create Account</button>
        <div id="signup-error"   class="error-msg"></div>
        <div id="signup-success" class="success-msg"></div>
      </div>
    </div>

    <!-- Footer note -->
    <p style="text-align:center;font-size:12px;color:#475569;margin-top:20px">
      LogMonitor — open source log observability
    </p>
  </div>
</div>

<script>
'use strict';
function switchTab(name) {
    document.getElementById('form-signin').style.display = name==='signin' ? '' : 'none';
    document.getElementById('form-signup').style.display = name==='signup' ? '' : 'none';
    document.getElementById('tab-signin').className = 'login-tab' + (name==='signin'?' active':'');
    document.getElementById('tab-signup').className = 'login-tab' + (name==='signup'?' active':'');
}
async function doSignin() {
    const email    = document.getElementById('signin-email').value.trim();
    const password = document.getElementById('signin-password').value;
    const errEl    = document.getElementById('signin-error');
    errEl.textContent = '';
    if (!email || !password) { errEl.textContent = 'Please enter your email and password.'; return; }
    try {
        const r = await fetch('/auth/login', {
            method:'POST', headers:{'Content-Type':'application/json'},
            body: JSON.stringify({email, password})
        });
        const d = await r.json();
        if (!r.ok) { errEl.textContent = d.error || 'Login failed.'; return; }
        localStorage.setItem('lm_token',   d.token);
        localStorage.setItem('lm_user_id', d.user_id);
        localStorage.setItem('lm_email',   email);
        location.href = '/app';
    } catch(e) { errEl.textContent = 'Network error. Is LogMonitor running?'; }
}
async function doSignup() {
    const email    = document.getElementById('signup-email').value.trim();
    const password = document.getElementById('signup-password').value;
    const errEl    = document.getElementById('signup-error');
    const okEl     = document.getElementById('signup-success');
    errEl.textContent = ''; okEl.textContent = '';
    if (!email || !password) { errEl.textContent = 'Please enter email and password.'; return; }
    try {
        const r = await fetch('/auth/register', {
            method:'POST', headers:{'Content-Type':'application/json'},
            body: JSON.stringify({email, password})
        });
        const d = await r.json();
        if (!r.ok) { errEl.textContent = d.error || 'Registration failed.'; return; }
        okEl.textContent = 'Account created! Signing you in…';
        document.getElementById('signin-email').value    = email;
        document.getElementById('signin-password').value = password;
        setTimeout(() => { switchTab('signin'); doSignin(); }, 700);
    } catch(e) { errEl.textContent = 'Network error.'; }
}
document.addEventListener('keydown', function(e) {
    if (e.key !== 'Enter') return;
    if (document.getElementById('form-signup').style.display === 'none') doSignin();
    else doSignup();
});
if (localStorage.getItem('lm_token')) location.href = '/app';
</script>
</body>
</html>)html";
}

// ─── Dashboard shell ──────────────────────────────────────────────────────────

inline std::string app_html() {
    return R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>LogMonitor</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
  <script src="https://cdn.tailwindcss.com"></script>
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
  <link rel="stylesheet" href="/static/app.css">
  <style>
    [data-tab].active { background: rgba(249,115,22,0.18) !important; color: #fb923c !important; }
  </style>
</head>
<body class="flex overflow-hidden bg-slate-50" style="height:100vh;font-family:'Inter',sans-serif">

<!-- ════════════════════════════════ SIDEBAR ════════════════════════════════ -->
<aside class="flex flex-col flex-shrink-0 bg-slate-900" style="width:232px">

  <!-- Logo -->
  <div class="px-5 py-5 border-b border-slate-800">
    <a href="/app" class="text-[17px] font-bold text-white no-underline">
      ◉ Log<span class="text-orange-500">Monitor</span>
    </a>
    <p class="text-[10px] text-slate-600 mt-0.5 font-medium tracking-wider uppercase">Observability Platform</p>
  </div>

  <!-- Project selector -->
  <div class="px-3 py-3 border-b border-slate-800">
    <label class="block text-[10px] font-semibold uppercase tracking-widest text-slate-600 mb-1.5">Active Project</label>
    <select id="project-select"><option>Loading…</option></select>
  </div>

  <!-- Navigation -->
  <nav class="flex-1 px-2 py-3 space-y-0.5 overflow-y-auto">
    <a href="#" data-tab="logs" class="nav-item">
      <svg class="w-4 h-4 flex-shrink-0" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z"/>
      </svg>
      Log Explorer
    </a>
    <a href="#" data-tab="metrics" class="nav-item">
      <svg class="w-4 h-4 flex-shrink-0" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" d="M9 19v-6a2 2 0 00-2-2H5a2 2 0 00-2 2v6a2 2 0 002 2h2a2 2 0 002-2zm0 0V9a2 2 0 012-2h2a2 2 0 012 2v10m-6 0a2 2 0 002 2h2a2 2 0 002-2m0 0V5a2 2 0 012-2h2a2 2 0 012 2v14a2 2 0 01-2 2h-2a2 2 0 01-2-2z"/>
      </svg>
      Metrics
    </a>
    <a href="#" data-tab="alerts" class="nav-item">
      <svg class="w-4 h-4 flex-shrink-0" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" d="M15 17h5l-1.405-1.405A2.032 2.032 0 0118 14.158V11a6.002 6.002 0 00-4-5.659V5a2 2 0 10-4 0v.341C7.67 6.165 6 8.388 6 11v3.159c0 .538-.214 1.055-.595 1.436L4 17h5m6 0v1a3 3 0 11-6 0v-1m6 0H9"/>
      </svg>
      Alert Rules
    </a>
    <a href="#" data-tab="settings" class="nav-item">
      <svg class="w-4 h-4 flex-shrink-0" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z"/><circle cx="12" cy="12" r="3"/>
      </svg>
      Settings
    </a>
  </nav>

  <!-- User footer -->
  <div class="px-4 py-4 border-t border-slate-800">
    <div class="flex items-center gap-2 mb-2">
      <div class="w-7 h-7 rounded-full bg-orange-500 flex items-center justify-center flex-shrink-0">
        <svg class="w-3.5 h-3.5 text-white" fill="currentColor" viewBox="0 0 24 24">
          <path d="M12 12c2.7 0 4.8-2.1 4.8-4.8S14.7 2.4 12 2.4 7.2 4.5 7.2 7.2 9.3 12 12 12zm0 2.4c-3.2 0-9.6 1.6-9.6 4.8v2.4h19.2v-2.4c0-3.2-6.4-4.8-9.6-4.8z"/>
        </svg>
      </div>
      <p class="text-xs text-slate-400 truncate flex-1" id="user-email"></p>
    </div>
    <button id="btn-logout" class="text-[11px] text-slate-600 hover:text-slate-400 transition-colors flex items-center gap-1">
      <svg class="w-3 h-3" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" d="M17 16l4-4m0 0l-4-4m4 4H7m6 4v1a3 3 0 01-3 3H6a3 3 0 01-3-3V7a3 3 0 013-3h4a3 3 0 013 3v1"/>
      </svg>
      Sign out
    </button>
  </div>
</aside>

<!-- ═══════════════════════════ MAIN CONTENT ════════════════════════════════ -->
<main class="flex-1 overflow-y-auto">

  <!-- ── No project ── -->
  <section class="tab-panel active" id="tab-no-project">
    <div class="flex items-center justify-center min-h-screen">
      <div class="text-center max-w-sm mx-auto px-6">
        <div class="w-16 h-16 bg-orange-50 border-2 border-orange-100 rounded-2xl flex items-center justify-center mx-auto mb-5">
          <svg class="w-8 h-8 text-orange-500" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M12 4.5v15m7.5-7.5h-15"/>
          </svg>
        </div>
        <h2 class="text-xl font-semibold text-slate-800 mb-2">Create your first project</h2>
        <p class="text-sm text-slate-500 mb-6 leading-relaxed">
          Each project gets an isolated log store and a unique API key.
        </p>
        <input type="text" id="new-proj-name-welcome" placeholder="e.g. MyWebApp"
               class="w-full px-4 py-2.5 border border-slate-200 rounded-xl text-sm outline-none focus:border-orange-400 mb-3 text-slate-700">
        <button onclick="createProject('new-proj-name-welcome')"
                class="w-full py-2.5 bg-orange-500 hover:bg-orange-600 text-white font-semibold rounded-xl text-sm transition-colors shadow-sm">
          Create Project →
        </button>
      </div>
    </div>
  </section>

  <!-- ── Logs tab ── -->
  <section class="tab-panel" id="tab-logs">
    <div class="p-6">
      <div class="mb-5">
        <h1 class="text-xl font-semibold text-slate-800 tracking-tight">Log Explorer</h1>
        <p class="text-sm text-slate-400 mt-0.5">Real-time search and filter across your application logs</p>
      </div>

      <!-- Filter bar -->
      <div class="flex gap-2 mb-4 flex-wrap">
        <select id="log-app-filter"
                class="px-3 py-2 bg-white border border-slate-200 rounded-lg text-sm text-slate-700 outline-none focus:border-orange-400 cursor-pointer shadow-sm">
          <option value="">All Apps</option>
        </select>
        <select id="log-level-filter"
                class="px-3 py-2 bg-white border border-slate-200 rounded-lg text-sm text-slate-700 outline-none focus:border-orange-400 cursor-pointer shadow-sm">
          <option value="">All Levels</option>
          <option>INFO</option><option>WARN</option><option>ERROR</option><option>CRITICAL</option>
        </select>
        <select id="log-range-select"
                class="px-3 py-2 bg-white border border-slate-200 rounded-lg text-sm text-slate-700 outline-none focus:border-orange-400 cursor-pointer shadow-sm">
          <option value="300">Last 5 min</option>
          <option value="900">Last 15 min</option>
          <option value="3600" selected>Last 1 hour</option>
          <option value="21600">Last 6 hours</option>
          <option value="86400">Last 24 hours</option>
        </select>
        <button id="log-execute"
                class="px-5 py-2 bg-orange-500 hover:bg-orange-600 text-white text-sm font-semibold rounded-lg transition-colors shadow-sm">
          Execute
        </button>
      </div>

      <!-- Log table -->
      <div class="bg-white border border-slate-200 rounded-xl overflow-hidden shadow-sm">
        <table class="w-full border-collapse">
          <thead>
            <tr class="bg-slate-50 border-b border-slate-100">
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider" style="width:190px">Timestamp</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider" style="width:130px">App</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider" style="width:100px">Level</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">Message</th>
            </tr>
          </thead>
          <tbody id="logs-tbody">
            <tr><td colspan="4" class="px-4 py-8 text-center text-slate-300 text-sm">Loading…</td></tr>
          </tbody>
        </table>
      </div>
    </div>
  </section>

  <!-- ── Metrics tab ── -->
  <section class="tab-panel" id="tab-metrics">
    <div class="p-6">
      <div class="mb-5">
        <h1 class="text-xl font-semibold text-slate-800 tracking-tight">Metrics</h1>
        <p class="text-sm text-slate-400 mt-0.5">Log volume by application and severity (last 1 hour)</p>
      </div>

      <!-- Stat cards -->
      <div class="grid grid-cols-4 gap-3 mb-5">
        <div class="bg-white border border-slate-200 rounded-xl p-4 shadow-sm">
          <p class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-2">INFO</p>
          <p class="text-3xl font-bold text-green-600 leading-none" id="stat-info">—</p>
          <p class="text-[11px] text-slate-400 mt-1.5">events / 1h</p>
        </div>
        <div class="bg-white border border-slate-200 rounded-xl p-4 shadow-sm">
          <p class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-2">WARN</p>
          <p class="text-3xl font-bold text-yellow-600 leading-none" id="stat-warn">—</p>
          <p class="text-[11px] text-slate-400 mt-1.5">events / 1h</p>
        </div>
        <div class="bg-white border border-slate-200 rounded-xl p-4 shadow-sm">
          <p class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-2">ERROR</p>
          <p class="text-3xl font-bold text-red-600 leading-none" id="stat-error">—</p>
          <p class="text-[11px] text-slate-400 mt-1.5">events / 1h</p>
        </div>
        <div class="bg-white border border-slate-200 rounded-xl p-4 shadow-sm">
          <p class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-2">CRITICAL</p>
          <p class="text-3xl font-bold text-purple-600 leading-none" id="stat-critical">—</p>
          <p class="text-[11px] text-slate-400 mt-1.5">events / 1h</p>
        </div>
      </div>

      <!-- Chart -->
      <div class="bg-white border border-slate-200 rounded-xl p-5 shadow-sm mb-4">
        <p class="text-xs font-semibold text-slate-400 uppercase tracking-wider mb-4">Log Volume by Application</p>
        <div style="position:relative;height:230px">
          <canvas id="metrics-chart"></canvas>
        </div>
      </div>

      <!-- Prometheus endpoint -->
      <div class="bg-white border border-slate-200 rounded-xl p-5 shadow-sm mb-4">
        <p class="text-xs font-semibold text-slate-400 uppercase tracking-wider mb-1">Prometheus Scrape Endpoint</p>
        <p class="text-xs text-slate-400 mb-3">Add this URL to your <code class="bg-slate-100 px-1 py-0.5 rounded text-slate-600">prometheus.yml</code> scrape_configs:</p>
        <div class="flex items-center gap-2">
          <code id="prom-endpoint" class="flex-1 text-xs bg-slate-50 border border-slate-200 rounded-lg px-3 py-2 text-slate-600 font-mono overflow-x-auto"></code>
          <button id="copy-prom-btn" onclick="copyPromEndpoint()"
                  class="px-3 py-2 text-xs text-slate-600 bg-white border border-slate-200 rounded-lg hover:bg-slate-50 transition-colors font-medium flex-shrink-0">Copy</button>
        </div>
      </div>

      <!-- App breakdown table -->
      <div class="bg-white border border-slate-200 rounded-xl overflow-hidden shadow-sm">
        <table class="w-full border-collapse">
          <thead>
            <tr class="bg-slate-50 border-b border-slate-100">
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">Application</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-green-600 uppercase tracking-wider">INFO</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-yellow-600 uppercase tracking-wider">WARN</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-red-600 uppercase tracking-wider">ERROR</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-purple-600 uppercase tracking-wider">CRITICAL</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">Total</th>
            </tr>
          </thead>
          <tbody id="metrics-tbody">
            <tr><td colspan="6" class="px-4 py-8 text-center text-slate-300 text-sm">Loading…</td></tr>
          </tbody>
        </table>
      </div>
    </div>
  </section>

  <!-- ── Alerts tab ── -->
  <section class="tab-panel" id="tab-alerts">
    <div class="p-6">
      <div class="mb-5">
        <h1 class="text-xl font-semibold text-slate-800 tracking-tight">Alert Rules</h1>
        <p class="text-sm text-slate-400 mt-0.5">Configured alerting rules and their current state</p>
      </div>
      <div class="bg-white border border-slate-200 rounded-xl overflow-hidden shadow-sm">
        <table class="w-full border-collapse">
          <thead>
            <tr class="bg-slate-50 border-b border-slate-100">
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">Rule ID</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">App</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">Level</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">Threshold / Window</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">Cooldown</th>
              <th class="px-4 py-3 text-left text-[11px] font-semibold text-slate-400 uppercase tracking-wider">State</th>
            </tr>
          </thead>
          <tbody id="alerts-tbody">
            <tr><td colspan="6" class="px-4 py-8 text-center text-slate-300 text-sm">Loading…</td></tr>
          </tbody>
        </table>
      </div>
    </div>
  </section>

  <!-- ── Settings tab ── -->
  <section class="tab-panel" id="tab-settings">
    <div class="p-6" style="max-width:680px">
      <div class="mb-5">
        <h1 class="text-xl font-semibold text-slate-800 tracking-tight">Project Settings</h1>
      </div>

      <!-- Project Info -->
      <div class="bg-white border border-slate-200 rounded-xl p-5 shadow-sm mb-4">
        <p class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-3">Project Info</p>
        <div class="space-y-2.5">
          <div class="flex items-center gap-3">
            <span class="text-sm text-slate-400 w-24">Name</span>
            <span class="text-sm font-semibold text-slate-800" id="settings-name"></span>
          </div>
          <div class="flex items-center gap-3">
            <span class="text-sm text-slate-400 w-24">Project ID</span>
            <code class="text-xs text-slate-500 bg-slate-50 px-2 py-1 rounded-md" id="settings-id"></code>
          </div>
        </div>
      </div>

      <!-- API Key -->
      <div class="bg-white border border-slate-200 rounded-xl p-5 shadow-sm mb-4">
        <p class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-1">API Key</p>
        <p class="text-xs text-slate-400 mb-3">Send this in the <code class="bg-slate-100 px-1 py-0.5 rounded text-slate-600">X-API-Key</code> header with every log ingest request.</p>
        <div class="flex items-center gap-2 bg-slate-50 border border-slate-200 rounded-xl px-4 py-3 mb-3">
          <span class="flex-1 text-sm font-mono text-slate-700 break-all leading-relaxed" id="settings-apikey"></span>
          <button id="copy-key-btn" onclick="copyApiKey()"
                  class="px-3 py-1.5 text-xs text-slate-600 bg-white border border-slate-200 rounded-lg hover:bg-slate-50 transition-colors font-medium flex-shrink-0">Copy</button>
          <button onclick="rotateApiKey()"
                  class="px-3 py-1.5 text-xs text-slate-600 bg-white border border-slate-200 rounded-lg hover:bg-slate-50 transition-colors font-medium flex-shrink-0">Rotate</button>
        </div>
      </div>

      <!-- Quick Start -->
      <div class="bg-white border border-slate-200 rounded-xl p-5 shadow-sm mb-4">
        <p class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-3">Quick Start</p>
        <pre id="settings-snippet" class="code-block text-xs"></pre>
      </div>

      <!-- New Project -->
      <div class="bg-white border border-slate-200 rounded-xl p-5 shadow-sm mb-4">
        <p class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-3">New Project</p>
        <div class="flex gap-2">
          <input type="text" id="new-proj-name" placeholder="Project name"
                 class="flex-1 px-3 py-2 border border-slate-200 rounded-lg text-sm outline-none focus:border-orange-400 text-slate-700">
          <button onclick="createProject()"
                  class="px-4 py-2 bg-orange-500 hover:bg-orange-600 text-white text-sm font-semibold rounded-lg transition-colors">Create</button>
        </div>
      </div>

      <!-- Danger Zone -->
      <div class="bg-red-50 border border-red-200 rounded-xl p-5">
        <p class="text-[11px] font-semibold text-red-500 uppercase tracking-wider mb-1">Danger Zone</p>
        <p class="text-xs text-slate-500 mb-3 leading-relaxed">Permanently delete this project, its API key, and all associated log data. This action cannot be undone.</p>
        <button onclick="deleteCurrentProject()"
                class="px-4 py-2 bg-red-600 hover:bg-red-700 text-white text-sm font-semibold rounded-lg transition-colors">
          Delete Project
        </button>
      </div>
    </div>
  </section>

</main>

<script src="/static/app.js"></script>
</body>
</html>)html";
}

} // namespace webui
} // namespace logmonitor
