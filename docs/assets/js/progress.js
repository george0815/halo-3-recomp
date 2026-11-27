// Draws docs/progress.md from window.PROGRESS_DATA (docs/_data/progress.json).
// Kept out of the page: just-the-docs compresses page HTML onto one line,
// which turns everything after an inline // comment into a comment.
(function () {
  var data = window.PROGRESS_DATA;
  if (!data) return;
  var SVGNS = "http://www.w3.org/2000/svg";
  var fmt = function (n) { return n == null ? "-" : n.toLocaleString("en-US"); };
  var pct = function (part, whole) {
    if (!whole) return "0%";
    var p = 100 * part / whole;
    return (p === 0 ? "0" : p < 0.1 ? "<0.1" : p.toFixed(p < 10 ? 2 : 1)) + "%";
  };
  var el = function (tag, attrs, parent) {
    var node = document.createElementNS(SVGNS, tag);
    for (var k in attrs) node.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(node);
    return node;
  };
  var shortDate = function (iso) {
    var d = new Date(iso + "T00:00:00");
    return d.toLocaleDateString("en-US", { month: "short", day: "numeric" });
  };
  var css = getComputedStyle(document.getElementById("progress-page"));
  var C = {
    grid: css.getPropertyValue("--pp-grid").trim(),
    surface: css.getPropertyValue("--pp-surface").trim(),
    muted: css.getPropertyValue("--pp-muted").trim(),
    text: css.getPropertyValue("--pp-text").trim(),
    verified: css.getPropertyValue("--pp-verified").trim(),
    unverified: css.getPropertyValue("--pp-unverified").trim()
  };

  // Tooltip shared by every chart.
  var tip = document.getElementById("pp-tooltip");
  function showTip(html, event) {
    tip.innerHTML = html;
    tip.hidden = false;
    var x = event.clientX + 14, y = event.clientY + 14;
    var w = tip.offsetWidth, h = tip.offsetHeight;
    if (x + w > window.innerWidth - 8) x = event.clientX - w - 14;
    if (y + h > window.innerHeight - 8) y = event.clientY - h - 14;
    tip.style.left = x + "px";
    tip.style.top = y + "px";
  }
  function hideTip() { tip.hidden = true; }

  var t = data.total;
  var hasVerified = t.verified != null;

  // Stat tiles.
  var tiles = [
    [fmt(t.replaced), "functions rewritten, of " + fmt(t.functions)],
    [hasVerified ? fmt(t.verified) : "-", "verified by call replay"],
    [pct(t.replaced, t.functions), "of all functions (" + pct(t.replaced_bytes, t.bytes) + " of the code)"]
  ];
  var tileBox = document.getElementById("pp-tiles");
  tiles.forEach(function (tile) {
    var div = document.createElement("div");
    div.className = "pp-tile";
    div.innerHTML = '<div class="pp-value">' + tile[0] + '</div><div class="pp-label">' + tile[1] + "</div>";
    tileBox.appendChild(div);
  });

  // A 0-100% bar: verified, then rewritten but unverified.
  function bar(done, verified, whole, big, label) {
    var wrap = document.createElement("div");
    wrap.className = "pp-bar" + (big ? " pp-big" : "");
    wrap.setAttribute("role", "img");
    wrap.setAttribute("aria-label", label + ": " + done + " of " + whole + " rewritten" +
      (verified != null ? ", " + verified + " verified" : ""));
    var v = verified == null ? 0 : verified;
    [[v, "pp-verified", "Verified"], [done - v, "pp-unverified", verified == null ? "Rewritten" : "Rewritten, not yet verified"]]
      .forEach(function (part) {
        if (part[0] <= 0) return;
        var seg = document.createElement("div");
        seg.className = "pp-seg " + part[1];
        seg.style.width = (100 * part[0] / whole) + "%";
        seg.addEventListener("mousemove", function (e) {
          showTip("<b>" + label + "</b><br>" + part[2] + ": " + fmt(part[0]) + ' <span class="pp-dim">of ' + fmt(whole) + "</span>", e);
        });
        seg.addEventListener("mouseleave", hideTip);
        wrap.appendChild(seg);
      });
    return wrap;
  }
  var overall = document.getElementById("pp-overall");
  overall.appendChild(bar(t.replaced, t.verified, t.functions, true, "All functions"));
  var cap = document.createElement("div");
  cap.className = "pp-bar-caption";
  cap.textContent = "The whole game: " + fmt(t.replaced) + " of " + fmt(t.functions) + " functions (" + pct(t.replaced, t.functions) + ").";
  overall.appendChild(cap);

  // Line chart of history: rewritten and verified, one y axis (functions).
  function lineChart(box, series, opts) {
    var W = opts.width, H = opts.height, m = opts.margin;
    var svg = el("svg", { viewBox: "0 0 " + W + " " + H, role: "img", "aria-label": opts.label });
    box.appendChild(svg);
    var dates = data.history.map(function (h) { return new Date(h.date + "T00:00:00").getTime(); });
    var x0 = Math.min.apply(null, dates), x1 = Math.max.apply(null, dates);
    if (x1 === x0) x1 = x0 + 86400000;
    var maxY = 0;
    series.forEach(function (s) { s.values.forEach(function (v) { if (v != null && v > maxY) maxY = v; }); });
    var step = niceStep(maxY || 1, opts.ticks);
    var yMax = Math.max(step, Math.ceil((maxY || 1) / step) * step);
    var X = function (t) { return m.l + (W - m.l - m.r) * (t - x0) / (x1 - x0); };
    var Y = function (v) { return H - m.b - (H - m.t - m.b) * v / yMax; };
    if (opts.axes) {
      for (var v = 0; v <= yMax + 1e-9; v += step) {
        el("line", { x1: m.l, x2: W - m.r, y1: Y(v), y2: Y(v), stroke: C.grid, "stroke-width": 1 }, svg);
        var lbl = el("text", { x: m.l - 8, y: Y(v) + 4, "text-anchor": "end", fill: C.muted, "font-size": 12 }, svg);
        lbl.textContent = fmt(v);
      }
      var first = el("text", { x: m.l, y: H - 6, fill: C.muted, "font-size": 12 }, svg);
      first.textContent = shortDate(data.history[0].date);
      var last = el("text", { x: W - m.r, y: H - 6, fill: C.muted, "font-size": 12, "text-anchor": "end" }, svg);
      last.textContent = shortDate(data.history[data.history.length - 1].date);
    } else {
      el("line", { x1: m.l, x2: W - m.r, y1: Y(0), y2: Y(0), stroke: C.grid, "stroke-width": 1 }, svg);
    }
    series.forEach(function (s) {
      var pts = [];
      s.values.forEach(function (v, i) { if (v != null) pts.push([X(dates[i]), Y(v)]); });
      if (!pts.length) return;
      // Steps: a count holds until the next commit changes it.
      var d = "M" + pts[0][0] + "," + pts[0][1];
      for (var i = 1; i < pts.length; i++) d += "H" + pts[i][0] + "V" + pts[i][1];
      el("path", { d: d, fill: "none", stroke: s.color, "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round" }, svg);
      var end = pts[pts.length - 1];
      el("circle", { cx: end[0], cy: end[1], r: 4, fill: s.color, stroke: C.surface, "stroke-width": 2 }, svg);
      if (opts.endLabels) {
        var tx = el("text", { x: end[0] - 8, y: end[1] + (s.labelBelow ? 18 : -10), "text-anchor": "end", fill: C.text, "font-size": 12 }, svg);
        tx.textContent = s.name + " " + fmt(s.values[s.values.length - 1]);
      }
    });
    // Hover: the nearest date, every series' value there.
    var hit = el("rect", { x: m.l, y: m.t, width: W - m.l - m.r, height: H - m.t - m.b, fill: "transparent", "class": "pp-hit" }, svg);
    var cross = el("line", { y1: m.t, y2: H - m.b, stroke: C.muted, "stroke-width": 1, visibility: "hidden" }, svg);
    hit.addEventListener("mousemove", function (e) {
      var r = svg.getBoundingClientRect();
      var px = (e.clientX - r.left) * W / r.width;
      var best = 0;
      dates.forEach(function (t, i) { if (Math.abs(X(t) - px) < Math.abs(X(dates[best]) - px)) best = i; });
      cross.setAttribute("x1", X(dates[best]));
      cross.setAttribute("x2", X(dates[best]));
      cross.setAttribute("visibility", "visible");
      var html = "<b>" + shortDate(data.history[best].date) + "</b>" + (opts.title ? ' <span class="pp-dim">' + opts.title + "</span>" : "");
      series.forEach(function (s) {
        html += "<br>" + s.name + ": " + (s.values[best] == null ? '<span class="pp-dim">not measured</span>' : fmt(s.values[best]));
      });
      showTip(html, e);
    });
    hit.addEventListener("mouseleave", function () { cross.setAttribute("visibility", "hidden"); hideTip(); });
    return svg;
  }
  function niceStep(max, ticks) {
    var raw = max / ticks, mag = Math.pow(10, Math.floor(Math.log10(raw)));
    var n = raw / mag;
    return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 5 ? 5 : 10) * mag;
  }

  var historyBox = document.getElementById("pp-history");
  var legend = document.createElement("div");
  legend.className = "pp-legend";
  legend.innerHTML = '<span><i class="pp-line-swatch" style="background:' + C.unverified + '"></i>Rewritten</span>' +
    '<span><i class="pp-line-swatch" style="background:' + C.verified + '"></i>Verified (measured from Nov 26)</span>';
  historyBox.appendChild(legend);
  lineChart(historyBox, [
    { name: "Rewritten", color: C.unverified, values: data.history.map(function (h) { return h.replaced; }) },
    { name: "Verified", color: C.verified, values: data.history.map(function (h) { return h.verified; }), labelBelow: true }
  ], { width: Math.max(300, historyBox.clientWidth - 24), height: 260, margin: { t: 16, r: 16, b: 28, l: 52 }, ticks: 4, axes: true, endLabels: true,
       label: "Functions rewritten and verified over time" });

  // Small multiples: one card per subsystem.
  var grid = document.getElementById("pp-grid");
  var rows = document.getElementById("pp-rows");
  data.subsystems.forEach(function (s) {
    var card = document.createElement("div");
    card.className = "pp-card";
    var h = document.createElement("h3");
    h.textContent = s.name;
    card.appendChild(h);
    var count = document.createElement("p");
    count.className = "pp-count";
    count.innerHTML = "<b>" + fmt(s.replaced) + "</b> of " + fmt(s.functions) + " · " + pct(s.replaced, s.functions) +
      (s.verified != null ? " · " + fmt(s.verified) + " verified" : "");
    card.appendChild(count);
    card.appendChild(bar(s.replaced, s.verified, s.functions, false, s.name));
    var values = data.history.map(function (hh) { return hh.subsystems[s.name] || 0; });
    lineChart(card, [{ name: "Rewritten", color: C.unverified, values: values }],
      { width: Math.max(200, grid.clientWidth > 600 ? 240 : grid.clientWidth - 28), height: 56, margin: { t: 6, r: 6, b: 4, l: 6 }, ticks: 2, axes: false, endLabels: false,
        title: s.name, label: s.name + ": functions rewritten over time" });
    grid.appendChild(card);

    var tr = document.createElement("tr");
    [s.name, fmt(s.functions), fmt(s.replaced), fmt(s.verified), pct(s.replaced, s.functions)].forEach(function (cell) {
      var td = document.createElement("td");
      td.textContent = cell;
      tr.appendChild(td);
    });
    rows.appendChild(tr);
  });

  document.getElementById("pp-updated").textContent = "Updated " +
    new Date(data.updated + "T00:00:00").toLocaleDateString("en-US", { year: "numeric", month: "long", day: "numeric" }) +
    ". Counts come from tools/re/progress.py; verification from tools/films/replay_calls.sh.";
})();
