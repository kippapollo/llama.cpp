/* Standalone AI Coding Agent chat front-end.
   Talks to api.php (chat proxy -> llama-server). Sessions persist in
   localStorage; supports text file attachments. ES5-style for broad support. */
(function () {
  'use strict';

  var CSRF = '';
  var SKEY = 'aica-sessions';
  var userId = null, userName = null;   // logged-in chat user
  var searchQuery = '';       // sidebar chat search
  var saveTimer = null;       // debounce for server-side chat save
  var appWired = false;       // one-time event wiring guard
  var isAdmin = false;        // current chat user has the 'admin' role
  var sessions = [];          // [{id,title,messages:[{role,content}],updated}]
  var current = null;         // current session object
  var editingId = null;       // session id being inline-renamed
  var attachments = [];       // [{name,content}]
  var streaming = false;
  var ctxSize = 0;            // model context window (n_ctx), from /props
  var abortCtl = null;        // AbortController for the in-flight chat stream
  var SEND_ICON = '&#10148;';
  var STOP_ICON = '<svg viewBox="0 0 24 24" width="14" height="14" aria-hidden="true"><rect x="6" y="6" width="12" height="12" rx="2" fill="currentColor"/></svg>';
  var COPY_ICON = '<svg class="i" viewBox="0 0 24 24" width="13" height="13" aria-hidden="true"><path fill="currentColor" d="M16 1H4a2 2 0 0 0-2 2v12h2V3h12V1zm3 4H8a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h11a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2zm0 16H8V7h11v14z"/></svg> ';
  var EDIT_ICON = '<svg class="i" viewBox="0 0 24 24" width="13" height="13" aria-hidden="true"><path fill="currentColor" d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.58z"/></svg> ';
  var PIN_ICON = '<svg viewBox="0 0 24 24" width="13" height="13" aria-hidden="true"><path fill="currentColor" d="M16 12V4h1V2H7v2h1v8l-2 2v2h5.2v6h1.6v-6H18v-2z"/></svg>';

  // ---- helpers -------------------------------------------------------------
  function esc(s) { return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]; }); }
  function el(id) { return document.getElementById(id); }
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
  function uiAlert(message, o) { o = o || {}; o.message = message; o.alert = true; return uiDialog(o); }
  function openChangePassword() {
    var ov = document.createElement('div'); ov.className = 'uidlg-overlay';
    ov.innerHTML = '<div class="uidlg"><div class="uidlg-title">Change password</div>' +
      '<form id="pwform" autocomplete="off">' +
      '<label>Current password <input type="password" name="current" autocomplete="current-password" required></label>' +
      '<label>New password <input type="password" name="newpw" autocomplete="new-password" required></label>' +
      '<label>Confirm new password <input type="password" name="newpw2" autocomplete="new-password" required></label>' +
      '<div id="pwmsg"></div>' +
      '<div class="uidlg-btns"><button type="button" class="btn ghost" data-pw="cancel">Cancel</button><button type="submit" class="btn">Change password</button></div>' +
      '</form></div>';
    document.body.appendChild(ov);
    function close() { if (ov.parentNode) ov.parentNode.removeChild(ov); }
    ov.addEventListener('click', function (e) { if (e.target === ov || (e.target.getAttribute && e.target.getAttribute('data-pw') === 'cancel')) close(); });
    ov.querySelector('#pwform').addEventListener('submit', function (e) {
      e.preventDefault();
      var f = this, cur = f.current.value, nw = f.newpw.value, nw2 = f.newpw2.value, msg = ov.querySelector('#pwmsg');
      if (nw !== nw2) { msg.innerHTML = '<div class="flash err">New passwords do not match.</div>'; return; }
      if (nw.length < 8) { msg.innerHTML = '<div class="flash err">New password must be at least 8 characters.</div>'; return; }
      msg.innerHTML = '<div class="muted">Saving&hellip;</div>';
      api('user_password', { body: { current: cur, 'new': nw } })
        .then(function () { msg.innerHTML = '<div class="flash ok">Password changed.</div>'; setTimeout(close, 1100); })
        .catch(function (er) { msg.innerHTML = '<div class="flash err">' + esc(er) + '</div>'; });
    });
    var inp = ov.querySelector('input'); if (inp) inp.focus();
  }
  function copyText(text, btn) {
    var orig = btn ? btn.innerHTML : null;
    function done() { if (btn) { btn.innerHTML = '&#10003; Copied'; btn.className += ' done'; setTimeout(function () { btn.innerHTML = orig; btn.className = btn.className.replace(/\s*done/, ''); }, 1300); } }
    function fallback() { var ta = document.createElement('textarea'); ta.value = text; ta.style.position = 'fixed'; ta.style.left = '-9999px'; document.body.appendChild(ta); ta.focus(); ta.select(); try { document.execCommand('copy'); done(); } catch (e) {} document.body.removeChild(ta); }
    if (navigator.clipboard && navigator.clipboard.writeText) { navigator.clipboard.writeText(text).then(done, fallback); } else { fallback(); }
  }
  function setStreamUI(on) {
    var b = el('chatsend'); if (!b) return;
    b.disabled = false;
    if (on) { b.className = 'sendbtn stopbtn'; b.title = 'Stop generating'; b.innerHTML = STOP_ICON; }
    else { b.className = 'sendbtn'; b.title = 'Send'; b.innerHTML = SEND_ICON; }
  }
  function setAgentState(s) {
    var b = el('agentbg'); if (!b) return;
    b.className = 'agentbg' + (s === 'busy' ? ' show on' : s === 'waiting' ? ' show' : '');
    if (s === 'off') stopJoker(); else startJoker();
  }
  var jokerTimer = null;
  function startJoker() { if (!jokerTimer) jokerTimer = setInterval(doJoker, 2600); }
  function stopJoker() { if (jokerTimer) { clearInterval(jokerTimer); jokerTimer = null; } }
  function doJoker() {
    var bg = el('agentbg'); if (!bg || bg.className.indexOf('show') < 0) return;
    var bot = bg.querySelector('.bot'); if (!bot) return;
    if (bg.className.indexOf('on') < 0 && Math.random() < 0.45) return;   // react less often when idle
    var moves = ['react-spin', 'react-hop', 'react-wob'], m = moves[Math.floor(Math.random() * moves.length)];
    bot.classList.remove('react-spin', 'react-hop', 'react-wob');
    void bot.offsetWidth;
    bot.classList.add(m);
    setTimeout(function () { if (bot) bot.classList.remove(m); }, 900);
  }
  function stopChat() { if (abortCtl) { try { abortCtl.abort(); } catch (e) {} } }
  function nowMs() { return (window.performance && performance.now) ? performance.now() : (new Date()).getTime(); }
  function fmtTok(n) { n = Math.round(n || 0); return n >= 1000 ? (n / 1000).toFixed(n >= 10000 ? 0 : 1) + 'k' : '' + n; }
  function estTok(s) { return s ? Math.max(1, Math.round(s.length / 4)) : 0; }
  function fmtStats(s) {
    if (!s) return '';
    var p = [];
    if (s.live) {
      p.push('&#9889; ' + (s.elapsed / 1000).toFixed(1) + 's');
      p.push(fmtTok(s.completion) + ' tok');
      if (s.tokps) p.push(Math.round(s.tokps) + ' tok/s');
    } else {
      if (s.stopped) p.push('&#9209;&nbsp;stopped');
      if (s.ttft) p.push('first&nbsp;' + (s.ttft / 1000).toFixed(2) + 's');
      if (s.tokps) p.push(Math.round(s.tokps) + '&nbsp;tok/s');
      p.push((s.prompt ? fmtTok(s.prompt) + '+' : '') + fmtTok(s.completion) + '&nbsp;tok');
      if (s.elapsed) p.push((s.elapsed / 1000).toFixed(2) + 's');
      if (s.ctx && s.total) { p.push('ctx&nbsp;' + fmtTok(s.total) + '/' + fmtTok(s.ctx) + '&nbsp;(' + Math.round(s.total / s.ctx * 100) + '%)'); }
    }
    return p.join(' &middot; ');
  }
  function api(action, opts) {
    opts = opts || {};
    var init = { credentials: 'same-origin', headers: {} };
    if (opts.body) { init.method = 'POST'; init.headers['Content-Type'] = 'application/json'; init.headers['X-CSRF'] = CSRF; init.body = JSON.stringify(opts.body); }
    return fetch('api.php?action=' + encodeURIComponent(action), init).then(function (r) {
      return r.json().then(function (j) { if (!r.ok) { throw (j && j.error) || ('HTTP ' + r.status); } return j; });
    });
  }
  window.toggleTheme = function () {
    var d = document.documentElement, next = d.getAttribute('data-theme') === 'light' ? 'dark' : 'light';
    if (next === 'light') d.setAttribute('data-theme', 'light'); else d.removeAttribute('data-theme');
    try { localStorage.setItem('admin-theme', next); } catch (e) {}
  };
  function setSidebar(collapsed) {
    if (collapsed) document.body.classList.add('sb-collapsed'); else document.body.classList.remove('sb-collapsed');
    try { localStorage.setItem('aica-sb', collapsed ? '1' : '0'); } catch (e) {}
  }
  function applySidebar() { var c = false; try { c = localStorage.getItem('aica-sb') === '1'; } catch (e) {} if (c) document.body.classList.add('sb-collapsed'); }

  // ---- markdown (same renderer as the admin) -------------------------------
  function renderMarkdown(src) {
    src = String(src || ''); var blocks = [];
    src = src.replace(/```(\w*)\r?\n?([\s\S]*?)```/g, function (_, lang, code) {
      var clean = code.replace(/\s+$/, '');
      blocks.push('<div class="codeblock"><div class="cbhead"><span class="cblang">' + esc(lang || 'code') +
        '</span><button type="button" class="cbcopy" data-copy>' + COPY_ICON + 'Copy</button></div>' +
        '<pre class="code"><code>' + esc(clean) + '</code></pre></div>');
      return '@@' + (blocks.length - 1) + '@@';
    });
    src = esc(src);
    src = src.replace(/^######\s+(.*)$/gm, '<h6>$1</h6>').replace(/^#####\s+(.*)$/gm, '<h5>$1</h5>').replace(/^####\s+(.*)$/gm, '<h4>$1</h4>').replace(/^###\s+(.*)$/gm, '<h3>$1</h3>').replace(/^##\s+(.*)$/gm, '<h2>$1</h2>').replace(/^#\s+(.*)$/gm, '<h1>$1</h1>');
    src = src.replace(/`([^`]+)`/g, '<code>$1</code>');
    src = src.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>').replace(/\*([^*\n]+)\*/g, '<em>$1</em>');
    src = '<p>' + src.replace(/\n{2,}/g, '</p><p>') + '</p>';
    src = src.replace(/\n/g, '<br>');
    // a lone code-block sentinel must not stay wrapped in <p> (invalid: div-in-p)
    src = src.replace(/<p>\s*(?:<br>)?\s*@@(\d+)@@\s*(?:<br>)?\s*<\/p>/g, function (_, i) { return blocks[+i]; });
    src = src.replace(/@@(\d+)@@/g, function (_, i) { return blocks[+i]; });
    src = src.replace(/<p>(\s*<h[1-6][\s\S]*?<\/h[1-6]>\s*)<\/p>/g, '$1');
    return src;
  }

  // Split a raw stream that may contain a <think>...</think> block (used when
  // the server keeps reasoning inline instead of in a separate field).
  function splitThink(raw) {
    var open = raw.indexOf('<think>');
    if (open === -1) return { reasoning: '', answer: raw };
    var before = raw.slice(0, open);
    var close = raw.indexOf('</think>', open + 7);
    if (close === -1) return { reasoning: raw.slice(open + 7), answer: before };       // still thinking
    return { reasoning: raw.slice(open + 7, close), answer: before + raw.slice(close + 8) };
  }

  // ---- sessions (server-side per user, with a localStorage cache) -----------
  function skey() { return SKEY + ':' + (userId || 'anon'); }
  function loadSessions() { try { sessions = JSON.parse(localStorage.getItem(skey())) || []; } catch (e) { sessions = []; } if (!(sessions instanceof Array)) sessions = []; }
  function saveSessions() {
    try { localStorage.setItem(skey(), JSON.stringify(sessions)); } catch (e) {}   // instant local cache
    if (saveTimer) clearTimeout(saveTimer);
    saveTimer = setTimeout(serverSaveNow, 800);                                     // debounced server persist
  }
  function serverSaveNow() {
    saveTimer = null;
    api('chats_save', { body: { sessions: sessions } }).catch(function () {});
  }
  function uid() { return 's' + (new Date().getTime()) + Math.floor(Math.random() * 1000); }
  function newSession() {
    current = { id: uid(), title: 'New chat', messages: [], updated: nowStr() };
    sessions.unshift(current); saveSessions(); renderSessions(); paintChat();
    var i = el('chatinput'); if (i) i.focus();
  }
  function nowStr() { var d = new Date(); return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes()); }
  function pad(n) { return (n < 10 ? '0' : '') + n; }
  function selectSession(id) { for (var i = 0; i < sessions.length; i++) { if (sessions[i].id === id) { current = sessions[i]; break; } } renderSessions(); paintChat(); }
  function deleteSession(id) {
    sessions = sessions.filter(function (s) { return s.id !== id; });
    if (!current || current.id === id) { current = sessions[0] || null; }
    if (!current) newSession(); else { saveSessions(); renderSessions(); paintChat(); }
    saveSessions();
  }
  function touchSession() {
    if (!current) return;
    if ((!current.title || current.title === 'New chat') && current.messages.length) {
      var first = current.messages[0].content || '';
      current.title = first.replace(/\s+/g, ' ').slice(0, 40) || 'New chat';
    }
    current.updated = nowStr();
    // move current to top
    sessions = sessions.filter(function (s) { return s.id !== current.id; });
    sessions.unshift(current);
    saveSessions(); renderSessions();
  }
  function findSession(id) { for (var i = 0; i < sessions.length; i++) { if (sessions[i].id === id) return sessions[i]; } return null; }
  function togglePin(id) { var s = findSession(id); if (!s) return; s.pinned = !s.pinned; saveSessions(); renderSessions(); }
  function startRename(id) { editingId = id; renderSessions(); var inp = el('sessionlist').querySelector('.srename'); if (inp) { inp.focus(); inp.select(); } }
  function commitRename(id, val) {
    if (editingId == null) return;
    editingId = null;
    var s = findSession(id);
    if (s) { var v = (val || '').replace(/\s+/g, ' ').trim(); if (v) s.title = v.slice(0, 60); }
    saveSessions(); renderSessions();
  }
  function sessionMatches(s, q) {
    if (!q) return true;
    if (('' + (s.title || '')).toLowerCase().indexOf(q) >= 0) return true;
    var msgs = s.messages || [];
    for (var i = 0; i < msgs.length; i++) {
      var m = msgs[i];
      var t = (m.display != null ? m.display : m.content) || '';
      if (('' + t).toLowerCase().indexOf(q) >= 0) return true;
      if (m.reasoning && ('' + m.reasoning).toLowerCase().indexOf(q) >= 0) return true;
    }
    return false;
  }
  function renderSessions() {
    var ul = el('sessionlist'); if (!ul) return;
    var q = searchQuery;
    var pinned = [], rest = [];
    sessions.forEach(function (s) { if (sessionMatches(s, q)) (s.pinned ? pinned : rest).push(s); });
    if (!pinned.length && !rest.length) { ul.innerHTML = '<li class="noresult muted">' + (q ? 'No chats match “' + esc(q) + '”.' : 'No chats yet.') + '</li>'; return; }
    ul.innerHTML = pinned.concat(rest).map(function (s) {
      var titleCell = (s.id === editingId)
        ? '<input class="srename" data-id="' + esc(s.id) + '" value="' + esc(s.title) + '" maxlength="60">'
        : '<span class="stitle" title="' + esc(s.title) + '">' + esc(s.title) + '</span>';
      return '<li class="' + (current && s.id === current.id ? 'active' : '') + (s.pinned ? ' pinned' : '') + '" data-id="' + esc(s.id) + '">' +
        '<button type="button" class="spin" data-pin="' + esc(s.id) + '" title="' + (s.pinned ? 'Unpin' : 'Pin') + '">' + PIN_ICON + '</button>' +
        titleCell +
        '<span class="sact">' +
          '<button type="button" class="siconbtn" data-rename="' + esc(s.id) + '" title="Rename">' + EDIT_ICON + '</button>' +
          '<button type="button" class="siconbtn sdelbtn" data-del="' + esc(s.id) + '" title="Delete">&#10005;</button>' +
        '</span></li>';
    }).join('');
  }

  // ---- chat rendering ------------------------------------------------------
  function bubble(m, i) {
    if (m.role === 'assistant') {
      var aact = '<div class="cmsg-actions"><button type="button" class="msgbtn" data-act="copy-msg" data-i="' + i + '" title="Copy answer">' + COPY_ICON + 'Copy</button></div>';
      var hasAnswer = !!(m.content && m.content.trim());
      var isLast = !!(current && i === current.messages.length - 1);   // only the in-flight message shows live states
      var think = '';
      if (m.reasoning && m.reasoning.trim()) {
        var liveThink = streaming && isLast && !hasAnswer;       // still reasoning, no answer yet
        var label = liveThink ? 'Thinking&hellip;' : 'Reasoning';
        think = '<div class="thinkbox' + (liveThink ? ' open' : '') + '">' +
          '<button type="button" class="thinkhead" data-act="toggle-think"><span class="thinkico">&#128173;</span> ' + label +
          ' <span class="thinkchev">&#9656;</span></button><div class="thinkbody">' + renderMarkdown(m.reasoning) + '</div></div>';
      }
      var waiting = streaming && isLast && !hasAnswer && !(m.reasoning && m.reasoning.trim());
      var answer;
      if (hasAnswer) answer = renderMarkdown(m.content);
      else if (think) answer = '';
      else if (waiting) answer = '<div class="processing"><span class="dots"><span></span><span></span><span></span></span><span class="proctime">Processing prompt&hellip; 0.0s</span></div>';
      else if (m.stats && m.stats.stopped) answer = '<p class="muted">&#9209; Stopped before any output.</p>';
      else answer = '<p class="muted">&hellip;</p>';
      var stats = m.stats ? '<div class="cstats">' + fmtStats(m.stats) + '</div>' : '';
      return '<div class="cmsg cmsg-assistant" data-i="' + i + '"><div class="ccontent">' + think + answer + '</div>' + (waiting ? '' : stats) + aact + '</div>';
    }
    var text = (m.display != null ? m.display : m.content) || '';
    var atts = '';
    if (m.files && m.files.length) {
      atts = '<div class="cmsg-attach">&#128206;' + m.files.map(function (n) { return '<span class="f">' + esc(n) + '</span>'; }).join('') + '</div>';
    }
    var inner = text ? '<div class="ccontent"><p>' + esc(text).replace(/\n/g, '<br>') + '</p></div>' : '';
    var uact = '<div class="cmsg-actions">' +
      '<button type="button" class="msgbtn" data-act="copy-msg" data-i="' + i + '" title="Copy">' + COPY_ICON + 'Copy</button>' +
      '<button type="button" class="msgbtn" data-act="edit-msg" data-i="' + i + '" title="Edit &amp; resend">' + EDIT_ICON + 'Edit</button></div>';
    return '<div class="cmsg cmsg-user" data-i="' + i + '">' + inner + atts + uact + '</div>';
  }
  function editMessage(i) {
    if (streaming || !current) return;
    var m = current.messages[i]; if (!m || m.role !== 'user') return;
    var inp = el('chatinput');
    inp.value = (m.display != null ? m.display : m.content) || '';
    attachments = (m.atts && m.atts.length) ? m.atts.map(function (a) { return { name: a.name, content: a.content }; }) : [];
    renderAttachments();
    current.messages = current.messages.slice(0, i); // drop this prompt + everything after
    saveSessions(); renderSessions(); paintChat();
    inp.style.height = 'auto'; inp.style.height = Math.min(inp.scrollHeight, 200) + 'px';
    inp.focus();
  }
  function scrollChat() { var c = el('chatlog'); if (c) c.scrollTop = c.scrollHeight; updateScrollDown(); }
  function updateScrollDown() {
    var c = el('chatlog'), b = el('scrolldown'); if (!c || !b) return;
    b.hidden = (c.scrollTop + c.clientHeight >= c.scrollHeight - 80);   // hide when near the bottom
  }
  function paintChat() {
    var c = el('chatlog'); if (!c) return;
    var msgs = current ? current.messages : [];
    c.innerHTML = msgs.length ? msgs.map(bubble).join('')
      : '<div class="emptychat"><div class="big">&#129302;</div><h2>AI Coding Agent</h2><p class="muted">Ask a coding question, or attach a file and ask about it. This agent answers software &amp; coding topics only.</p></div>';
    scrollChat();
  }

  // ---- attachments ---------------------------------------------------------
  function renderAttachments() {
    var bar = el('attachbar');
    bar.innerHTML = attachments.map(function (a, i) {
      return '<span class="attachchip">&#128196; ' + esc(a.name) + ' <span class="muted">(' + fmtKB(a.content.length) + ')</span><span class="x" data-att="' + i + '">&#10005;</span></span>';
    }).join('');
  }
  function fmtKB(n) { return n >= 1024 ? Math.round(n / 1024) + ' KB' : n + ' B'; }
  function addFiles(files) {
    var max = 200 * 1024;
    Array.prototype.slice.call(files).forEach(function (f) {
      if (f.size > max) { uiAlert('“' + f.name + '” is too large (max 200 KB of text).', { title: 'File too large' }); return; }
      var reader = new FileReader();
      reader.onload = function () { attachments.push({ name: f.name, content: String(reader.result || '') }); renderAttachments(); };
      reader.readAsText(f);
    });
  }

  // ---- send / stream -------------------------------------------------------
  function buildUserContent(text) {
    var s = '';
    attachments.forEach(function (a) { s += 'Attached file `' + a.name + '`:\n```\n' + a.content + '\n```\n\n'; });
    return s + text;
  }
  function sendChat() {
    if (streaming || !current) return;
    var inp = el('chatinput'); var text = (inp.value || '').trim();
    if (!text && !attachments.length) return;
    var fileNames = attachments.map(function (a) { return a.name; });
    var atts = attachments.map(function (a) { return { name: a.name, content: a.content }; });
    current.messages.push({ role: 'user', content: buildUserContent(text), display: text, files: fileNames, atts: atts });
    var asst = { role: 'assistant', content: '' };
    current.messages.push(asst);
    attachments = []; renderAttachments();
    inp.value = ''; inp.style.height = 'auto';
    streaming = true; setStreamUI(true); setAgentState('busy');   // set before paintChat so the processing indicator renders
    abortCtl = (window.AbortController) ? new AbortController() : null;
    paintChat(); touchSession();
    var t0 = nowMs(), tFirst = 0, usage = null, timings = null, aborted = false;
    var procTimer = setInterval(function () {
      if (tFirst || !streaming) { clearInterval(procTimer); procTimer = null; return; }
      var pt = document.querySelector('#chatlog .cmsg-assistant:last-child .proctime');
      if (pt) pt.innerHTML = 'Processing prompt&hellip; ' + ((nowMs() - t0) / 1000).toFixed(1) + 's';
    }, 100);
    function stopProc() { if (procTimer) { clearInterval(procTimer); procTimer = null; } }
    function computeStats(done) {
      var elapsed = nowMs() - t0;
      var comp = (usage && usage.completion_tokens) ? usage.completion_tokens : (estTok(asst.content) + estTok(asst.reasoning));
      var prompt = (usage && usage.prompt_tokens) ? usage.prompt_tokens : 0;
      var total = (usage && usage.total_tokens) ? usage.total_tokens : (prompt + comp);
      var tokps;
      if (timings && timings.predicted_per_second) tokps = timings.predicted_per_second;
      else { var genMs = tFirst ? (nowMs() - tFirst) : elapsed; tokps = genMs > 0 ? comp / (genMs / 1000) : 0; }
      asst.stats = { ttft: tFirst ? (tFirst - t0) : 0, elapsed: elapsed, prompt: prompt, completion: comp, total: total, tokps: tokps, ctx: ctxSize, live: !done, stopped: aborted };
    }
    function updateAsst() {
      var nodes = document.querySelectorAll('#chatlog .cmsg'); var last = nodes[nodes.length - 1];
      if (last) last.outerHTML = bubble(asst, current.messages.length - 1);
      scrollChat();
    }
    fetch('api.php?action=chat', { method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json', 'X-CSRF': CSRF }, body: JSON.stringify({ messages: current.messages.slice(0, -1) }), signal: abortCtl ? abortCtl.signal : undefined })
      .then(function (r) {
        if (!r.body || !r.body.getReader) { return r.text().then(function () { asst.content = '*[this browser does not support streaming]*'; updateAsst(); }); }
        var reader = r.body.getReader(), dec = new TextDecoder(), buf = '';
        function pump() {
          return reader.read().then(function (res) {
            if (res.done) return;
            buf += dec.decode(res.value, { stream: true });
            var idx;
            while ((idx = buf.indexOf('\n')) >= 0) {
              var line = buf.slice(0, idx).trim(); buf = buf.slice(idx + 1);
              if (line.indexOf('data:') !== 0) continue;
              var p = line.slice(5).trim(); if (p === '' || p === '[DONE]') continue;
              var o; try { o = JSON.parse(p); } catch (e) { continue; }
              if (o.usage) usage = o.usage;
              if (o.timings) timings = o.timings; else if (o.usage && o.usage.timings) timings = o.usage.timings;
              var d = o.choices && o.choices[0] && o.choices[0].delta;
              if (d) {
                if ((d.reasoning_content || d.content) && !tFirst) { tFirst = nowMs(); stopProc(); }
                if (d.reasoning_content) { asst.reasoning = (asst.reasoning || '') + d.reasoning_content; asst._rc = true; }
                if (d.content) { asst._raw = (asst._raw || '') + d.content; }
                if (d.reasoning_content || d.content) {
                  if (asst._rc) { asst.content = asst._raw || ''; }            // server already split reasoning out
                  else { var sp = splitThink(asst._raw || ''); asst.reasoning = sp.reasoning; asst.content = sp.answer; }
                  computeStats(false); updateAsst();
                }
              }
              if (o.error) { asst.content += '\n\n*[error: ' + (o.error.message || 'server error') + ']*'; updateAsst(); }
            }
            return pump();
          });
        }
        return pump();
      })
      .catch(function (err) { if (err && (err.name === 'AbortError' || err.code === 20)) { aborted = true; } else { asst.content += '\n\n*[connection error — the coding agent server is not reachable]*'; } updateAsst(); })
      .then(function () { stopProc(); streaming = false; abortCtl = null; setStreamUI(false); setAgentState('waiting'); if (!asst.content && !aborted) { asst.content = asst.reasoning ? '' : '*[no response]*'; } delete asst._raw; delete asst._rc; computeStats(true); updateAsst(); touchSession(); saveSessions(); var i = el('chatinput'); if (i) i.focus(); });
  }

  // ---- auth (chat-user login / signup) -------------------------------------
  function showAuth(html) { el('shell').hidden = true; el('nav').hidden = true; el('auth').hidden = false; el('authbody').innerHTML = html; }
  function authErr(m) { var e = el('authmsg'); if (e) e.innerHTML = '<div class="flash err">' + esc(m) + '</div>'; }
  function authErrList(list) { var e = el('authmsg'); if (e) e.innerHTML = list.map(function (x) { return '<div class="flash err">' + esc(x) + '</div>'; }).join(''); }
  function showAuthUser(mode) {
    mode = mode || 'login';
    var tabs = '<div class="authtabs">' +
      '<button type="button" class="authtab' + (mode === 'login' ? ' on' : '') + '" data-authtab="login">Sign in</button>' +
      '<button type="button" class="authtab' + (mode === 'signup' ? ' on' : '') + '" data-authtab="signup">Sign up</button></div>';
    var body = mode === 'login'
      ? '<form id="loginform" method="post" action="api.php?action=user_login"><label>User ID <input name="id" autocomplete="username" autofocus required></label>' +
        '<label>Password <input type="password" name="password" autocomplete="current-password" required></label>' +
        '<button type="submit">Sign in</button></form>'
      : '<form id="signupform" method="post" action="api.php?action=signup"><label>User ID <input name="id" placeholder="e.g. apollo" autocomplete="username" required></label>' +
        '<label>Display name <input name="name" placeholder="Apollo" required></label>' +
        '<label>Password <input type="password" name="password" autocomplete="new-password" required></label>' +
        '<label>Confirm password <input type="password" name="password2" autocomplete="new-password" required></label>' +
        '<button type="submit">Create account</button>' +
        '<p class="hint">New accounts must be approved by an administrator before the first sign-in.</p></form>';
    showAuth('<div class="authhead">&#129302; <strong>AI Coding Agent</strong></div>' + tabs + '<div id="authmsg"></div>' + body);
  }
  function formData(f) { var o = {}, e = f.elements; for (var i = 0; i < e.length; i++) if (e[i].name) o[e[i].name] = e[i].value; return o; }

  function enterApp(user) {
    if (user) { userId = user.id; userName = user.name; isAdmin = (user.role === 'admin' || user.admin === true); }
    el('auth').hidden = true; el('shell').hidden = false; el('nav').hidden = false;
    applySidebar();
    var who = el('whoami'); if (who) who.textContent = userName || '';
    var al = el('adminlink'); if (al) al.hidden = !isAdmin;   // Admin link only for administrators
    if (!appWired) {
      appWired = true;
      el('logoutlink').addEventListener('click', function (e) {
        e.preventDefault();
        if (saveTimer) { clearTimeout(saveTimer); serverSaveNow(); }
        api('user_logout', { body: {} }).then(afterLogout).catch(afterLogout);
      });
      var inp = el('chatinput'); var comp = inp.parentNode;
      inp.addEventListener('keydown', function (e) { if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendChat(); } });
      inp.addEventListener('input', function () { this.style.height = 'auto'; this.style.height = Math.min(this.scrollHeight, 200) + 'px'; });
      inp.addEventListener('focus', function () { if (comp) comp.className = 'composer focus'; });
      inp.addEventListener('blur', function () { if (comp) comp.className = 'composer'; });
      var srch = el('chatsearch'); if (srch) srch.addEventListener('input', function () { searchQuery = (this.value || '').toLowerCase(); renderSessions(); });
      var clog = el('chatlog'); if (clog) clog.addEventListener('scroll', updateScrollDown);
    }
    api('props').then(function (p) { ctxSize = (p && p.n_ctx) ? p.n_ctx : 0; }).catch(function () {});
    loadSessions();                                   // local cache first (instant paint)
    current = sessions[0] || null;
    if (current) { renderSessions(); paintChat(); }
    api('chats_load').then(function (r) {
      var server = (r && r.sessions instanceof Array) ? r.sessions : [];
      if (server.length) { sessions = server; }
      else if (sessions.length) { serverSaveNow(); }  // first login: migrate local cache up
      try { localStorage.setItem(skey(), JSON.stringify(sessions)); } catch (e) {}
      current = sessions[0] || null;
      if (!current) newSession(); else { renderSessions(); paintChat(); }
    }).catch(function () { if (!current) newSession(); });
    setAgentState(streaming ? 'busy' : 'waiting');   // mascot: idle "waiting for a prompt" state
    el('chatinput').focus();
  }
  function afterLogout() { userId = null; userName = null; isAdmin = false; sessions = []; current = null; searchQuery = ''; var s = el('chatsearch'); if (s) s.value = ''; setAgentState('off'); boot(); }
  function boot() {
    api('session').then(function (s) {
      CSRF = s.csrf;
      if (s.user) enterApp(s.user);
      else if (s.authed) enterApp({ id: '__admin__', name: 'Admin', admin: true });   // admin may use the chat too
      else showAuthUser('login');
    }).catch(function (e) { showAuth('<div class="flash err">Cannot reach API: ' + esc(e) + '</div>'); });
  }

  // ---- events --------------------------------------------------------------
  document.addEventListener('click', function (e) {
    var t = e.target;
    if (t.id === 'sidebtn' || (t.closest && t.closest('#sidebtn'))) { setSidebar(!document.body.classList.contains('sb-collapsed')); return; }
    var authTab = t.closest ? t.closest('[data-authtab]') : null;
    if (authTab) { e.preventDefault(); showAuthUser(authTab.getAttribute('data-authtab')); return; }
    if (t.id === 'newchat') { newSession(); return; }
    if (t.id === 'attachbtn') { el('fileinput').click(); return; }
    if (t.id === 'scrolldown') { var c = el('chatlog'); if (c) c.scrollTop = c.scrollHeight; updateScrollDown(); return; }
    if (t.id === 'pwlink') { e.preventDefault(); openChangePassword(); return; }
    var thinkHead = t.closest ? t.closest('[data-act="toggle-think"]') : null;
    if (thinkHead) { var tb = thinkHead.closest('.thinkbox'); if (tb) tb.className = (tb.className.indexOf('open') >= 0) ? 'thinkbox' : 'thinkbox open'; return; }
    var copyBtn = t.closest ? t.closest('[data-copy]') : null;
    if (copyBtn) { var cb = copyBtn.closest('.codeblock'); var code = cb && cb.querySelector('pre code'); copyText(code ? (code.textContent || code.innerText || '') : '', copyBtn); return; }
    var actBtn = t.closest ? t.closest('.cmsg-actions [data-act]') : null;
    if (actBtn) {
      var idx = parseInt(actBtn.getAttribute('data-i'), 10);
      var actName = actBtn.getAttribute('data-act');
      if (actName === 'copy-msg') { var mm = current && current.messages[idx]; if (mm) copyText((mm.display != null ? mm.display : mm.content) || '', actBtn); return; }
      if (actName === 'edit-msg') { editMessage(idx); return; }
    }
    var att = t.getAttribute && t.getAttribute('data-att'); if (att !== null && att !== undefined && att !== '') { /* fallthrough handled below */ }
    var del = t.closest ? t.closest('[data-del]') : null;
    if (del) { e.stopPropagation(); deleteSession(del.getAttribute('data-del')); return; }
    var pin = t.closest ? t.closest('[data-pin]') : null;
    if (pin) { e.stopPropagation(); togglePin(pin.getAttribute('data-pin')); return; }
    var ren = t.closest ? t.closest('[data-rename]') : null;
    if (ren) { e.stopPropagation(); startRename(ren.getAttribute('data-rename')); return; }
    if (t.className && ('' + t.className).indexOf('srename') >= 0) { return; }  // don't switch while editing the title
    if (t.getAttribute && t.getAttribute('data-att') !== null && t.getAttribute('data-att') !== undefined) {
      var i = parseInt(t.getAttribute('data-att'), 10); if (!isNaN(i)) { attachments.splice(i, 1); renderAttachments(); return; }
    }
    var li = t.closest ? t.closest('li[data-id]') : null;
    if (li) { selectSession(li.getAttribute('data-id')); }
  });
  document.addEventListener('change', function (e) { if (e.target.id === 'fileinput') { addFiles(e.target.files); e.target.value = ''; } });
  document.addEventListener('keydown', function (e) {
    var t = e.target;
    if (!t || !t.className || ('' + t.className).indexOf('srename') < 0) return;
    if (e.key === 'Enter') { e.preventDefault(); commitRename(t.getAttribute('data-id'), t.value); }
    else if (e.key === 'Escape') { e.preventDefault(); editingId = null; renderSessions(); }
  });
  document.addEventListener('focusout', function (e) {
    var t = e.target;
    if (t && t.className && ('' + t.className).indexOf('srename') >= 0) { commitRename(t.getAttribute('data-id'), t.value); }
  });
  document.addEventListener('submit', function (e) {
    var f = e.target;
    // NB: read the id via getAttribute — a form control named "id" (our login/signup
    // forms have one) clobbers the `form.id` property, returning the input element.
    var fid = f.getAttribute ? f.getAttribute('id') : f.id;
    if (fid === 'chatform') { e.preventDefault(); if (streaming) stopChat(); else sendChat(); }
    else if (fid === 'loginform') {
      e.preventDefault(); var d = formData(f);
      api('user_login', { body: { id: d.id, password: d.password } })
        .then(function (r) { if (r.csrf) CSRF = r.csrf; enterApp(r.user); })
        .catch(authErr);
    }
    else if (fid === 'signupform') {
      e.preventDefault(); var d2 = formData(f);
      if (d2.password !== d2.password2) { authErr('Passwords do not match.'); return; }
      api('signup', { body: { id: d2.id, name: d2.name, password: d2.password } })
        .then(function (r) {
          if (r.errors) { authErrList(r.errors); return; }
          showAuthUser('login');
          var m = el('authmsg'); if (m) m.innerHTML = '<div class="flash ok">Account created. An administrator must approve it before you can sign in.</div>';
        })
        .catch(authErr);
    }
  });

  boot();
})();
