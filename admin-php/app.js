/* Standalone admin frontend — talks to api.php. Compatible with older browsers
   (ES5-style, no template literals; uses closest()/fetch which are Edge 15+). */
(function () {
  'use strict';

  var CSRF = '';
  var logRows = [], logSeen = {}, logPage = 1, logSize = 50, logFrom = '', logTo = '', logMin = '', logMax = '';
  var pendingLog = null; // set when navigating from Usage -> Logs for one user
  var perfTimer = null, logsTimer = null, dashTimer = null;
  var HIST = 90, hist = { cpu: [], mem: [], gpu: [], srv: [] };

  // ---- helpers -------------------------------------------------------------
  function esc(s) { return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]; }); }
  function fmtNum(n) { return ('' + n).replace(/\B(?=(\d{3})+(?!\d))/g, ','); }
  function fmtBytes(b) { b = +b || 0; var u = ['B', 'KB', 'MB', 'GB', 'TB'], i = 0; while (b >= 1024 && i < u.length - 1) { b /= 1024; i++; } return (b < 10 && i > 0 ? b.toFixed(1) : Math.round(b)) + ' ' + u[i]; }
  function setMain(h) { document.getElementById('main').innerHTML = h; }
  // ---- styled confirm/alert (replaces the browser's native dialogs) ---------
  function uiDialog(o) {
    return new Promise(function (resolve) {
      var ov = document.createElement('div'); ov.className = 'uidlg-overlay';
      ov.innerHTML = '<div class="uidlg" role="dialog" aria-modal="true">' +
        (o.title ? '<div class="uidlg-title">' + esc(o.title) + '</div>' : '') +
        '<div class="uidlg-msg">' + esc(o.message || '').replace(/\n/g, '<br>') + '</div>' +
        '<div class="uidlg-btns">' +
        (o.alert ? '' : '<button type="button" class="btn ghost" data-uid="0">' + esc(o.cancelText || 'Cancel') + '</button>') +
        '<button type="button" class="btn' + (o.danger ? ' danger' : '') + '" data-uid="1">' + esc(o.okText || 'OK') + '</button></div></div>';
      document.body.appendChild(ov);
      function done(v) { document.removeEventListener('keydown', onkey); if (ov.parentNode) ov.parentNode.removeChild(ov); resolve(v); }
      function onkey(e) { if (e.key === 'Escape') { e.preventDefault(); done(false); } else if (e.key === 'Enter') { e.preventDefault(); done(true); } }
      ov.addEventListener('click', function (e) { var b = e.target.closest ? e.target.closest('[data-uid]') : null; if (b) { done(b.getAttribute('data-uid') === '1'); } else if (e.target === ov) { done(false); } });
      document.addEventListener('keydown', onkey);
      var ok = ov.querySelector('[data-uid="1"]'); if (ok) ok.focus();
    });
  }
  function uiConfirm(message, o) { o = o || {}; o.message = message; return uiDialog(o); }
  function uiAlert(message, o) { o = o || {}; o.message = message; o.alert = true; return uiDialog(o); }
  function setText(id, t) { var e = document.getElementById(id); if (e) e.textContent = t; }
  function showError(err) { setMain('<div class="card"><div class="flash err">' + esc(err) + '</div></div>'); }

  // ---- top loading bar (NProgress-style) -----------------------------------
  var _busy = 0, _barEl = null, _barShow = null, _barHide = null;
  function loadStart() {
    _busy++;
    if (_busy === 1 && !_barShow) {
      _barShow = setTimeout(function () {                 // debounce: skip a flash for quick calls
        _barShow = null;
        if (!_barEl) { _barEl = document.createElement('div'); _barEl.className = 'loadbar'; document.body.appendChild(_barEl); }
        var b = _barEl; if (_barHide) { clearTimeout(_barHide); _barHide = null; }
        b.style.transition = 'none'; b.style.width = '6%'; b.classList.add('on'); void b.offsetWidth;
        b.style.transition = 'width 8s cubic-bezier(.05,.7,.05,1), opacity .3s'; b.style.width = '85%';
      }, 140);
    }
  }
  function loadDone() {
    if (_busy > 0) _busy--;
    if (_busy !== 0) return;
    if (_barShow) { clearTimeout(_barShow); _barShow = null; }
    var b = _barEl;
    if (b && b.classList.contains('on')) {
      b.style.transition = 'width .25s ease, opacity .4s ease .2s'; b.style.width = '100%'; b.classList.remove('on');
      _barHide = setTimeout(function () { if (_busy === 0 && b) { b.style.transition = 'none'; b.style.width = '0%'; } _barHide = null; }, 650);
    }
  }
  function api(action, opts) {
    opts = opts || {};
    var url = 'api.php?action=' + encodeURIComponent(action);
    if (opts.query) { for (var k in opts.query) { if (opts.query.hasOwnProperty(k)) url += '&' + k + '=' + encodeURIComponent(opts.query[k]); } }
    var init = { credentials: 'same-origin', headers: {} };
    if (opts.body) { init.method = 'POST'; init.headers['Content-Type'] = 'application/json'; init.headers['X-CSRF'] = CSRF; init.body = JSON.stringify(opts.body); }
    if (!opts.quiet) loadStart();
    var p = fetch(url, init).then(function (r) {
      return r.json().then(function (j) { if (!r.ok) { throw (j && j.error) || ('HTTP ' + r.status); } return j; });
    });
    if (!opts.quiet) { p = p.then(function (v) { loadDone(); return v; }, function (e) { loadDone(); throw e; }); }
    return p;
  }

  // ---- theme / tooltip / modal (global handlers) ---------------------------
  window.toggleTheme = function () {
    var d = document.documentElement;
    var next = d.getAttribute('data-theme') === 'light' ? 'dark' : 'light';
    if (next === 'light') d.setAttribute('data-theme', 'light'); else d.removeAttribute('data-theme');
    try { localStorage.setItem('admin-theme', next); } catch (e) {}
    if (document.getElementById('cpugraph')) redrawPerf();
  };

  (function () {
    var tip = document.getElementById('charttip');
    function pos(e) {
      var x = e.clientX, y = e.clientY; tip.style.left = (x + 14) + 'px'; tip.style.top = (y + 14) + 'px';
      var r = tip.getBoundingClientRect();
      if (r.right > window.innerWidth) tip.style.left = (x - r.width - 14) + 'px';
      if (r.bottom > window.innerHeight) tip.style.top = (y - r.height - 14) + 'px';
    }
    document.addEventListener('mousemove', function (e) {
      var node = e.target && e.target.closest ? e.target.closest('[data-tip]') : null;
      if (node) { tip.textContent = node.getAttribute('data-tip'); tip.style.display = 'block'; pos(e); }
      else if (tip.style.display === 'block') tip.style.display = 'none';
    });
  })();

  function renderMarkdown(src) {
    src = String(src || ''); var blocks = [];
    src = src.replace(/```(\w*)\r?\n?([\s\S]*?)```/g, function (_, l, code) { blocks.push('<pre class="code"><code>' + esc(code.replace(/\s+$/, '')) + '</code></pre>'); return '@@' + (blocks.length - 1) + '@@'; });
    src = esc(src);
    src = src.replace(/^######\s+(.*)$/gm, '<h6>$1</h6>').replace(/^#####\s+(.*)$/gm, '<h5>$1</h5>').replace(/^####\s+(.*)$/gm, '<h4>$1</h4>').replace(/^###\s+(.*)$/gm, '<h3>$1</h3>').replace(/^##\s+(.*)$/gm, '<h2>$1</h2>').replace(/^#\s+(.*)$/gm, '<h1>$1</h1>');
    src = src.replace(/`([^`]+)`/g, '<code>$1</code>');
    src = src.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>').replace(/\*([^*\n]+)\*/g, '<em>$1</em>');
    src = '<p>' + src.replace(/\n{2,}/g, '</p><p>') + '</p>';
    src = src.replace(/\n/g, '<br>');
    src = src.replace(/@@(\d+)@@/g, function (_, i) { return blocks[+i]; });
    src = src.replace(/<p>(\s*<(?:h[1-6]|pre)[\s\S]*?<\/(?:h[1-6]|pre)>\s*)<\/p>/g, '$1');
    return src;
  }
  window.closeDetail = function () { document.getElementById('modal').classList.remove('open'); };
  window.toggleRaw = function () { var r = document.getElementById('m-raw'); r.hidden = !r.hidden; };
  function openDetail(sig) {
    var d = null; for (var i = 0; i < logRows.length; i++) { if (logRows[i].sig === sig) { d = logRows[i]; break; } }
    if (!d) return;
    var stcls = d.status >= 500 ? 'st5' : (d.status >= 400 ? 'st4' : 'st2');
    document.getElementById('m-meta').innerHTML = '<strong>' + esc(d.user) + '</strong> <span class="badge ' + stcls + '">' + d.status + '</span> <code>' + esc(d.method + ' ' + d.path) + '</code> <span class="muted">' + esc(d.ts) + (d.latency ? ' · ' + esc(d.latency) : '') + '</span>';
    var body = document.getElementById('m-body'); body.innerHTML = '';
    if (d.conversation && d.conversation.length) {
      d.conversation.forEach(function (t) { var x = document.createElement('div'); x.className = 'msg msg-' + (t.role || 'user'); x.innerHTML = '<div class="role">' + esc(t.role || '') + '</div><div class="content">' + renderMarkdown(t.content) + '</div>'; body.appendChild(x); });
    } else { body.innerHTML = '<p class="muted">No chat content for this request.</p>'; }
    var raw = document.getElementById('m-raw'); raw.hidden = true; raw.textContent = 'REQUEST:\n' + (d.requestRaw || '(empty)') + '\n\nRESPONSE:\n' + (d.responseRaw || '(empty)');
    var sb = document.getElementById('m-suspend');
    if (sb) { if (d.userId) { sb.hidden = false; sb.setAttribute('data-id', d.userId); sb.setAttribute('data-name', d.user || d.userId); } else { sb.hidden = true; } }
    document.getElementById('modal').classList.add('open');
  }
  document.addEventListener('keydown', function (e) { if (e.key === 'Escape') { window.closeDetail(); if (window.closeHostModal) { window.closeHostModal(); } } });

  // ---- charts (SVG strings) ------------------------------------------------
  function svgBar(points) {
    if (!points.length) return '<p class="muted">No data yet.</p>';
    var W = 560, H = 200, padL = 8, padR = 8, padB = 22, padT = 16, n = points.length;
    var max = Math.max(1, Math.max.apply(null, points.map(function (p) { return p.count; })));
    var plotH = H - padT - padB, bw = (W - padL - padR) / n, s = '';
    points.forEach(function (p, i) {
      var bh = plotH * (p.count / max), x = padL + i * bw + bw * 0.15, wid = bw * 0.7, y = padT + plotH - bh;
      s += '<rect class="bar" x="' + x.toFixed(1) + '" y="' + y.toFixed(1) + '" width="' + wid.toFixed(1) + '" height="' + bh.toFixed(1) + '" rx="3" style="fill:var(--accent)" data-tip="' + esc(p.label + ': ' + p.count) + '"/>';
      if (p.count > 0) s += '<text x="' + (x + wid / 2).toFixed(1) + '" y="' + (y - 4).toFixed(1) + '" text-anchor="middle" class="cval">' + p.count + '</text>';
      s += '<text x="' + (x + wid / 2).toFixed(1) + '" y="' + (H - padB + 14) + '" text-anchor="middle" class="cax">' + esc(p.label.slice(5)) + '</text>';
    });
    return '<svg viewBox="0 0 ' + W + ' ' + H + '" class="chart" preserveAspectRatio="xMidYMid meet">' + s + '</svg>';
  }
  function svgDonut(items) {
    var total = items.reduce(function (a, b) { return a + b.value; }, 0);
    if (total <= 0) return '<p class="muted">No data yet.</p>';
    var cx = 90, cy = 90, r = 82, ang = -Math.PI / 2, s = '';
    items.forEach(function (it) {
      if (it.value <= 0) return;
      var frac = it.value / total, tip = esc(it.label + ': ' + fmtNum(it.value) + ' (' + Math.round(frac * 100) + '%)');
      if (frac >= 0.9999) { s += '<circle class="slice" cx="' + cx + '" cy="' + cy + '" r="' + r + '" style="fill:' + it.color + '" data-tip="' + tip + '"/>'; return; }
      var a2 = ang + frac * 2 * Math.PI;
      var x1 = cx + r * Math.cos(ang), y1 = cy + r * Math.sin(ang), x2 = cx + r * Math.cos(a2), y2 = cy + r * Math.sin(a2), large = frac > 0.5 ? 1 : 0;
      s += '<path class="slice" d="M' + cx + ' ' + cy + ' L' + x1.toFixed(2) + ' ' + y1.toFixed(2) + ' A' + r + ' ' + r + ' 0 ' + large + ' 1 ' + x2.toFixed(2) + ' ' + y2.toFixed(2) + ' Z" style="fill:' + it.color + '" data-tip="' + tip + '"/>';
      ang = a2;
    });
    s += '<circle cx="' + cx + '" cy="' + cy + '" r="47" style="fill:var(--panel)"/>';
    s += '<text x="' + cx + '" y="' + (cy - 1) + '" text-anchor="middle" class="donut-center">' + fmtNum(total) + '</text>';
    s += '<text x="' + cx + '" y="' + (cy + 14) + '" text-anchor="middle" class="donut-sub">total</text>';
    return '<svg viewBox="0 0 180 180" class="chart pie">' + s + '</svg>';
  }
  function svgTimebars(points, xkind) {
    if (!points.length) return '<p class="muted">No data.</p>';
    var W = 600, H = 190, padL = 8, padR = 8, padB = 22, padT = 12, n = points.length;
    var max = Math.max(1, Math.max.apply(null, points.map(function (p) { return p.count; })));
    var plotH = H - padT - padB, bw = (W - padL - padR) / n, bars = '', labels = '', pts = [], every = n > 16 ? Math.ceil(n / 12) : 1;
    points.forEach(function (p, i) {
      var bh = plotH * (p.count / max), x = padL + i * bw, cx = x + bw / 2, y = padT + plotH - bh;
      bars += '<rect class="tb" x="' + (x + bw * 0.12).toFixed(1) + '" y="' + y.toFixed(1) + '" width="' + (bw * 0.76).toFixed(1) + '" height="' + bh.toFixed(1) + '" rx="2" style="fill:var(--accent);opacity:.4" data-tip="' + esc(p.label + ': ' + p.count) + '"/>';
      pts.push(cx.toFixed(1) + ',' + y.toFixed(1));
      if (i % every === 0) { var xl = xkind === 'hour' ? p.label + 'h' : p.label.slice(5); labels += '<text x="' + cx.toFixed(1) + '" y="' + (H - padB + 14) + '" text-anchor="middle" class="cax">' + esc(xl) + '</text>'; }
    });
    var line = '<polyline points="' + pts.join(' ') + '" fill="none" style="stroke:var(--accent)" stroke-width="2"/>';
    var dots = pts.map(function (p) { var a = p.split(','); return '<circle cx="' + a[0] + '" cy="' + a[1] + '" r="2.4" style="fill:var(--accent)"/>'; }).join('');
    return '<svg viewBox="0 0 ' + W + ' ' + H + '" class="chart" preserveAspectRatio="xMidYMid meet">' + bars + line + dots + labels + '</svg>';
  }

  // ---- perf canvas graphs --------------------------------------------------
  function pushHist(a, v) { a.push(v); if (a.length > HIST) a.shift(); }
  function colorFor(k, v) { if (k === 'cpu') return v >= 90 ? '#f87171' : (v >= 70 ? '#fbbf24' : '#4a9eff'); if (k === 'mem') return '#34d399'; if (k === 'gpu') return '#a78bfa'; return '#60a5fa'; }
  function drawGraph(id, data, kind) {
    var c = document.getElementById(id); if (!c) return;
    var dpr = window.devicePixelRatio || 1, W = c.clientWidth || 280, H = c.clientHeight || 60;
    c.width = W * dpr; c.height = H * dpr; var ctx = c.getContext('2d'); ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, W, H);
    ctx.strokeStyle = 'rgba(128,128,128,0.18)'; ctx.lineWidth = 1; ctx.beginPath();
    for (var g = 0; g <= 4; g++) { var gy = Math.round(H * g / 4) + 0.5; ctx.moveTo(0, gy); ctx.lineTo(W, gy); } ctx.stroke();
    var n = data.length; if (!n) return; var slot = W / HIST;
    for (var i = 0; i < n; i++) { var v = data[i]; if (v == null) continue; v = Math.max(0, Math.min(100, v)); var bh = H * v / 100, x = W - (n - i) * slot; ctx.fillStyle = colorFor(kind, v); ctx.fillRect(x, H - bh, Math.max(1, slot - 1), bh); }
  }
  function redrawPerf() { drawGraph('cpugraph', hist.cpu, 'cpu'); drawGraph('memgraph', hist.mem, 'mem'); var g = document.getElementById('gpu-metric'); if (g && !g.hidden) drawGraph('gpugraph', hist.gpu, 'gpu'); drawGraph('srvgraph', hist.srv, 'srv'); }
  function pollStats() {
    api('stats', { quiet: true }).then(function (s) {
      if (!document.getElementById('cpugraph')) return;
      if (s.cpu != null) { pushHist(hist.cpu, s.cpu); setText('cpuval', s.cpu + '%'); }
      if (s.memTotal) { var used = s.memTotal - s.memFree, pct = Math.round(used / s.memTotal * 100); pushHist(hist.mem, pct); setText('memval', fmtBytes(used) + ' / ' + fmtBytes(s.memTotal) + ' (' + pct + '%)'); }
      var gm = document.getElementById('gpu-metric');
      if (s.gpu) { if (gm) gm.hidden = false; setText('gpuname', s.gpu.name || ''); pushHist(hist.gpu, s.gpu.util); setText('gpuval', s.gpu.util + '% · ' + fmtBytes(s.gpu.memUsed) + ' / ' + fmtBytes(s.gpu.memTotal)); }
      else if (gm) { gm.hidden = true; }
      var srv = (s.serverMem && s.memTotal) ? s.serverMem / s.memTotal * 100 : 0; pushHist(hist.srv, srv); setText('srvval', s.serverMem ? fmtBytes(s.serverMem) : 'not running');
      redrawPerf(); setText('histspan', hist.cpu.length * 4); setText('perf-updated', 'updated ' + new Date().toLocaleTimeString());
    }).catch(function () {});
  }
  // Inner HTML of the status card, including the single Start/Stop toggle.
  function statusTileInner(health, ctl) {
    var st = (health && health.status) || 'offline', stLabel = { online: 'Online', loading: 'Loading…', offline: 'Offline' }[st] || 'Unknown';
    var running = !!(ctl && ctl.running);
    var meta = (st === 'online' && health.model) ? '<div class="muted">model: ' + esc(health.model) + '</div>'
      : (running && st !== 'online' ? '<div class="muted">process up &middot; PID ' + esc(ctl.pid) + '</div>'
        : (st === 'offline' ? '<div class="muted">no response on /health</div>' : ''));
    var btn = running
      ? '<button class="btn danger small" data-act="srv-toggle" data-running="1">&#9632;&nbsp; Stop</button>'
      : '<button class="btn small" data-act="srv-toggle" data-running="0">&#9654;&nbsp; Start</button>';
    return '<div class="stattop"><div class="statmain">' +
      '<div class="num"><span class="dot"></span>' + stLabel + '</div>' +
      '<div class="lbl">Server &middot; <code>' + esc(health.url) + '</code></div>' + meta +
      '<span id="srvmsg" class="muted srvmsg"></span>' +
      '</div><div class="statbtns">' + btn +
      '<button type="button" class="btn ghost small" data-act="srv-logs" title="Live console output">&#128220;&nbsp; Logs</button>' +
      '</div></div>';
  }
  function paintServerStatus(health, ctl) {
    var tile = document.getElementById('statustile'); if (!tile) return;
    var prev = document.getElementById('srvmsg'); var keep = prev ? prev.textContent : '';   // preserve any error message across polls
    tile.className = 'stat status-' + ((health && health.status) || 'offline');
    tile.innerHTML = statusTileInner(health, ctl);
    var now = document.getElementById('srvmsg'); if (now && keep) now.textContent = keep;
  }
  function pollStatus() {
    if (!document.getElementById('statustile')) return;
    api('server_status', { quiet: true }).then(function (r) { paintServerStatus(r.health, r.ctl); }).catch(function () {});
  }
  function startPerf() { hist = { cpu: [], mem: [], gpu: [], srv: [] }; pollStats(); pollStatus(); perfTimer = setInterval(function () { pollStats(); pollStatus(); }, 4000); }
  function stopTimers() { if (perfTimer) { clearInterval(perfTimer); perfTimer = null; } if (logsTimer) { clearInterval(logsTimer); logsTimer = null; } if (dashTimer) { clearInterval(dashTimer); dashTimer = null; } if (typeof window.closeServerLog === 'function') window.closeServerLog(); }
  window.addEventListener('resize', function () { if (document.getElementById('cpugraph')) redrawPerf(); });

  // ---- auth views ----------------------------------------------------------
  function showSetup() {
    document.getElementById('nav').hidden = true;
    setMain('<div class="authwrap"><div class="card narrow"><h1>First-run setup</h1><p class="muted">No admin password is set. Choose one to protect this panel.</p><div id="authmsg"></div>' +
      '<form id="setupform"><label>New password <input type="password" name="password" required></label><label>Confirm <input type="password" name="password2" required></label><button type="submit">Set password</button></form></div></div>');
  }
  function showLogin() {
    document.getElementById('nav').hidden = true;
    setMain('<div class="authwrap"><div class="card narrow"><h1>Log in</h1><div id="authmsg"></div>' +
      '<form id="loginform"><label>Password <input type="password" name="password" autofocus required></label><button type="submit">Log in</button></form></div></div>');
  }
  function authErr(m) { var e = document.getElementById('authmsg'); if (e) e.innerHTML = '<div class="flash err">' + esc(m) + '</div>'; }

  // ---- dashboard -----------------------------------------------------------
  function tile(label, valId, canvasId) { return '<div class="perftile"><div class="mhead"><span class="mlabel">' + label + '</span><span class="mval" id="' + valId + '">—</span></div><canvas class="graph" id="' + canvasId + '"></canvas></div>'; }
  var actDays = 14, actFrom = '', actTo = '';
  function actRangeButtons() {
    var btns = [7, 14, 30].map(function (d) { return '<button class="btn small ' + (actDays === d ? '' : 'ghost') + '" data-act="act-range" data-days="' + d + '">' + d + ' days</button>'; }).join('');
    return btns +
      '<input type="date" id="actfrom" class="actdate" value="' + esc(actFrom) + '" title="From">' +
      '<span class="muted">to</span>' +
      '<input type="date" id="actto" class="actdate" value="' + esc(actTo) + '" title="To">' +
      '<button class="btn small ' + (actDays === 0 ? '' : 'ghost') + '" data-act="act-apply">Apply</button>';
  }
  function reloadActivity(quiet) {
    var q = (actDays === 0 && actFrom && actTo) ? { from: actFrom, to: actTo } : { days: actDays };
    api('activity', { query: q, quiet: quiet }).then(function (d) {
      var c = document.getElementById('actchart'); if (c) c.innerHTML = svgBar(d.perday);
      var r = document.getElementById('actrange'); if (r) r.innerHTML = actRangeButtons();
    }).catch(function () {});
  }
  function accountsSubLine(ac) {
    ac = ac || {};
    return (ac.active || 0) + ' active' + (ac.admins ? ' &middot; ' + ac.admins + ' admin' : '') +
      (ac.pending ? ' &middot; <span class="warn-tag">' + ac.pending + ' pending</span>' : '');
  }
  // Periodically refresh the dashboard counts (accounts, hosts, requests, log files)
  // and the activity chart in place — without rebuilding the page or disturbing the
  // live performance graphs, the server status tile, or any open modal.
  function refreshDash() {
    api('overview', { quiet: true }).then(function (o) {
      if (!document.getElementById('dash-accounts')) return;   // navigated away
      var ac = o.accounts || {};
      setText('dash-accounts', ac.total);
      var sub = document.getElementById('dash-accounts-sub'); if (sub) sub.innerHTML = accountsSubLine(ac);
      setText('dash-hosts', o.allowedHosts);
      setText('dash-requests', o.requestsToday);
      setText('dash-logfiles', o.logFiles);
    }).catch(function () {});
    reloadActivity(true);                                      // requests-per-day chart, at the current range (quiet)
  }
  function renderDashboard() {
    stopTimers();
    api('overview').then(function (o) {
      var st = o.health.status, stLabel = { online: 'Online', loading: 'Loading…', offline: 'Offline' }[st] || 'Unknown';
      var ac = o.accounts || { total: 0, active: 0, pending: 0, admins: 0 };
      var sc = o.serverCtl || {};
      var launchModal = '<div id="launchmodal" class="modal" onclick="if(event.target===this)closeLaunchModal()">' +
        '<div class="modal-panel" style="max-width:560px">' +
        '<div class="modal-head"><strong>Launch configuration</strong><button type="button" class="btn ghost small" onclick="closeLaunchModal()">Close &#10005;</button></div>' +
        '<div style="padding:18px">' +
        '<label>Executable path <input id="srv-exe" value="' + esc(sc.exe || '') + '" placeholder="C:\\llama\\llama-server.exe"></label>' +
        '<label>Command-line arguments <textarea id="srv-args" rows="4" spellcheck="false">' + esc(sc.args || '') + '</textarea></label>' +
        '<div class="row"><button type="button" class="btn" data-act="srv-save">Save</button> <button type="button" class="btn ghost" onclick="closeLaunchModal()">Cancel</button><span class="muted" id="srvsavemsg"></span><span class="muted" style="margin-left:auto">port ' + esc(sc.port) + '</span></div>' +
        '<p class="hint">&#9888; <strong>Start</strong> runs this under the web-server (Apache) account — for GPU access, launch Apache from the XAMPP Control Panel in your own desktop session (not as a Windows service). <strong>Stop</strong> only targets the process on port ' + esc(sc.port) + ', and only if it is llama-server.</p>' +
        '</div></div></div>';
      var logModal = '<div id="logmodal" class="modal" onclick="if(event.target===this)closeServerLog()">' +
        '<div class="modal-panel" style="max-width:880px">' +
        '<div class="modal-head"><strong>llama-server console <span class="muted" style="font-weight:400;font-size:12px">&middot; live</span></strong>' +
        '<button type="button" class="btn ghost small" onclick="closeServerLog()">Close &#10005;</button></div>' +
        '<pre id="srvlogpre" class="rawbox srvlog">(loading&hellip;)</pre>' +
        '<div style="padding:0 18px 14px"><span class="muted" style="font-size:11px">Console output is captured only for servers started from this dashboard.</span></div>' +
        '</div></div>';
      setMain(
        '<h1>Dashboard</h1><div class="cards">' +
        '<div class="stat status-' + st + '" id="statustile">' + statusTileInner(o.health, sc) + '</div>' +
        '<div class="stat"><div class="num" id="dash-accounts">' + ac.total + '</div><div class="lbl">Accounts</div>' +
        '<div class="muted" id="dash-accounts-sub" style="font-size:12px">' + accountsSubLine(ac) + '</div>' +
        '<a href="#users">Manage &rarr;</a></div>' +
        '<div class="stat"><div class="num" id="dash-hosts">' + o.allowedHosts + '</div><div class="lbl">Allowed hosts</div><a href="#users">Manage &rarr;</a></div>' +
        '<div class="stat"><div class="num" id="dash-requests">' + o.requestsToday + '</div><div class="lbl">Requests today</div><a href="#logs">View logs &rarr;</a></div>' +
        '<div class="stat"><div class="num" id="dash-logfiles">' + o.logFiles + '</div><div class="lbl">Log files</div></div></div>' +
        '<div class="card"><div class="row" style="justify-content:space-between;align-items:baseline;margin:0"><h2 style="margin:0">Machine performance <span class="muted" style="font-size:12px;font-weight:400">live</span></h2><span class="muted" id="perf-updated"></span></div>' +
        '<div class="perftiles">' + tile('CPU', 'cpuval', 'cpugraph') + tile('Memory', 'memval', 'memgraph') +
        '<div class="perftile" id="gpu-metric" hidden><div class="mhead"><span class="mlabel">GPU <span class="muted" id="gpuname"></span></span><span class="mval" id="gpuval">—</span></div><canvas class="graph" id="gpugraph"></canvas></div>' +
        tile('llama-server RAM', 'srvval', 'srvgraph') + '</div><div class="muted" style="font-size:11px;margin-top:8px">~<span id="histspan">0</span>s history · sampled every 4s</div></div>' +
        '<div class="card"><div class="row" style="justify-content:space-between;align-items:center;margin:0"><h2 style="margin:0">Activity <span class="muted" style="font-weight:400;font-size:12px">requests per day</span></h2>' +
        '<div class="row" id="actrange">' + actRangeButtons() + '</div></div>' +
        '<div id="actchart">' + svgBar(o.perday) + '</div></div>' +
        '<div class="card"><div class="row" style="justify-content:space-between;align-items:center;margin:0"><h2 style="margin:0">Configuration</h2><button type="button" class="btn ghost small" data-act="launch-edit">&#9998; Edit launch command</button></div>' +
        '<table class="kv"><tr><th>Executable</th><td>' + (sc.exe ? '<code>' + esc(sc.exe) + '</code>' : '<span class="muted">not set</span>') + '</td></tr>' +
        '<tr><th>Arguments</th><td>' + (sc.args ? '<code>' + esc(sc.args) + '</code>' : '<span class="muted">—</span>') + '</td></tr>' +
        '<tr><th>Access-allow file</th><td><code>' + esc(o.config.access_allow_file) + '</code> ' + (o.config.allow_found ? '<span class="ok-tag">found</span>' : '<span class="warn-tag">missing</span>') + '</td></tr>' +
        '<tr><th>Log directory</th><td><code>' + esc(o.config.log_dir) + '</code> ' + (o.config.log_found ? '<span class="ok-tag">found</span>' : '<span class="warn-tag">missing</span>') + '</td></tr></table>' +
        '<p class="muted">Allow-list changes are hot-reloaded by llama-server. Loopback (127.0.0.1 / ::1) always bypasses the list.</p></div>' +
        launchModal + logModal
      );
      startPerf();
      dashTimer = setInterval(refreshDash, 15000);   // live counts + activity every 15s
    }).catch(showError);
  }
  function srvMsg(m) { var e = document.getElementById('srvmsg'); if (e) e.textContent = m; }
  function srvAction(action) {
    srvMsg('Working…');
    api(action, { body: {} }).then(function (r) {
      if (r.error) { srvMsg('✕ ' + r.error); }               // error persists (preserved across polls)
      else { srvMsg(''); setTimeout(pollStatus, 600); }       // success: let the toggle flip via a fresh status
    }).catch(function (er) { srvMsg('✕ ' + er); });
  }
  function srvSave() {
    var exe = document.getElementById('srv-exe').value, args = document.getElementById('srv-args').value;
    var m = document.getElementById('srvsavemsg'); if (m) m.textContent = 'Saving…';
    api('server_config_save', { body: { exe: exe, args: args } }).then(function () { window.closeLaunchModal(); renderDashboard(); }).catch(function (er) { if (m) m.textContent = '✕ ' + er; });
  }
  window.openLaunchModal = function () { var m = document.getElementById('launchmodal'); if (!m) return; var s = document.getElementById('srvsavemsg'); if (s) s.textContent = ''; m.classList.add('open'); var e = document.getElementById('srv-exe'); if (e) e.focus(); };
  window.closeLaunchModal = function () { var m = document.getElementById('launchmodal'); if (m) m.classList.remove('open'); };
  var srvLogTimer = null, srvLogOffset = 0;
  function pollServerLog(first) {
    api('server_log', { query: { from: srvLogOffset }, quiet: true }).then(function (r) {
      var pre = document.getElementById('srvlogpre'); if (!pre) return;
      if (!r.exists) { pre.textContent = '(no console log yet — start the server from this dashboard to capture its output)'; srvLogOffset = 0; return; }
      if (first) pre.textContent = '';
      if (r.text) {
        var atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 24;
        pre.textContent += r.text;
        if (atBottom || first) pre.scrollTop = pre.scrollHeight;
      } else if (first) { pre.textContent = '(console log is empty)'; }
      srvLogOffset = r.size;
    }).catch(function () {});
  }
  window.openServerLog = function () {
    var m = document.getElementById('logmodal'); if (!m) return;
    srvLogOffset = 0; var pre = document.getElementById('srvlogpre'); if (pre) pre.textContent = '(loading…)';
    m.classList.add('open');
    pollServerLog(true);
    if (srvLogTimer) clearInterval(srvLogTimer);
    srvLogTimer = setInterval(function () { pollServerLog(false); }, 1500);
  };
  window.closeServerLog = function () {
    var m = document.getElementById('logmodal'); if (m) m.classList.remove('open');
    if (srvLogTimer) { clearInterval(srvLogTimer); srvLogTimer = null; }
  };

  // ---- users ---------------------------------------------------------------
  var usersCache = [];
  var accountsCache = [];
  // ---- Users & access: chat accounts + API allow-list hosts (merged) -------
  function renderUsers() {
    stopTimers();
    Promise.all([api('accounts'), api('users')]).then(function (res) {
      accountsCache = (res[0] && res[0].accounts) || [];
      usersCache = (res[1] && res[1].users) || [];
      drawUsersPage();
    }).catch(showError);
  }
  function accStatusBadge(s) {
    var cls = s === 'active' ? 'ok' : (s === 'pending' ? 'warn' : 'muted');
    return '<span class="badge ' + cls + '">' + esc(s || 'unknown') + '</span>';
  }
  function accountsSection(flash) {
    var list = accountsCache.slice().sort(function (a, b) {
      var order = { pending: 0, active: 1 };
      var da = order[a.status] == null ? 2 : order[a.status], db = order[b.status] == null ? 2 : order[b.status];
      return da - db || String(b.created).localeCompare(String(a.created));
    });
    var pend = list.filter(function (a) { return a.status === 'pending'; }).length;
    var rows = list.length ? list.map(function (a) {
      var id = esc(a.id);
      var acts;
      if (a.status === 'pending') {
        acts = '<button class="btn small" data-act="acct-approve" data-id="' + id + '">Approve</button> <button class="btn small" data-act="acct-approve-allow" data-id="' + id + '" title="Approve and add this IP + MAC to the access-allow list">Approve + allow host</button> <button class="btn small danger" data-act="acct-reject" data-id="' + id + '" data-name="' + esc(a.name) + '">Reject</button>';
      } else if (a.status === 'active') {
        var roleBtn = a.role === 'admin'
          ? '<button class="btn small ghost" data-act="acct-revoke-admin" data-id="' + id + '" data-name="' + esc(a.name) + '" title="Remove administrator access">Revoke admin</button> '
          : '<button class="btn small" data-act="acct-make-admin" data-id="' + id + '" data-name="' + esc(a.name) + '" title="Grant admin-panel access (no separate password login)">Make admin</button> ';
        acts = roleBtn + '<button class="btn small ghost" data-act="acct-allow" data-id="' + id + '" title="Add this IP + MAC to the access-allow list">Allow host</button> <button class="btn small ghost" data-act="acct-reset-pw" data-id="' + id + '" data-name="' + esc(a.name) + '" title="Set a new password for this account">Reset password</button> <button class="btn small ghost" data-act="acct-suspend" data-id="' + id + '">Suspend</button> <button class="btn small danger" data-act="acct-delete" data-id="' + id + '" data-name="' + esc(a.name) + '">Delete</button>';
      } else {
        acts = '<button class="btn small danger" data-act="acct-delete" data-id="' + id + '" data-name="' + esc(a.name) + '">Delete</button>';
      }
      var macs = (a.macs && a.macs.length) ? a.macs.map(function (m) { return '<code class="mac">' + esc(m) + '</code>'; }).join(' ') : '<span class="muted">—</span>';
      var roleBadge = a.role === 'admin' ? '<span class="badge ok">admin</span>' : '<span class="muted">user</span>';
      return '<tr><td>' + esc(a.name) + '</td><td><code>' + id + '</code></td><td>' + accStatusBadge(a.status) +
        '</td><td>' + roleBadge + '</td><td><code>' + esc(a.ip || '—') + '</code></td><td>' + macs + '</td><td class="muted">' + esc(a.created) + '</td>' +
        '<td class="actions">' + acts + '</td></tr>';
    }).join('') : '<tr><td colspan="8" class="muted">No accounts yet. Users sign up on the chat page; new signups appear here as “pending”.</td></tr>';
    var flashHtml = (flash && flash.length) ? flash.map(function (m) { return '<div class="flash warn">' + esc(m) + '</div>'; }).join('') : '';
    return '<div class="row" style="justify-content:space-between;align-items:center"><h2 style="margin:0">Chat accounts</h2>' +
      (pend ? '<span class="badge warn">' + pend + ' pending</span>' : '') + '</div>' +
      '<p class="muted">People sign up on the chat page (their IP + MAC are captured). Approve one to let them sign in. Make an account an <strong>admin</strong> to give it this admin panel (they reach it from the chat page’s Admin link — no separate password).</p>' +
      flashHtml +
      '<div class="card"><table class="grid"><thead><tr><th>Name</th><th>ID</th><th>Status</th><th>Role</th><th>IP</th><th>MAC(s)</th><th>Signed up</th><th></th></tr></thead><tbody>' + rows + '</tbody></table></div>';
  }
  function hostsSection() {
    var list = usersCache;
    var rows = list.length ? list.map(function (u) {
      return '<tr><td>' + esc(u.alias) + '</td><td><code>' + esc(u.ip) + '</code></td><td>' + u.macs.map(function (m) { return '<code class="mac">' + esc(m) + '</code>'; }).join(' ') +
        '</td><td class="actions"><button class="btn small" data-act="user-edit" data-ip="' + esc(u.ip) + '">Edit</button> <button class="btn small danger" data-act="user-delete" data-ip="' + esc(u.ip) + '" data-alias="' + esc(u.alias) + '">Delete</button></td></tr>';
    }).join('') : '<tr><td colspan="4" class="muted">No hosts yet.</td></tr>';
    return '<div class="row" style="justify-content:space-between;align-items:center;margin-top:26px"><h2 style="margin:0">Allowed hosts <span class="muted" style="font-weight:400">(direct API access)</span></h2>' +
      '<button class="btn" data-act="user-add">+ Add host</button></div>' +
      '<p class="muted">The list llama-server enforces: a host is allowed only when its IP is listed <em>and</em> the resolved MAC matches. Chat users go through the app, so they don’t need to be here unless they also use the API directly.</p>' +
      '<div class="card"><table class="grid"><thead><tr><th>Alias</th><th>IP</th><th>MAC(s)</th><th></th></tr></thead><tbody>' + rows + '</tbody></table></div>' +
      '<div id="hostmodal" class="modal" onclick="if(event.target===this)closeHostModal()">' +
      '<div class="modal-panel" style="max-width:460px">' +
      '<div class="modal-head"><strong id="hosttitle">Add host</strong><button type="button" class="btn ghost small" onclick="closeHostModal()">Close &#10005;</button></div>' +
      '<div style="padding:18px"><form id="userform">' +
      '<input type="hidden" name="act" value="add"><input type="hidden" name="original_ip" value="">' +
      '<label>Alias <input name="alias" placeholder="office-desktop" required></label>' +
      '<label>IP address <input name="ip" placeholder="192.168.1.10" required></label>' +
      '<label>MAC address(es) <input name="macs" placeholder="00:11:22:33:44:55" required></label>' +
      '<div class="row"><button type="submit" id="hostsave">Add host</button> <button type="button" class="btn ghost" onclick="closeHostModal()">Cancel</button></div>' +
      '<p class="hint">Separate multiple MACs with commas or spaces. Format xx:xx:xx:xx:xx:xx.</p>' +
      '</form><div id="usermsg"></div></div></div></div>';
  }
  function drawUsersPage(flash) {
    setMain(
      '<h1 style="margin:0 0 4px">Users &amp; access</h1>' +
      '<p class="muted" style="margin:0 0 20px">Chat login accounts and the API access-allow list, in one place.</p>' +
      accountsSection(flash) + hostsSection()
    );
  }
  function openResetPassword(id, name) {
    var ov = document.createElement('div'); ov.className = 'uidlg-overlay';
    ov.innerHTML = '<div class="uidlg"><div class="uidlg-title">Reset password for ' + esc(name || id) + '</div>' +
      '<form id="rpform" autocomplete="off">' +
      '<label>New password <input type="password" name="p1" autocomplete="new-password" required></label>' +
      '<label>Confirm password <input type="password" name="p2" autocomplete="new-password" required></label>' +
      '<div id="rpmsg"></div>' +
      '<div class="uidlg-btns"><button type="button" class="btn ghost" data-rp="cancel">Cancel</button><button type="submit" class="btn">Set password</button></div>' +
      '</form></div>';
    document.body.appendChild(ov);
    function close() { if (ov.parentNode) ov.parentNode.removeChild(ov); }
    ov.addEventListener('click', function (e) { if (e.target === ov || (e.target.getAttribute && e.target.getAttribute('data-rp') === 'cancel')) close(); });
    ov.querySelector('#rpform').addEventListener('submit', function (e) {
      e.preventDefault();
      var f = this, p1 = f.p1.value, p2 = f.p2.value, msg = ov.querySelector('#rpmsg');
      if (p1.length < 8) { msg.innerHTML = '<div class="flash err">Password must be at least 8 characters.</div>'; return; }
      if (p1 !== p2) { msg.innerHTML = '<div class="flash err">Passwords do not match.</div>'; return; }
      msg.innerHTML = '<div class="muted">Saving&hellip;</div>';
      api('account_action', { body: { act: 'set_password', id: id, password: p1 } })
        .then(function (r) { accountsCache = r.accounts || accountsCache; msg.innerHTML = '<div class="flash ok">Password reset for ' + esc(name || id) + '.</div>'; setTimeout(close, 900); })
        .catch(function (er) { msg.innerHTML = '<div class="flash err">' + esc(er) + '</div>'; });
    });
    var inp = ov.querySelector('input'); if (inp) inp.focus();
  }
  function acctAction(act, id) {
    api('account_action', { body: { act: act, id: id } }).then(function (r) {
      accountsCache = r.accounts || accountsCache;
      if (act === 'approve_allow' || act === 'allow_host' || act === 'suspend') {   // hosts changed too — refresh them
        api('users').then(function (u) { usersCache = u.users || usersCache; drawUsersPage(r.warn); }).catch(function () { drawUsersPage(r.warn); });
      } else { drawUsersPage(r.warn); }
    }).catch(showError);
  }
  function openHostModal(ip) {
    var f = document.getElementById('userform');
    var edit = ip ? usersCache.filter(function (u) { return u.ip === ip; })[0] : null;
    document.getElementById('hosttitle').textContent = edit ? 'Edit host' : 'Add host';
    document.getElementById('hostsave').textContent = edit ? 'Save changes' : 'Add host';
    f.act.value = edit ? 'update' : 'add';
    f.original_ip.value = edit ? edit.ip : '';
    f.alias.value = edit ? edit.alias : '';
    f.ip.value = edit ? edit.ip : '';
    f.macs.value = edit ? edit.macs.join(', ') : '';
    document.getElementById('usermsg').innerHTML = '';
    document.getElementById('hostmodal').classList.add('open');
    f.alias.focus();
  }
  window.closeHostModal = function () { var m = document.getElementById('hostmodal'); if (m) m.classList.remove('open'); };

  // Chat moved to its own standalone page (chat.html).

  // ---- logs ----------------------------------------------------------------
  var logBackups = [];
  function backupsHtml() {
    var items = logBackups.length ? logBackups.map(function (b) {
      return '<tr><td><code>' + esc(b.name) + '</code></td><td class="muted nowrap">' + esc(b.when) + '</td><td class="nowrap">' + fmtBytes(b.size) + '</td>' +
        '<td class="nowrap"><a class="btn small ghost" href="#" data-act="logs-browse" data-zip="' + esc(b.name) + '">Browse</a> ' +
        '<a class="btn small ghost" href="api.php?action=logs_backup_download&name=' + encodeURIComponent(b.name) + '">Download</a></td></tr>';
    }).join('') : '<tr><td colspan="4" class="muted">No backups yet. Logs are auto-archived monthly; use “Backup &amp; clear” to zip now.</td></tr>';
    return '<div class="row" style="justify-content:space-between;align-items:center;margin:0"><h2 style="margin:0">Log backups</h2>' +
      '<span class="muted" style="font-size:12px">Auto-archived monthly &middot; kept in the log directory</span></div>' +
      '<div class="card" style="padding:0;margin:12px 0 0"><table class="grid"><thead><tr><th>File</th><th>Created</th><th>Size</th><th></th></tr></thead><tbody>' + items + '</tbody></table></div>';
  }
  function doLogBackup() {
    setText('lnote', 'Backing up…');
    api('logs_backup', { body: {} }).then(function (r) {
      reloadLogs();   // refetch (old logs gone) + refresh the backups panel
      uiAlert(r.count > 0 ? ('Archived ' + r.count + ' log file(s) → ' + r.zip + ' (' + fmtBytes(r.size) + ').') : (r.note || 'Nothing to back up.'), { title: 'Backup complete' });
    }).catch(function (e) { setText('lnote', ''); uiAlert('Backup failed: ' + e, { title: 'Backup failed' }); });
  }
  function renderLogs() {
    stopTimers();
    var pend = pendingLog; pendingLog = null;
    logFrom = pend ? (pend.from || '') : isoToday();   // default to today's requests
    logTo = pend ? (pend.to || '') : isoToday();
    logRows = []; logSeen = {}; logPage = 1;
    var q = {}; if (logFrom) q.from = logFrom; if (logTo) q.to = logTo;
    api('logs', { query: q }).then(function (d) {
      logRows = d.rows; logMin = d.minDate; logMax = d.maxDate; logBackups = d.backups || [];
      d.rows.forEach(function (r) { logSeen[r.sig] = 1; });
      drawLogsShell(d.cap);
      if (pend && pend.filter) {
        document.getElementById('lfilter').value = pend.filter;
        document.getElementById('lpromptonly').checked = true; // show only the user's prompt requests
      }
      applyLogs();
      if (document.getElementById('llive').checked) logsTimer = setInterval(pollLogs, 4000);
    }).catch(showError);
  }
  function drawLogsShell(cap) {
    setMain(
      '<div class="card"><div class="row">' +
      '<h1 style="margin:0">Request logs</h1>' +
      '<label>From <input type="date" id="lfrom" value="' + esc(logFrom) + '" min="' + esc(logMin) + '" max="' + esc(logMax) + '"></label>' +
      '<label>To <input type="date" id="lto" value="' + esc(logTo) + '" min="' + esc(logMin) + '" max="' + esc(logMax) + '"></label>' +
      '<button class="btn" data-act="logs-apply">Apply</button><button class="btn ghost" data-act="logs-reset">Reset</button>' +
      '<button class="btn ghost" data-act="logs-backup" title="Zip every log before today and delete the originals to reclaim space">&#128190; Backup &amp; clear</button>' +
      '<input type="search" id="lfilter" placeholder="filter (user, prompt, status...)">' +
      '<label class="chk"><input type="checkbox" id="lpromptonly" checked> prompt only</label>' +
      '<label class="chk live-on" id="llivewrap"><input type="checkbox" id="llive" checked> <span class="livedot"></span> live</label>' +
      '<span class="muted" id="lnote"></span></div></div>' +
      '<div class="card"><table class="grid logs"><thead><tr><th>Time</th><th>User</th><th>Prompt</th><th>Status</th><th>Latency</th><th>Detail</th></tr></thead><tbody id="logbody"></tbody></table>' +
      '<div class="pager"><button class="btn ghost small" data-act="logs-prev">‹ Prev</button><span id="pageinfo" class="muted"></span><button class="btn ghost small" data-act="logs-next">Next ›</button>' +
      '<label class="pagesize">Rows <select id="lpsize"><option>25</option><option selected>50</option><option>100</option><option>250</option></select></label></div></div>' +
      '<div class="card" id="backupscard">' + backupsHtml() + '</div>'
    );
    document.getElementById('lfilter').addEventListener('input', function () { logPage = 1; renderLogBody(); });
    document.getElementById('lpromptonly').addEventListener('change', function () { logPage = 1; renderLogBody(); });
    document.getElementById('lpsize').addEventListener('change', function () { logSize = parseInt(this.value, 10) || 50; logPage = 1; renderLogBody(); });
    document.getElementById('llive').addEventListener('change', function () {
      document.getElementById('llivewrap').classList.toggle('live-on', this.checked);
      if (this.checked) { if (!logsTimer) logsTimer = setInterval(pollLogs, 4000); } else if (logsTimer) { clearInterval(logsTimer); logsTimer = null; }
    });
  }
  // ---- browse a backup zip: same logs table, fed from the archive ----------
  var browsingBackup = null;
  function openBackup(name) {
    stopTimers();
    browsingBackup = name;
    api('logs_backup_view', { query: { name: name } }).then(function (d) {
      logRows = d.rows || []; logSeen = {}; logPage = 1;
      logRows.forEach(function (r) { logSeen[r.sig] = 1; });
      drawBackupShell(name, d.cap);
      setText('lnote', logRows.length + (logRows.length >= d.cap ? ' (newest ' + d.cap + ', capped)' : '') + ' request(s) in this backup');
      renderLogBody();
    }).catch(showError);
  }
  function drawBackupShell(name, cap) {
    setMain(
      '<div class="card"><div class="row">' +
      '<button class="btn ghost" data-act="logs-live" title="Return to the live request logs">&#8249; Live logs</button>' +
      '<h1 style="margin:0">Backup <code>' + esc(name) + '</code></h1>' +
      '<a class="btn ghost small" href="api.php?action=logs_backup_download&name=' + encodeURIComponent(name) + '">&#11015; Download zip</a>' +
      '<input type="search" id="lfilter" placeholder="filter (user, prompt, status...)">' +
      '<label class="chk"><input type="checkbox" id="lpromptonly" checked> prompt only</label>' +
      '<span class="muted" id="lnote"></span></div></div>' +
      '<div class="card"><table class="grid logs"><thead><tr><th>Time</th><th>User</th><th>Prompt</th><th>Status</th><th>Latency</th><th>Detail</th></tr></thead><tbody id="logbody"></tbody></table>' +
      '<div class="pager"><button class="btn ghost small" data-act="logs-prev">‹ Prev</button><span id="pageinfo" class="muted"></span><button class="btn ghost small" data-act="logs-next">Next ›</button>' +
      '<label class="pagesize">Rows <select id="lpsize"><option>25</option><option selected>50</option><option>100</option><option>250</option></select></label></div></div>'
    );
    document.getElementById('lfilter').addEventListener('input', function () { logPage = 1; renderLogBody(); });
    document.getElementById('lpromptonly').addEventListener('change', function () { logPage = 1; renderLogBody(); });
    document.getElementById('lpsize').addEventListener('change', function () { logSize = parseInt(this.value, 10) || 50; logPage = 1; renderLogBody(); });
  }
  function logRowHtml(r) {
    var cls = r.status >= 500 ? 'st5' : (r.status >= 400 ? 'st4' : 'st2');
    var u = esc(r.user);
    var userCell = r.user === 'loopback'
      ? '<span class="tag-loop logu" data-act="log-user" data-user="loopback" title="Filter by this user">loopback</span>'
      : (r.user === 'unknown'
        ? '<span class="muted logu" data-act="log-user" data-user="unknown" title="Filter by this user">unknown</span>'
        : '<strong class="logu" data-act="log-user" data-user="' + u + '" title="Filter by this user">' + u + '</strong>');
    var prompt = r.prompt ? esc(r.prompt.length > 200 ? r.prompt.slice(0, 200) + ' …' : r.prompt) : '<span class="muted">—</span>';
    return '<tr class="logrow" data-sig="' + esc(r.sig) + '" data-ts="' + esc(r.ts) + '" data-hasprompt="' + (r.hasPrompt ? '1' : '0') + '">' +
      '<td class="nowrap">' + esc(r.ts) + '</td><td>' + userCell + '</td><td class="prompt">' + prompt + '</td>' +
      '<td><span class="badge ' + cls + '">' + r.status + '</span></td><td class="nowrap">' + (r.latency ? esc(r.latency) : '<span class="muted">—</span>') + '</td>' +
      '<td><button class="btn small ghost" data-act="detail" data-sig="' + esc(r.sig) + '">View</button></td></tr>';
  }
  function renderLogBody() {
    var body = document.getElementById('logbody'); if (!body) return;
    var q = (document.getElementById('lfilter').value || '').toLowerCase();
    var promptOnly = document.getElementById('lpromptonly').checked;
    var filtered = logRows.filter(function (r) {
      if (promptOnly && !r.hasPrompt) return false;
      if (q) { var hay = (r.ts + ' ' + r.user + ' ' + r.remote + ' ' + r.method + ' ' + r.path + ' ' + r.status + ' ' + (r.prompt || '')).toLowerCase(); if (hay.indexOf(q) === -1) return false; }
      return true;
    });
    var pages = Math.max(1, Math.ceil(filtered.length / logSize));
    if (logPage > pages) logPage = pages; if (logPage < 1) logPage = 1;
    var slice = filtered.slice((logPage - 1) * logSize, logPage * logSize);
    body.innerHTML = slice.length ? slice.map(logRowHtml).join('') : '<tr><td colspan="6" class="muted">No matching requests.</td></tr>';
    setText('pageinfo', filtered.length ? 'Page ' + logPage + ' of ' + pages + ' · ' + filtered.length + ' rows' : 'no rows');
    var prev = document.querySelector('[data-act="logs-prev"]'), next = document.querySelector('[data-act="logs-next"]');
    if (prev) prev.disabled = logPage <= 1; if (next) next.disabled = logPage >= pages;
  }
  function applyLogs() { setText('lnote', 'newest ' + logRows.length + ' · max cap'); renderLogBody(); }
  function pollLogs() {
    if (!document.getElementById('logbody')) return;
    var after = logRows.length ? logRows[0].ts : '';
    api('logs', { query: { from: logFrom, to: logTo, after: after }, quiet: true }).then(function (d) {
      if (!document.getElementById('logbody')) return;
      var added = 0;
      d.rows.slice().reverse().forEach(function (r) { if (logSeen[r.sig]) return; logSeen[r.sig] = 1; logRows.unshift(r); added++; });
      if (added) { applyLogs(); }
    }).catch(function () {});
  }

  // ---- usage ---------------------------------------------------------------
  var usagePeriod = 'day', usageFrom = '', usageTo = '';
  function renderUsage() {
    stopTimers();
    var q = {}; if (usageFrom) q.from = usageFrom; if (usageTo) q.to = usageTo;
    api('usage', { query: q }).then(function (d) {
      var users = d.users, totalReq = 0, totalPrompts = 0, maxreq = 1;
      users.forEach(function (u) { totalReq += u.requests; totalPrompts += u.prompts; if (u.requests > maxreq) maxreq = u.requests; });
      var palette = ['#4a9eff', '#34d399', '#a78bfa', '#fbbf24', '#f472b6'];
      var pie = users.slice(0, 5).map(function (u, i) { return { label: u.user, value: u.requests, color: palette[i % palette.length] }; });
      var others = users.slice(5).reduce(function (a, u) { return a + u.requests; }, 0);
      if (others > 0) pie.push({ label: 'others', value: others, color: '#92a0b0' });
      var pieTotal = pie.reduce(function (a, b) { return a + b.value; }, 0);
      var bars = users.slice(0, 15).map(function (u) {
        var w = maxreq ? Math.round(u.requests / maxreq * 100) : 0;
        var name = u.user === 'loopback' ? '<span class="tag-loop">loopback</span>' : esc(u.user);
        return '<div class="ubar clickable" data-act="usage-user" data-user="' + esc(u.user) + '" data-tip="' + esc(u.user + ': ' + fmtNum(u.requests) + ' requests, ' + fmtNum(u.prompts) + ' prompts — click for logs') + '"><div class="uname">' + name + '</div><div class="utrack"><span style="width:' + w + '%"></span></div><div class="ucount">' + fmtNum(u.requests) + '</div></div>';
      }).join('');
      var tsTitle = d.bucket === 'hour' ? 'Prompts per hour' : 'Prompts per day';
      var tsTotal = d.timeseries.reduce(function (a, b) { return a + b.count; }, 0);
      var periods = [['day', 'Today'], ['week', 'This week'], ['month', 'This month'], ['all', 'All time']];
      var pbtns = periods.map(function (p) { return '<button class="btn ' + (usagePeriod === p[0] ? '' : 'ghost') + '" data-act="usage-period" data-period="' + p[0] + '">' + p[1] + '</button>'; }).join('');
      var legend = pie.map(function (s) { var nm = s.label === 'loopback' ? '<span class="tag-loop">loopback</span>' : esc(s.label); return '<li><span class="sw" style="background:' + s.color + '"></span>' + nm + '<span class="lv">' + fmtNum(s.value) + ' · ' + (pieTotal ? Math.round(s.value / pieTotal * 100) : 0) + '%</span></li>'; }).join('');
      var tbody = users.length ? users.map(function (u) {
        var nm = u.user === 'loopback' ? '<span class="tag-loop">loopback</span>' : (u.user === 'unknown' ? '<span class="muted">unknown</span>' : '<strong>' + esc(u.user) + '</strong>');
        return '<tr class="clickrow" data-act="usage-user" data-user="' + esc(u.user) + '" title="Click to see this user\'s logs"><td>' + nm + '</td><td><code>' + esc(u.ip) + '</code></td><td>' + fmtNum(u.requests) + '</td><td>' + fmtNum(u.prompts) + '</td>' +
          '<td>' + (u.denied > 0 ? '<span class="warn-tag">' + fmtNum(u.denied) + '</span>' : '0') + '</td><td>' + (u.errors > 0 ? '<span style="color:var(--err)">' + fmtNum(u.errors) + '</span>' : '0') + '</td>' +
          '<td class="nowrap">' + (u.avgLatency ? esc(u.avgLatency) : '<span class="muted">—</span>') + '</td><td class="nowrap muted">' + esc(u.last) + '</td></tr>';
      }).join('') : '<tr><td colspan="8" class="muted">No requests in this period.</td></tr>';
      setMain(
        '<div class="row" style="align-items:center">' +
        '<h1 style="margin:0">Usage by user</h1>' +
        '<label>From <input type="date" id="ufrom" value="' + esc(usageFrom) + '" min="' + esc(d.minDate) + '" max="' + esc(d.maxDate) + '"></label>' +
        '<label>To <input type="date" id="uto" value="' + esc(usageTo) + '" min="' + esc(d.minDate) + '" max="' + esc(d.maxDate) + '"></label>' +
        '<button class="btn" data-act="usage-applyrange">Apply range</button>' + pbtns + '</div>' +
        '<div class="cards"><div class="stat"><div class="num">' + users.length + '</div><div class="lbl">Users seen</div></div>' +
        '<div class="stat"><div class="num">' + fmtNum(totalReq) + '</div><div class="lbl">Requests</div></div>' +
        '<div class="stat"><div class="num">' + fmtNum(totalPrompts) + '</div><div class="lbl">Prompts</div></div></div>' +
        (users.length ?
          '<div class="card"><div class="charts">' +
          '<div class="chartbox"><h3>' + tsTitle + ' <span class="muted" style="font-weight:400;font-size:12px">(' + fmtNum(tsTotal) + ' prompts)</span></h3>' + svgTimebars(d.timeseries, d.bucket) + '</div>' +
          '<div class="chartbox"><h3>Top 5 users</h3><div class="pierow">' + svgDonut(pie) + '<ul class="legend">' + legend + '</ul></div></div>' +
          '<div class="chartbox"><h3>Requests per user</h3><div class="ubars">' + bars + '</div></div>' +
          '</div></div>' +
          '<div class="card"><h2>Details</h2><table class="grid"><thead><tr><th>User</th><th>IP</th><th>Requests</th><th>Prompts</th><th title="4xx — incl. 403 denials for non-loopback">4xx</th><th>5xx</th><th>Avg latency</th><th>Last seen</th></tr></thead><tbody>' + tbody + '</tbody></table></div>'
          : '<div class="card"><p class="muted">No requests in this period.</p></div>')
      );
    }).catch(showError);
  }

  // ---- routing -------------------------------------------------------------
  function setActive(view) {
    var links = document.querySelectorAll('#nav a[data-view]');
    for (var i = 0; i < links.length; i++) links[i].className = links[i].getAttribute('data-view') === view ? 'on' : '';
  }
  function route() {
    var v = (location.hash || '#dashboard').replace('#', '') || 'dashboard';
    if (v === 'chat') { window.location.href = 'index.html'; return; }
    if (v === 'accounts') v = 'users';                       // merged into Users & access
    if (['dashboard', 'users', 'logs', 'usage'].indexOf(v) === -1) v = 'dashboard';
    setActive(v);
    if (v === 'dashboard') renderDashboard();
    else if (v === 'users') renderUsers();
    else if (v === 'logs') renderLogs();
    else if (v === 'usage') renderUsage();
  }
  window.addEventListener('hashchange', route);

  // ---- global click / submit delegation ------------------------------------
  document.addEventListener('click', function (e) {
    var t = e.target.closest ? e.target.closest('[data-act]') : null;
    if (!t) return;
    var act = t.getAttribute('data-act');
    if (act === 'detail') { e.preventDefault(); openDetail(t.getAttribute('data-sig')); }
    else if (act === 'logout') { e.preventDefault(); doLogout(); }
    else if (act === 'logout-denied') { e.preventDefault(); api('logout', { body: {} }).then(showLoggedOut).catch(showLoggedOut); }
    else if (act === 'user-add') { e.preventDefault(); openHostModal(null); }
    else if (act === 'user-edit') { e.preventDefault(); openHostModal(t.getAttribute('data-ip')); }
    else if (act === 'user-delete') { e.preventDefault(); var uip = t.getAttribute('data-ip'), ual = t.getAttribute('data-alias'); uiConfirm('Remove ' + ual + ' (' + uip + ') from the allowed hosts?', { title: 'Remove host', danger: true, okText: 'Remove' }).then(function (ok) { if (ok) saveUser({ act: 'delete', ip: uip }); }); }
    else if (act === 'srv-toggle') {
      e.preventDefault();
      if (t.getAttribute('data-running') === '1') { uiConfirm('Stop llama-server? Any in-progress generations will be cut off.', { title: 'Stop server', danger: true, okText: 'Stop' }).then(function (ok) { if (ok) srvAction('server_stop'); }); }
      else srvAction('server_start');
    }
    else if (act === 'launch-edit') { e.preventDefault(); window.openLaunchModal(); }
    else if (act === 'srv-logs') { e.preventDefault(); window.openServerLog(); }
    else if (act === 'srv-save') { e.preventDefault(); srvSave(); }
    else if (act === 'act-range') { e.preventDefault(); actDays = parseInt(t.getAttribute('data-days'), 10) || 14; actFrom = ''; actTo = ''; reloadActivity(); }
    else if (act === 'act-apply') { e.preventDefault(); var af = document.getElementById('actfrom'), at = document.getElementById('actto'); var fv = af ? af.value : '', tv = at ? at.value : ''; if (fv && tv) { actFrom = fv; actTo = tv; actDays = 0; reloadActivity(); } }
    else if (act === 'acct-approve') { e.preventDefault(); acctAction('approve', t.getAttribute('data-id')); }
    else if (act === 'acct-approve-allow') { e.preventDefault(); acctAction('approve_allow', t.getAttribute('data-id')); }
    else if (act === 'acct-make-admin') { e.preventDefault(); var ma = t.getAttribute('data-id'), mn = t.getAttribute('data-name'); uiConfirm('Make “' + mn + '” an administrator? They will have full access to this admin panel.', { title: 'Grant admin', okText: 'Make admin' }).then(function (ok) { if (ok) acctAction('make_admin', ma); }); }
    else if (act === 'acct-revoke-admin') { e.preventDefault(); var ra = t.getAttribute('data-id'), rn = t.getAttribute('data-name'); uiConfirm('Revoke administrator access from “' + rn + '”?', { title: 'Revoke admin', danger: true, okText: 'Revoke' }).then(function (ok) { if (ok) acctAction('revoke_admin', ra); }); }
    else if (act === 'acct-allow') { e.preventDefault(); acctAction('allow_host', t.getAttribute('data-id')); }
    else if (act === 'acct-reset-pw') { e.preventDefault(); openResetPassword(t.getAttribute('data-id'), t.getAttribute('data-name')); }
    else if (act === 'acct-suspend') { e.preventDefault(); acctAction('suspend', t.getAttribute('data-id')); }
    else if (act === 'acct-reject') { e.preventDefault(); var rja = t.getAttribute('data-id'), rjn = t.getAttribute('data-name'); uiConfirm('Reject and delete the signup “' + rjn + '”?', { title: 'Reject signup', danger: true, okText: 'Reject' }).then(function (ok) { if (ok) acctAction('reject', rja); }); }
    else if (act === 'acct-delete') { e.preventDefault(); var da = t.getAttribute('data-id'), dn = t.getAttribute('data-name'); uiConfirm('Delete account “' + dn + '”? Their saved chats are not removed.', { title: 'Delete account', danger: true, okText: 'Delete' }).then(function (ok) { if (ok) acctAction('delete', da); }); }
    else if (act === 'log-user') { e.preventDefault(); var f = document.getElementById('lfilter'); if (f) f.value = t.getAttribute('data-user') || ''; logPage = 1; renderLogBody(); }
    else if (act === 'suspend-log-user') { e.preventDefault(); var sid = t.getAttribute('data-id'), snm = t.getAttribute('data-name') || sid; if (sid) { uiConfirm('Suspend user “' + snm + '”? They will be signed out and their host removed from the allow-list.', { title: 'Suspend user', danger: true, okText: 'Suspend' }).then(function (ok) { if (ok) { api('account_action', { body: { act: 'suspend', id: sid } }).then(function () { window.closeDetail(); if (browsingBackup) { openBackup(browsingBackup); } else { reloadLogs(); } }).catch(showError); } }); } }
    else if (act === 'logs-backup') { e.preventDefault(); uiConfirm('Zip every request log before today and delete the originals? Today’s log is kept, and the zip stays in the log directory.', { title: 'Backup & clear logs', danger: true, okText: 'Backup & clear' }).then(function (ok) { if (ok) doLogBackup(); }); }
    else if (act === 'logs-browse') { e.preventDefault(); openBackup(t.getAttribute('data-zip')); }
    else if (act === 'logs-live') { e.preventDefault(); browsingBackup = null; renderLogs(); }
    else if (act === 'logs-prev') { logPage--; renderLogBody(); }
    else if (act === 'logs-next') { logPage++; renderLogBody(); }
    else if (act === 'logs-apply') { e.preventDefault(); logFrom = document.getElementById('lfrom').value; logTo = document.getElementById('lto').value; reloadLogs(); }
    else if (act === 'logs-reset') {
      e.preventDefault();
      logFrom = isoToday(); logTo = isoToday();   // back to the default (today)
      var lf = document.getElementById('lfrom'); if (lf) lf.value = logFrom;
      var lt = document.getElementById('lto'); if (lt) lt.value = logTo;
      var flt = document.getElementById('lfilter'); if (flt) flt.value = '';
      var po = document.getElementById('lpromptonly'); if (po) po.checked = true;
      logPage = 1;
      reloadLogs();
    }
    else if (act === 'usage-period') { e.preventDefault(); usagePeriod = t.getAttribute('data-period'); usageFrom = ''; usageTo = ''; computeUsageRange(); renderUsage(); }
    else if (act === 'usage-applyrange') { e.preventDefault(); usagePeriod = 'custom'; usageFrom = document.getElementById('ufrom').value; usageTo = document.getElementById('uto').value; renderUsage(); }
    else if (act === 'usage-user') { e.preventDefault(); pendingLog = { filter: t.getAttribute('data-user') || '', from: usageFrom, to: usageTo }; if (location.hash === '#logs') route(); else location.hash = '#logs'; }
  });
  document.addEventListener('submit', function (e) {
    var f = e.target;
    if (f.id === 'setupform') { e.preventDefault(); doSetup(f); }
    else if (f.id === 'loginform') { e.preventDefault(); doLogin(f); }
    else if (f.id === 'userform') { e.preventDefault(); var fd = formData(f); saveUser(fd); }
  });
  function formData(f) { var o = {}, els = f.elements; for (var i = 0; i < els.length; i++) { if (els[i].name) o[els[i].name] = els[i].value; } return o; }

  // ---- actions -------------------------------------------------------------
  function doSetup(f) { var d = formData(f); if (d.password !== d.password2) { authErr('Passwords do not match.'); return; } api('setup', { body: { password: d.password } }).then(function () { return refreshCsrf(); }).then(showLogin).catch(authErr); }
  function doLogin(f) { var d = formData(f); api('login', { body: { password: d.password } }).then(function (r) { if (r.csrf) CSRF = r.csrf; enterApp(); }).catch(authErr); }
  function doLogout() { api('logout', { body: {} }).then(boot).catch(boot); }
  function saveUser(fd) { api('users_save', { body: fd }).then(function (r) { if (r.errors) { var m = document.getElementById('usermsg'); if (m) m.innerHTML = r.errors.map(function (x) { return '<div class="flash err">' + esc(x) + '</div>'; }).join(''); } else { usersCache = r.users; window.closeHostModal(); drawUsersPage(); } }).catch(function (e) { var m = document.getElementById('usermsg'); if (m) m.innerHTML = '<div class="flash err">' + esc(e) + '</div>'; }); }
  function reloadLogs() {
    stopTimers(); logRows = []; logSeen = {}; logPage = 1;
    var q = {}; if (logFrom) q.from = logFrom; if (logTo) q.to = logTo;
    api('logs', { query: q }).then(function (d) { logRows = d.rows; d.rows.forEach(function (r) { logSeen[r.sig] = 1; }); logBackups = d.backups || logBackups; var bc = document.getElementById('backupscard'); if (bc) bc.innerHTML = backupsHtml(); applyLogs(); if (document.getElementById('llive') && document.getElementById('llive').checked) logsTimer = setInterval(pollLogs, 4000); }).catch(showError);
  }
  function computeUsageRange() {
    var today = isoToday();
    if (usagePeriod === 'day') { usageFrom = today; usageTo = today; }
    else if (usagePeriod === 'week') { usageFrom = isoDaysAgo(6); usageTo = today; }
    else if (usagePeriod === 'month') { usageFrom = isoDaysAgo(29); usageTo = today; }
    else { usageFrom = ''; usageTo = ''; }
  }
  function isoToday() { var d = new Date(); return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()); }
  function isoDaysAgo(n) { var d = new Date(); d.setDate(d.getDate() - n); return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()); }
  function pad(n) { return (n < 10 ? '0' : '') + n; }

  // ---- boot ----------------------------------------------------------------
  function refreshCsrf() { return api('session').then(function (s) { CSRF = s.csrf; return s; }); }
  function enterApp() {
    document.getElementById('nav').hidden = false;
    document.getElementById('logoutlink').setAttribute('data-act', 'logout');
    if (!location.hash || location.hash === '#chat') location.hash = '#dashboard';
    computeUsageRange();
    route();
  }
  function showHostDenied(s) {
    var loggedIn = !!(s && (s.authed || s.user));   // only offer Log out when there's actually a session to clear
    document.getElementById('nav').hidden = true;
    setMain('<div class="authwrap"><div class="card narrow"><h1>Access denied</h1>' +
      '<p class="muted">The admin panel is available from the server itself (<code>http://localhost/admin</code>), or from the exact device (IP) registered to your administrator account — sign in through the chat there, then use the Admin link. This device isn’t authorized.</p>' +
      '<p><a class="btn ghost" href="index.html">Go to chat</a>' +
      (loggedIn ? ' <button class="btn ghost" data-act="logout-denied">Log out</button>' : '') +
      '</p></div></div>');
  }
  function showLoggedOut() {
    document.getElementById('nav').hidden = true;
    setMain('<div class="authwrap"><div class="card narrow"><h1>Logged out</h1>' +
      '<p class="muted">Your admin session has been cleared. The admin panel is only available from the server (<code>http://localhost/admin</code>).</p>' +
      '<p><a class="btn ghost" href="index.html">Go to chat</a></p></div></div>');
  }
  function boot() {
    stopTimers();
    api('session').then(function (s) {
      CSRF = s.csrf;
      if (s.hostAllowed === false) { showHostDenied(s); return; }   // admin panel is localhost-only — no login form off-server
      if (s.isAdmin) enterApp();             // admin password OR admin-role chat user — no extra login
      else if (s.needsSetup) showSetup();
      else showLogin();
    }).catch(function (e) { showError('Cannot reach API: ' + e); });
  }
  boot();
})();
