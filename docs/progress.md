---
title: Port progress
nav_order: 9
---

# Port progress

How much of the game has been rewritten as native C++ (the [native
port](NATIVE_PORT_STATUS.html), roadmap phase 6). The recompiled game has
about 22,400 functions; each one is replaced by a hand-written version at
a time, and the rest keep running as recompiled code. A function counts as
**verified** once replaying calls recorded from Theater films through both
versions gives the same result on every call; the others still have no
recorded calls that reach them.

<div class="progress-page" id="progress-page">
  <div class="pp-tiles" id="pp-tiles"></div>
  <div class="pp-overall" id="pp-overall"></div>

  <h2>Over time</h2>
  <div class="pp-chart" id="pp-history"></div>

  <h2>By subsystem</h2>
  <p class="pp-note">Subsystems come from a coarse map of the executable's code
  ranges: approximate, and code outside the mapped ranges counts as "other".
  Each bar runs from 0 to all of that subsystem's functions; the line under
  it is its count of rewritten functions over time.</p>
  <div class="pp-legend" aria-hidden="true">
    <span><i class="pp-swatch pp-verified"></i>Verified</span>
    <span><i class="pp-swatch pp-unverified"></i>Rewritten, not yet verified</span>
  </div>
  <div class="pp-grid" id="pp-grid"></div>

  <details class="pp-table">
    <summary>Table view</summary>
    <table>
      <thead><tr><th>Subsystem</th><th>Functions</th><th>Rewritten</th><th>Verified</th><th>%</th></tr></thead>
      <tbody id="pp-rows"></tbody>
    </table>
  </details>
  <p class="pp-note" id="pp-updated"></p>
  <div class="pp-tooltip" id="pp-tooltip" role="status" hidden></div>
</div>

<style>
  .progress-page {
    --pp-surface: #0b1a2b;
    --pp-card: rgba(11, 26, 43, 0.72);
    --pp-grid: #1b3149;
    --pp-track: #142a40;
    --pp-text: #b4c8dc;
    --pp-strong: #e3eefa;
    --pp-muted: #8fa9c2;
    --pp-verified: #9ccaf0;
    --pp-unverified: #4f7fae;
    color: var(--pp-text);
  }
  .pp-tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 12px; margin: 1.2rem 0; }
  .pp-tile { background: var(--pp-card); border: 1px solid var(--pp-grid); border-radius: 6px; padding: 14px 16px; }
  .pp-tile .pp-value { font-family: "Barlow Semi Condensed", "Barlow", system-ui, sans-serif; font-size: 2.2rem; font-weight: 500; color: var(--pp-strong); line-height: 1.1; }
  .pp-tile .pp-label { font-size: 0.9rem; color: var(--pp-muted); margin-top: 4px; }
  .pp-overall { margin: 0.4rem 0 1.4rem; }
  .pp-bar { display: flex; gap: 2px; height: 12px; background: var(--pp-track); border-radius: 4px; overflow: hidden; }
  .pp-bar.pp-big { height: 18px; }
  .pp-seg { height: 100%; min-width: 2px; }
  .pp-seg.pp-verified, .pp-swatch.pp-verified { background: var(--pp-verified); }
  .pp-seg.pp-unverified, .pp-swatch.pp-unverified { background: var(--pp-unverified); }
  .pp-bar-caption { font-size: 0.9rem; color: var(--pp-muted); margin-top: 6px; }
  .pp-chart { background: var(--pp-card); border: 1px solid var(--pp-grid); border-radius: 6px; padding: 12px; }
  .pp-chart svg { display: block; width: 100%; height: auto; overflow: visible; }
  .pp-legend { display: flex; flex-wrap: wrap; gap: 18px; font-size: 0.9rem; margin: 0.2rem 0 0.8rem; }
  .pp-legend span { display: inline-flex; align-items: center; gap: 6px; }
  .pp-swatch { display: inline-block; width: 12px; height: 12px; border-radius: 3px; }
  .pp-line-swatch { display: inline-block; width: 16px; height: 2px; border-radius: 1px; }
  .pp-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(250px, 1fr)); gap: 12px; }
  .pp-card { background: var(--pp-card); border: 1px solid var(--pp-grid); border-radius: 6px; padding: 12px 14px; }
  .pp-card h3 { font-size: 0.98rem !important; margin: 0 0 6px !important; color: var(--pp-strong); line-height: 1.3; min-height: 2.6em; }
  .pp-card .pp-count { font-size: 0.88rem; color: var(--pp-muted); margin: 0 0 8px; }
  .pp-card .pp-count b { color: var(--pp-strong); font-weight: 500; font-size: 1.15rem; }
  .pp-card svg { display: block; width: 100%; height: auto; margin-top: 10px; overflow: visible; }
  .pp-note { font-size: 0.92rem; color: var(--pp-muted); }
  .pp-table { margin: 1.4rem 0 0.6rem; }
  .pp-table summary { cursor: pointer; color: var(--pp-strong); }
  .pp-table td:not(:first-child), .pp-table th:not(:first-child) { text-align: right; }
  .pp-tooltip {
    position: fixed; z-index: 10; pointer-events: none; background: #050d18; color: var(--pp-strong);
    border: 1px solid var(--pp-grid); border-radius: 4px; padding: 6px 9px; font-size: 0.85rem; line-height: 1.35;
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.45); max-width: 260px;
  }
  .pp-tooltip .pp-dim { color: var(--pp-muted); }
  .pp-hit { cursor: crosshair; }
</style>

<script>window.PROGRESS_DATA = {{ site.data.progress | jsonify }};</script>
<script src="{{ '/assets/js/progress.js' | relative_url }}"></script>
