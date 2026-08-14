    // ────────────────────────────────────────────────────────
    //  Dual-mode client: WebSocket (push) with HTTP polling fallback
    //  - Tries WebSocket on port 9002 for real-time push updates
    //  - Falls back to HTTP polling on same origin if WS unavailable
    // ────────────────────────────────────────────────────────

    const RECONNECT_MS = 3000;
    const POLL_MS = 2000;
    const WS_CONNECT_TIMEOUT_MS = 3000;

    let mode = 'init';   // 'init' | 'ws' | 'http'
    let ws = null;
    let callbackSeq = 0;
    let pendingCallbacks = {};
    let pollTimer = null;

    let editorAlertTimer = null;

    function showEditorAlert(msg) {
        const el = document.getElementById('editorAlert');
        if (!el) return;
        if (!msg) {
            el.textContent = '';
            el.style.display = 'none';
            return;
        }
        el.textContent = msg;
        el.style.display = 'block';
        if (editorAlertTimer) clearTimeout(editorAlertTimer);
        editorAlertTimer = setTimeout(() => {
            el.textContent = '';
            el.style.display = 'none';
        }, 10000);
    }

    // Used to queue initial loads so opening the portal doesn't fan out a burst of requests.
    let loadQueue = Promise.resolve();
    let didInitialIpLoad = false;
    let didInitialGitLoad = false;
    let didInitialWidgetLoad = false;
    let widgetReqSeq = 0;
    let currentWidgetRows = [];

    const queryParams = new URLSearchParams(location.search);
    const widgetAppProject = (queryParams.get('widget_project') || '').trim();
    const widgetAppCommand = (queryParams.get('widget_command') || '').trim();
    const isWidgetAppMode = !!(widgetAppProject && widgetAppCommand);

    if (isWidgetAppMode) {
        document.body.classList.add('widget-app-mode');
    }

    function enqueueLoad(fn) {
        loadQueue = loadQueue.then(fn).catch(() => { /* swallow */ });
        return loadQueue;
    }

    function wsUrl() {
        // Connect via same host:port — the HTTP server proxies to the WS server
        const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
        return `${proto}//${location.host}/ws`;
    }

    function apiUrl(endpoint) {
        return `${location.protocol}//${location.host}/_api/${endpoint}`;
    }

    // ────────────────────────────────────────────────────────
    //  IPC wire protocol helpers (for WebSocket mode)
    // ────────────────────────────────────────────────────────

    function makeCbId(seq) {
        return String(seq).padStart(5, '0');
    }

    function ipcRequest(command, payload, timeoutMs = 5000) {
        return new Promise((resolve, reject) => {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                return reject(new Error('not connected'));
            }
            const seq = ++callbackSeq;
            const cbId = makeCbId(seq);
            const prefix = `${command}:${cbId}`;
            const message = `${prefix} ${payload}`;

            const timer = setTimeout(() => {
                delete pendingCallbacks[prefix];
                reject(new Error('timeout'));
            }, timeoutMs);

            pendingCallbacks[prefix] = { resolve, reject, timer };
            ws.send(message);
        });
    }

    function handleMessage(raw) {
        const spaceIdx = raw.indexOf(' ');
        if (spaceIdx === -1) return;

        const fullCmd = raw.substring(0, spaceIdx);
        const body = raw.substring(spaceIdx + 1);

        if (pendingCallbacks[fullCmd]) {
            const cb = pendingCallbacks[fullCmd];
            clearTimeout(cb.timer);
            delete pendingCallbacks[fullCmd];
            cb.resolve(body);
            return;
        }

        if (fullCmd === 'push:update') {
            try {
                const data = JSON.parse(body);
                if (data.configs) renderProjects(data.configs);
                if (data.actions) renderActions(data.actions);
            } catch (e) {
                console.warn('Push parse error:', e);
            }
        }
    }

    // ────────────────────────────────────────────────────────
    //  WebSocket connection (preferred)
    // ────────────────────────────────────────────────────────

    function connectWs() {
        if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

        const sock = new WebSocket(wsUrl());
        const connectTimer = setTimeout(() => {
            // If still connecting after timeout, give up and fall back
            if (sock.readyState === WebSocket.CONNECTING) {
                sock.close();
            }
        }, WS_CONNECT_TIMEOUT_MS);

        sock.onopen = () => {
            clearTimeout(connectTimer);
            ws = sock;
            mode = 'ws';
            stopPolling();
            setStatus(true, 'ws');
            enqueueLoad(async () => { await refreshViaWs(); });
            enqueueLoad(async () => { await fetchWidgets(); didInitialWidgetLoad = true; });
            enqueueLoad(async () => { await fetchGitStatus(); didInitialGitLoad = true; });
            enqueueLoad(async () => { await fetchIpStates(); didInitialIpLoad = true; });
        };

        sock.onmessage = (event) => handleMessage(event.data);

        sock.onclose = () => {
            clearTimeout(connectTimer);
            ws = null;
            for (const key of Object.keys(pendingCallbacks)) {
                clearTimeout(pendingCallbacks[key].timer);
                pendingCallbacks[key].reject(new Error('closed'));
            }
            pendingCallbacks = {};

            if (mode === 'ws') {
                // Was connected, lost connection — try WS again soon
                setStatus(false);
                setTimeout(connectWs, RECONNECT_MS);
            } else {
                // Never connected — fall back to HTTP polling
                startPolling();
            }
        };

        sock.onerror = () => { /* onclose fires after */ };
    }

    async function refreshViaWs() {
        try {
            const [configsRaw, actionsRaw] = await Promise.all([
                ipcRequest('list-registered-configs', '*'),
                ipcRequest('recent-actions', '20'),
            ]);
            renderProjects(JSON.parse(configsRaw));
            renderActions(JSON.parse(actionsRaw));
        } catch (e) {
            console.warn('WS fetch error:', e);
        }
    }

    // ────────────────────────────────────────────────────────
    //  HTTP polling fallback
    // ────────────────────────────────────────────────────────

    async function pollHttp() {
        try {
            const [projResp, actResp] = await Promise.all([
                fetch(apiUrl('list-configs')),
                fetch(apiUrl('recent-actions')),
            ]);
            if (!projResp.ok || !actResp.ok) throw new Error('bad status');
            renderProjects(await projResp.json());
            renderActions(await actResp.json());
            if (mode !== 'ws' && mode !== 'http') {
                enqueueLoad(async () => { await fetchWidgets(); didInitialWidgetLoad = true; });
                enqueueLoad(async () => { await fetchGitStatus(); didInitialGitLoad = true; });
                enqueueLoad(async () => { await fetchIpStates(); didInitialIpLoad = true; });
            }
            if (mode !== 'ws') { mode = 'http'; setStatus(true, 'http'); }
        } catch (e) {
            if (mode === 'http') setStatus(false);
        }
        // Try upgrading to WebSocket periodically
        if (mode === 'http' && !ws) connectWs();
        pollTimer = setTimeout(pollHttp, POLL_MS);
    }

    function startPolling() {
        if (pollTimer) return;
        mode = 'http';
        pollHttp();
    }

    function stopPolling() {
        if (pollTimer) { clearTimeout(pollTimer); pollTimer = null; }
    }

    // ────────────────────────────────────────────────────────
    //  Git Status
    // ────────────────────────────────────────────────────────

    async function fetchGitStatus() {
        try {
            const resp = await fetch(apiUrl('git-status'));
            if (!resp.ok) throw new Error('bad status');
            const repos = await resp.json();
            renderGitStatus(repos);
        } catch (e) {
            console.warn('git-status fetch error:', e);
        }
    }

    // ────────────────────────────────────────────────────────
    //  IP States
    // ────────────────────────────────────────────────────────

    function countPortLines(text) {
        // Matches lines like: "  192.168.50.87:8780"
        const m = (text || '').match(/^\s*\d+\.\d+\.\d+\.\d+:\d+\s*$/gm);
        return m ? m.length : 0;
    }

    async function fetchIpStates() {
        const el = document.getElementById('ipStateBody');
        try {
            let text = '';
            if (mode === 'ws' && ws && ws.readyState === WebSocket.OPEN) {
                text = await ipcRequest('wsl-forwarded-ports-list', '*', 10000);
            } else {
                const resp = await fetch(apiUrl('wsl-forwarded-ports-list'));
                if (!resp.ok) throw new Error('bad status');
                // Server returns JSON {text: "..."}
                const j = await resp.json();
                text = j && j.text ? j.text : '';
            }

            document.getElementById('ipStateCount').textContent = String(countPortLines(text));
            if (!text || !text.trim()) {
                el.innerHTML = '<div class="empty-state">No IP state data</div>';
                return;
            }
            el.innerHTML = `<div class="mono-pre">${esc(text)}</div>`;
        } catch (e) {
            console.warn('ip-states fetch error:', e);
            el.innerHTML = '<div class="empty-state">Failed to load IP states</div>';
        }
    }

    async function runWidgetCommand(project, command, payload) {
        if (mode === 'ws' && ws && ws.readyState === WebSocket.OPEN) {
            const raw = await ipcRequest('widget-command', JSON.stringify({
                type: 'widget-command',
                project,
                command,
                payload: payload === undefined ? null : payload,
            }), 15000);
            return JSON.parse(raw);
        }

        const url = apiUrl('widget-command') +
            '?project=' + encodeURIComponent(project) +
            '&command=' + encodeURIComponent(command) +
            '&payload=' + encodeURIComponent(JSON.stringify(payload === undefined ? null : payload));
        const resp = await fetch(url);
        if (!resp.ok) throw new Error('widget-command request failed');
        return resp.json();
    }

    function widgetBridgeScript(project) {
        return `<script>(function(){
            const __project = ${JSON.stringify(project)};
            let __seq = 0;
            const __pending = new Map();

            function post(req){ parent.postMessage(req, '*'); }

            window.webdash = {
                runCommand: function(name, payload, callback) {
                    if (typeof payload === 'function') {
                        callback = payload;
                        payload = undefined;
                    }

                    const requestId = 'wreq-' + (++__seq);
                    const req = {
                        type: 'webdash-widget-command',
                        requestId,
                        project: __project,
                        command: String(name || ''),
                        payload: payload === undefined ? null : payload,
                    };

                    const promise = new Promise(function(resolve, reject) {
                        __pending.set(requestId, { resolve, reject, callback });
                        post(req);
                    });

                    if (typeof callback === 'function') {
                        promise.then(function(result) {
                            callback(result);
                        }).catch(function(err) {
                            callback(null, err && err.message ? err.message : String(err));
                        });
                    }

                    return promise;
                }
            };

            window.addEventListener('message', function(ev) {
                const data = ev && ev.data;
                if (!data || data.type !== 'webdash-widget-response') return;
                const entry = __pending.get(data.requestId);
                if (!entry) return;
                __pending.delete(data.requestId);
                if (data.ok) {
                    entry.resolve(data.data);
                } else {
                    entry.reject(new Error(data.error || 'widget command failed'));
                }
            });
        })();<\/script>`;
    }

    function buildWidgetSrcdoc(project, html) {
        return widgetBridgeScript(project) + (html || '');
    }

    function normalizeWidgetHeight(rawHeight) {
        if (rawHeight == null) return null;

        if (typeof rawHeight === 'number' && Number.isFinite(rawHeight)) {
            const px = Math.max(1, Math.floor(rawHeight));
            return `${px}px`;
        }

        const text = String(rawHeight).trim();
        if (!text) return null;

        if (/^\d+$/.test(text)) {
            return `${text}px`;
        }

        if (/^\d+(?:\.\d+)?(px|rem|em|vh|vw|%)$/.test(text)) {
            return text;
        }

        return null;
    }

    function mountWidgetFrames(rows) {
        for (let idx = 0; idx < rows.length; idx++) {
            const row = rows[idx];
            if (!row || !row.widget || !row.widget.ok) continue;

            const frame = document.getElementById(`widget-frame-${idx}`);
            if (!frame) continue;

            const runtimeHeight = normalizeWidgetHeight(row.widget.height);
            if (runtimeHeight) {
                frame.style.height = runtimeHeight;
            }

            try {
                const doc = frame.contentDocument || (frame.contentWindow && frame.contentWindow.document);
                if (!doc) continue;
                const html = buildWidgetSrcdoc(row.project.project, row.widget.html || '');
                doc.open();
                doc.write(html);
                doc.close();
            } catch (e) {
                const card = frame.closest('.widget-card');
                if (card) {
                    const error = document.createElement('div');
                    error.className = 'widget-error';
                    error.textContent = `Failed to render widget iframe: ${e && e.message ? e.message : e}`;
                    frame.replaceWith(error);
                }
            }
        }
    }

    function buildWidgetPopoutHtml(title) {
        const safeTitle = String(title || 'Widget');
        return `<!doctype html>
<html>
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>${esc(safeTitle)} · WebDash</title>
<style>
    :root {
        --bg: #0e1117;
        --surface: #161b22;
        --border: #30363d;
        --text: #c9d1d9;
        --accent: #58a6ff;
    }
    * { box-sizing: border-box; }
    html, body { width: 100%; height: 100%; margin: 0; padding: 0; background: var(--bg); color: var(--text); }
    body { display: flex; flex-direction: column; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif; }
    .bar {
        height: 38px;
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 10px;
        padding: 0 10px;
        background: var(--surface);
        border-bottom: 1px solid var(--border);
        flex: 0 0 auto;
    }
    .title {
        font-size: 12px;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
        color: var(--text);
    }
    .close-btn {
        border: 1px solid var(--border);
        background: rgba(88, 166, 255, 0.15);
        color: var(--accent);
        border-radius: 4px;
        font-size: 11px;
        padding: 3px 9px;
        cursor: pointer;
    }
    iframe {
        width: 100%;
        height: calc(100% - 38px);
        border: 0;
        background: #000;
        flex: 1 1 auto;
    }
</style>
</head>
<body>
<div class="bar">
    <div class="title">${esc(safeTitle)}</div>
    <button class="close-btn" onclick="window.close()">Close</button>
</div>
<iframe id="widgetPopFrame" sandbox="allow-scripts allow-same-origin" title="${esc(safeTitle)}"></iframe>
<script>
    const __pendingWidgetCalls = new Map();

    window.addEventListener('message', function(ev) {
        const data = ev && ev.data;
        if (!data) return;

        if (data.type === 'webdash-widget-command') {
            const requestId = data.requestId;
            if (!requestId) return;

            __pendingWidgetCalls.set(requestId, ev.source);

            if (!window.opener || window.opener.closed) {
                const src = __pendingWidgetCalls.get(requestId);
                __pendingWidgetCalls.delete(requestId);
                if (src) {
                    src.postMessage({
                        type: 'webdash-widget-response',
                        requestId,
                        ok: false,
                        error: 'Portal window is unavailable'
                    }, '*');
                }
                return;
            }

            window.opener.postMessage({
                type: 'webdash-popup-widget-command',
                requestId,
                project: data.project,
                command: data.command,
                payload: data.payload
            }, '*');
            return;
        }

        if (data.type === 'webdash-popup-widget-response') {
            const requestId = data.requestId;
            const src = __pendingWidgetCalls.get(requestId);
            if (!src) return;
            __pendingWidgetCalls.delete(requestId);

            src.postMessage({
                type: 'webdash-widget-response',
                requestId,
                ok: !!data.ok,
                data: data.data,
                error: data.error || null
            }, '*');
        }
    });
<\/script>
</body>
</html>`;
    }

    async function tryOpenNativeWidgetWindow(row) {
        if (!row || !row.widget || !row.widget.ok) return false;

        const payload = {
            project: String(row.project && row.project.project ? row.project.project : ''),
            command: String(row.widget && row.widget.name ? row.widget.name : ''),
            portalPath: String(location.pathname || ''),
            width: 920,
            height: 680,
        };

        if (!payload.project || !payload.command) return false;

        if (ws && ws.readyState === WebSocket.OPEN) {
            try {
                const raw = await ipcRequest('open-widget-window', JSON.stringify(payload), 5000);
                const resp = JSON.parse(raw || '{}');
                if (resp && resp.ok) {
                    return true;
                }
                if (resp && resp.error) {
                    showEditorAlert(`Native pop-out failed: ${resp.error}`);
                }
            } catch (e) {
                // Fall through to HTTP fallback.
            }
        }

        try {
            const qs = new URLSearchParams({
                project: payload.project,
                command: payload.command,
                portal_path: payload.portalPath || '',
                width: String(payload.width),
                height: String(payload.height),
            }).toString();
            const respHttp = await fetch(apiUrl(`open-widget-window?${qs}`));
            if (!respHttp.ok) {
                showEditorAlert(`Native pop-out failed: HTTP ${respHttp.status}`);
                return false;
            }
            const resp = await respHttp.json();
            if (resp && resp.ok) {
                return true;
            }
            if (resp && resp.error) {
                showEditorAlert(`Native pop-out failed: ${resp.error}`);
            }
        } catch (e) {
            showEditorAlert(`Native pop-out unavailable: ${e && e.message ? e.message : e}`);
        }

        return false;
    }

    async function openWidgetPopout(index) {
        const row = currentWidgetRows[index];
        if (!row || !row.widget || !row.widget.ok) return;

        const nativeOpened = await tryOpenNativeWidgetWindow(row);
        if (nativeOpened) return;
        showEditorAlert('Native pop-out failed. Check server logs and Chrome path setup.');
    }

    function renderWidgets(projects) {
        const el = document.getElementById('widgetList');

        const norm = (v) => String(v == null ? '' : v).trim();

        let rows = [];
        for (const project of (projects || [])) {
            const widgets = project.widgets || [];
            for (const widget of widgets) {
                rows.push({
                    project,
                    widget,
                });
            }
        }

        if (isWidgetAppMode) {
            const wantedProject = norm(widgetAppProject);
            const wantedCommand = norm(widgetAppCommand);

            const exactRows = rows.filter((r) => norm(r.project && r.project.project ? r.project.project : '') === wantedProject
                && norm(r.widget && r.widget.name ? r.widget.name : '') === wantedCommand);

            if (exactRows.length > 0) {
                rows = exactRows;
            } else {
                const projectRows = rows.filter((r) => norm(r.project && r.project.project ? r.project.project : '') === wantedProject);
                rows = projectRows.length > 0 ? [projectRows[0]] : [];
            }
        }

        currentWidgetRows = rows;

        document.getElementById('widgetCount').textContent = String(rows.length);

        if (rows.length === 0) {
            el.innerHTML = '<div class="empty-state">No widget projects detected</div>';
            return;
        }

        el.classList.add('widget-list');
        el.innerHTML = rows.map((r, idx) => {
            const project = r.project;
            const widget = r.widget;
            const stripTags = (s) => String(s || '').replace(/<[^>]*>/g, '').trim();
            const baseTitle = stripTags(project.projectName || '') || project.project || project.projectDir || 'Widget';
            const widgetName = String(widget.name || 'widget');
            const title = `${baseTitle} • ${widgetName}`;

            if (!widget.ok) {
                return `<div class="widget-card">
                    <div class="widget-card-header">
                        <span class="widget-title">${esc(title)}</span>
                        <span class="widget-header-actions">
                            <span>${esc(project.project || '')}</span>
                        </span>
                    </div>
                    <div class="widget-error">${esc(widget.error || 'Failed to load widget')}</div>
                </div>`;
            }

            return `<div class="widget-card">
                <div class="widget-card-header">
                    <span class="widget-title">${esc(title)}</span>
                    <span class="widget-header-actions">
                        <span>${esc(project.project || '')}</span>
                        <button class="widget-pop-btn" type="button" onclick="openWidgetPopout(${idx})" title="Open widget in separate window">Pop-out</button>
                    </span>
                </div>
                <iframe class="widget-frame" sandbox="allow-scripts allow-same-origin" title="${esc(title)}" id="widget-frame-${idx}"${normalizeWidgetHeight(widget.height) ? ` style="height:${normalizeWidgetHeight(widget.height)}"` : ''}></iframe>
            </div>`;
        }).join('');

        setTimeout(() => mountWidgetFrames(rows), 0);
    }

    async function fetchWidgets() {
        const resp = await fetch(apiUrl('widgets'));
        if (!resp.ok) throw new Error('widget list fetch failed');
        const projects = await resp.json();
        renderWidgets(projects);
    }

    async function refreshWidgets(btn) {
        if (btn) btn.classList.add('spinning');
        try {
            await fetchWidgets();
        } catch (e) {
            console.warn('widget refresh error:', e);
        }
        if (btn) setTimeout(() => btn.classList.remove('spinning'), 400);
    }

    window.addEventListener('message', async (event) => {
        const data = event && event.data;
        if (!data || data.type !== 'webdash-widget-command') return;

        const requestId = data.requestId;
        const source = event.source;
        if (!source || !requestId) return;

        const reply = (payload) => {
            try {
                source.postMessage({
                    type: 'webdash-widget-response',
                    requestId,
                    ...payload,
                }, '*');
            } catch (_) { }
        };

        try {
            const result = await runWidgetCommand(data.project, data.command, data.payload);
            if (!result || !result.ok) {
                reply({ ok: false, error: (result && result.error) ? result.error : 'widget command failed' });
                return;
            }
            reply({ ok: true, data: result.data !== undefined ? result.data : result.content });
        } catch (e) {
            reply({ ok: false, error: (e && e.message) ? e.message : String(e) });
        }
    });

    window.addEventListener('message', async (event) => {
        const data = event && event.data;
        if (!data || data.type !== 'webdash-popup-widget-command') return;

        const requestId = data.requestId;
        const source = event.source;
        if (!source || !requestId) return;

        const reply = (payload) => {
            try {
                source.postMessage({
                    type: 'webdash-popup-widget-response',
                    requestId,
                    ...payload,
                }, '*');
            } catch (_) { }
        };

        try {
            const result = await runWidgetCommand(data.project, data.command, data.payload);
            if (!result || !result.ok) {
                reply({ ok: false, error: (result && result.error) ? result.error : 'widget command failed' });
                return;
            }
            reply({ ok: true, data: result.data !== undefined ? result.data : result.content });
        } catch (e) {
            reply({ ok: false, error: (e && e.message) ? e.message : String(e) });
        }
    });

    async function refreshIpStates(btn) {
        if (btn) { btn.classList.add('spinning'); }
        try {
            await fetchIpStates();
        } catch (e) {
            console.warn('ip refresh error:', e);
        }
        if (btn) { setTimeout(() => btn.classList.remove('spinning'), 400); }
    }

    function renderGitStatus(repos) {
        const el = document.getElementById('gitList');
        document.getElementById('gitCount').textContent = repos.length;

        if (repos.length === 0) {
            el.innerHTML = '<div class="empty-state">No git repositories found</div>';
            return;
        }

        el.innerHTML = repos.map(r => {
            const isClean = r.changes === 0 && r.ahead <= 0 && r.behind <= 0;
            const dotCls = r.changes > 0 ? 'dirty' : (r.ahead > 0 ? 'ahead' : 'clean');

            let badges = '';
            if (isClean) {
                badges += '<span class="git-badge clean">✓ clean</span>';
            } else {
                if (r.changes > 0)
                    badges += `<span class="git-badge changes">${r.changes} change${r.changes !== 1 ? 's' : ''}</span>`;
                if (r.ahead > 0)
                    badges += `<span class="git-badge ahead-badge">↑ ${r.ahead}</span>`;
                if (r.behind > 0)
                    badges += `<span class="git-badge behind-badge">↓ ${r.behind}</span>`;
            }

            return `<div class="git-item">
                <span class="git-dot ${dotCls}"></span>
                <span class="git-name" title="${esc(r.path)}">${esc(r.name)}</span>
                <span class="git-branch">${esc(r.branch)}</span>
                <div class="git-badges">${badges}</div>
            </div>`;
        }).join('');
    }

    async function refreshAll(btn) {
        if (btn) { btn.classList.add('spinning'); }
        try {
            if (mode === 'ws' && ws && ws.readyState === WebSocket.OPEN) {
                await refreshViaWs();
            } else {
                const [projResp, actResp] = await Promise.all([
                    fetch(apiUrl('list-configs')),
                    fetch(apiUrl('recent-actions')),
                ]);
                if (projResp.ok) renderProjects(await projResp.json());
                if (actResp.ok) renderActions(await actResp.json());
            }
            await fetchWidgets();
            await fetchGitStatus();
            await fetchIpStates();
        } catch (e) {
            console.warn('refresh error:', e);
        }
        if (btn) { setTimeout(() => btn.classList.remove('spinning'), 400); }
    }

    async function refreshGit(btn) {
        if (btn) { btn.classList.add('spinning'); }
        try {
            await fetchGitStatus();
        } catch (e) {
            console.warn('git refresh error:', e);
        }
        if (btn) { setTimeout(() => btn.classList.remove('spinning'), 400); }
    }

    // ────────────────────────────────────────────────────────
    //  Rendering
    // ────────────────────────────────────────────────────────

    function escJs(s) {
        return String(s)
            .replace(/\\/g, '\\\\')
            .replace(/'/g, "\\'")
            .replace(/\r/g, '')
            .replace(/\n/g, '\\n');
    }

    // Allow limited inline HTML for project names (from webdash.config.jsonc "project-name").
    // Security: strip all attributes and disallow non-inline tags to avoid XSS.
    function sanitizeInlineHtml(input) {
        const s = String(input ?? '');
        if (!s) return '';

        const parser = new DOMParser();
        const doc = parser.parseFromString('<div id="r"></div>', 'text/html');
        const root = doc.getElementById('r');
        root.innerHTML = s;

        const allowed = new Set(['B', 'I', 'EM', 'STRONG', 'CODE', 'SMALL', 'SUP', 'SUB', 'BR', 'SPAN']);

        const normalizeCssColor = (raw) => {
            const v = String(raw ?? '').trim();
            if (!v) return '';
            const vl = v.toLowerCase();

            if (/^#[0-9a-f]{3}$/.test(vl) || /^#[0-9a-f]{6}$/.test(vl) || /^#[0-9a-f]{8}$/.test(vl)) return v;
            if (/^(inherit|initial|unset|currentcolor)$/.test(vl)) return v;
            if (/^var\(--[a-z0-9_-]+\)$/.test(vl)) return v;
            if (/^[a-z]+$/.test(vl)) return v; // named colors

            const parseByte = (x) => {
                if (!/^\d{1,3}$/.test(x)) return null;
                const n = Number(x);
                if (!Number.isFinite(n) || n < 0 || n > 255) return null;
                return n;
            };

            const parseAlpha = (x) => {
                const t = String(x).trim();
                if (!/^(0|1|0?\.\d+)$/.test(t)) return null;
                const n = Number(t);
                if (!Number.isFinite(n) || n < 0 || n > 1) return null;
                return n;
            };

            // rgb()/rgba()
            {
                const m = vl.match(/^rgba?\(\s*([^\)]+)\s*\)$/);
                if (m) {
                    const parts = m[1].split(',').map(p => p.trim());
                    if (parts.length === 3 || parts.length === 4) {
                        const r = parseByte(parts[0]);
                        const g = parseByte(parts[1]);
                        const b = parseByte(parts[2]);
                        if (r !== null && g !== null && b !== null) {
                            if (parts.length === 3) return `rgb(${r}, ${g}, ${b})`;
                            const a = parseAlpha(parts[3]);
                            if (a !== null) return `rgba(${r}, ${g}, ${b}, ${a})`;
                        }
                    }
                }
            }

            // hsl()/hsla()
            {
                const m = vl.match(/^hsla?\(\s*([^\)]+)\s*\)$/);
                if (m) {
                    const parts = m[1].split(',').map(p => p.trim());
                    if (parts.length === 3 || parts.length === 4) {
                        if (!/^\d{1,3}$/.test(parts[0])) return '';
                        const h = Number(parts[0]);
                        const sPct = parts[1];
                        const lPct = parts[2];
                        if (!/^\d{1,3}%$/.test(sPct) || !/^\d{1,3}%$/.test(lPct)) return '';
                        const sNum = Number(sPct.replace('%', ''));
                        const lNum = Number(lPct.replace('%', ''));
                        if (!(Number.isFinite(h) && h >= 0 && h <= 360 && sNum >= 0 && sNum <= 100 && lNum >= 0 && lNum <= 100)) return '';
                        if (parts.length === 3) return `hsl(${h}, ${sNum}%, ${lNum}%)`;
                        const a = parseAlpha(parts[3]);
                        if (a !== null) return `hsla(${h}, ${sNum}%, ${lNum}%, ${a})`;
                    }
                }
            }

            return '';
        };

        const walk = (node) => {
            for (const child of Array.from(node.childNodes)) {
                if (child.nodeType === Node.ELEMENT_NODE) {
                    walk(child);

                    if (!allowed.has(child.tagName)) {
                        // Unwrap disallowed elements but keep their (sanitized) children.
                        while (child.firstChild) node.insertBefore(child.firstChild, child);
                        node.removeChild(child);
                        continue;
                    }

                    // Allow only style="color: ..."; strip everything else.
                    const rawStyle = child.getAttribute('style');
                    for (const attr of Array.from(child.attributes)) {
                        child.removeAttribute(attr.name);
                    }
                    if (rawStyle) {
                        const m = String(rawStyle).match(/(?:^|;)\s*color\s*:\s*([^;]+)\s*(?:;|$)/i);
                        const color = m ? normalizeCssColor(m[1]) : '';
                        if (color) child.setAttribute('style', `color: ${color};`);
                    }
                } else if (child.nodeType === Node.TEXT_NODE) {
                    // ok
                } else {
                    // comments, document fragments, etc.
                    node.removeChild(child);
                }
            }
        };

        walk(root);
        return root.innerHTML;
    }

    function renderProjects(projects) {
        const el = document.getElementById('projectList');
        document.getElementById('projectCount').textContent = projects.length;

        if (projects.length === 0) {
            el.innerHTML = '<div class="empty-state">No registered projects</div>';
            return;
        }

        // Sort by priority (ascending, undefined = Infinity), then lexicographically by display name.
        const sorted = projects.slice().sort((a, b) => {
            const pa = (typeof a === 'object' && a.priority !== undefined) ? a.priority : Infinity;
            const pb = (typeof b === 'object' && b.priority !== undefined) ? b.priority : Infinity;
            if (pa !== pb) return pa - pb;

            const nameA = ((typeof a === 'object' && a.projectName) ? stripHtml(a.projectName) : folderNameFromPath(typeof a === 'string' ? a : a.path)).toLowerCase();
            const nameB = ((typeof b === 'object' && b.projectName) ? stripHtml(b.projectName) : folderNameFromPath(typeof b === 'string' ? b : b.path)).toLowerCase();
            return nameA.localeCompare(nameB);
        });

        el.innerHTML = sorted.map(p => {
            const fullPath = typeof p === 'string' ? p : p.path;
            const relPath = (typeof p === 'object' && p.rel) ? p.rel : '';
            // Show just the parent directory name
            const parts = fullPath.replace(/\/webdash\.config\.json$/, '').split('/');
            const folderName = parts[parts.length - 1] || fullPath;
            const projName = (typeof p === 'object' && p.projectName) ? String(p.projectName) : '';
            const displayNameHtml = projName ? sanitizeInlineHtml(projName) : esc(folderName);
            const user = (typeof p === 'object' && p.uid !== undefined)
                ? `uid ${p.uid}` : '';

            const terminalBtns = (window._editorTerminals || []).map(t =>
                `<button class="terminal-btn" title="Open ${esc(t)} terminal" onclick="event.stopPropagation(); openTerminal('${escJs(fullPath)}', '${escJs(t)}')">${terminalIcon(t)}</button>`
            ).join('');

            return `<div class="project-item" onclick="projectClicked('${escJs(fullPath)}')">
                <span class="project-icon"></span>
                <span class="project-path" title="${esc(fullPath)}">${displayNameHtml}</span>
                ${relPath ? `<span class="project-relpath" title="${esc(relPath)}">${esc(relPath)}</span>` : ''}
                ${user ? `<span class="project-user">${esc(user)}</span>` : ''}
                ${terminalBtns ? `<span class="project-terminals">${terminalBtns}</span>` : ''}
            </div>`;
        }).join('');
    }

    // ── Terminal icons & launch ─────────────────────────────

    const _terminalIcons = {
        wsl: `<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"/></svg>`,
        powershell: `<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path d="M21.83 4H2.17C1.52 4 1 4.52 1 5.17v13.66c0 .65.52 1.17 1.17 1.17h19.66c.65 0 1.17-.52 1.17-1.17V5.17C23 4.52 22.48 4 21.83 4zM5.56 16.2l-.88-.76 5.2-4.02-5.2-3.55.84-.8 6.36 4.33-6.32 4.8zm8.94.3h-3.5v-1.1h3.5v1.1z"/></svg>`,
    };

    function terminalIcon(type) {
        return _terminalIcons[type.toLowerCase()] || `<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg"><path d="M20 4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V6c0-1.1-.9-2-2-2zm0 14H4V8h16v12zM6 10l4 3-4 3v-6zm5 5h5v2h-5v-2z"/></svg>`;
    }

    async function openTerminal(configPath, type) {
        try {
            const resp = await fetch(apiUrl('open-terminal') + '?config=' + encodeURIComponent(configPath) + '&type=' + encodeURIComponent(type));
            const j = await resp.json();
            if (!j.ok && j.error) {
                console.warn('Terminal launch failed:', j.error);
            }
        } catch (e) {
            console.error('Terminal launch error:', e);
        }
    }

    async function fetchEditorConfig() {
        try {
            const resp = await fetch(apiUrl('editor-config'));
            if (resp.ok) {
                const data = await resp.json();
                window._editorTerminals = data.terminals || [];
            }
        } catch (e) {
            console.warn('Failed to fetch editor config:', e);
        }
    }

    // ────────────────────────────────────────────────────────
    //  Project click behavior
    //  - If server has $.code.editor => open project directory
    //  - Else => edit webdash.config.jsonc in a modal
    // ────────────────────────────────────────────────────────

    let modalConfigPath = '';

    function openConfigModal(path, content, warningMsg = '') {
        modalConfigPath = path;
        document.getElementById('configModalPath').textContent = path;
        document.getElementById('configModalText').value = content || '';
        const st = document.getElementById('configModalStatus');
        st.classList.remove('error');
        if (warningMsg) {
            st.textContent = warningMsg;
            st.classList.add('error');
        } else {
            st.textContent = ' ';
        }
        document.getElementById('configModalSave').disabled = false;
        document.getElementById('configModal').classList.add('open');
    }

    function closeConfigModal() {
        modalConfigPath = '';
        document.getElementById('configModal').classList.remove('open');
    }

    async function projectClicked(configPath) {
        try {
            const resp = await fetch(apiUrl('open-project') + '?config=' + encodeURIComponent(configPath));
            const j = await resp.json();
            const editorMsg = (j && (j.warning || j.error)) ? String(j.warning || j.error) : '';
            if (!j.ok) {
                // Fall back to portal editing so clicks always do something useful.
                if (j.error) showEditorAlert(String(j.error));
            } else if (j.action === 'opened') {
                showEditorAlert('');
                return;
            } else if (j.warning) {
                showEditorAlert(String(j.warning));
            }

            const r2 = await fetch(apiUrl('read-config') + '?config=' + encodeURIComponent(configPath));
            const j2 = await r2.json();
            if (!j2.ok) {
                if (j2.error) showEditorAlert(String(j2.error));
                return;
            }
            openConfigModal(configPath, j2.content, editorMsg);
        } catch (e) {
            showEditorAlert('Project click failed');
        }
    }

    function ensureWsConnected(timeoutMs = 3000) {
        return new Promise((resolve, reject) => {
            if (ws && ws.readyState === WebSocket.OPEN) return resolve();

            connectWs();
            const start = Date.now();
            const timer = setInterval(() => {
                if (ws && ws.readyState === WebSocket.OPEN) {
                    clearInterval(timer);
                    resolve();
                    return;
                }
                if (Date.now() - start > timeoutMs) {
                    clearInterval(timer);
                    reject(new Error('WebSocket unavailable'));
                }
            }, 50);
        });
    }

    async function saveConfigModal() {
        const statusEl = document.getElementById('configModalStatus');
        const saveBtn = document.getElementById('configModalSave');
        const text = document.getElementById('configModalText').value;

        if (!modalConfigPath) return;

        saveBtn.disabled = true;
        statusEl.classList.remove('error');
        statusEl.textContent = 'Saving…';

        try {
            await ensureWsConnected(3000);
            const payload = JSON.stringify({ config: modalConfigPath, content: text });
            const raw = await ipcRequest('write-config', payload, 15000);
            const j = JSON.parse(raw);
            if (!j.ok) {
                statusEl.textContent = j.error || 'Save failed';
                statusEl.classList.add('error');
                saveBtn.disabled = false;
                return;
            }
            statusEl.textContent = 'Saved';
            setTimeout(() => closeConfigModal(), 250);
        } catch (e) {
            statusEl.textContent = String(e && e.message ? e.message : e);
            statusEl.classList.add('error');
            saveBtn.disabled = false;
        }
    }

    function renderActions(actions) {
        const el = document.getElementById('actionList');
        document.getElementById('actionCount').textContent = actions.length;

        if (actions.length === 0) {
            el.innerHTML = '<div class="empty-state">No actions recorded yet</div>';
            return;
        }

        el.innerHTML = actions.map(a => {
            const ok = a.return_code === 0;
            const statusCls = ok ? 'ok' : 'fail';
            const statusTxt = ok ? 'OK' : `exit ${a.return_code}`;

            // Shorten config path: show parent directory name
            const parts = a.config.replace(/\/webdash\.config\.json$/, '').split('/');
            const short = parts[parts.length - 1] || parts.slice(-2).join('/');

            // Task name may be "configpath#taskname" — extract just the task name
            const task = a.task.includes('#') ? a.task.split('#').pop() : a.task;

            const time = a.timestamp.substring(11, 19); // HH:MM:SS

            return `<div class="action-item">
                <span class="action-time">${esc(time)}</span>
                <span class="action-detail">
                    <span class="config-name" title="${esc(a.config)}">${esc(short)}</span>
                    <span class="task-name">#${esc(task)}</span>
                </span>
                <span class="action-status ${statusCls}">${statusTxt}</span>
                <button class="action-play" onclick="runTask(this, '${esc(a.config)}', '${esc(task)}')" title="Re-run this task">▶</button>
            </div>`;
        }).join('');
    }

    // ────────────────────────────────────────────────────────
    //  Helpers
    // ────────────────────────────────────────────────────────

    async function runTask(btn, config, task) {
        btn.classList.add('running');
        btn.textContent = '⏳';
        try {
            let j;
            if (mode === 'ws' && ws && ws.readyState === WebSocket.OPEN) {
                const payload = JSON.stringify({ config, task });
                const raw = await ipcRequest('run-task', payload);
                j = JSON.parse(raw);
            } else {
                const url = apiUrl('run-task') +
                    '?config=' + encodeURIComponent(config) +
                    '&task=' + encodeURIComponent(task);
                const resp = await fetch(url);
                j = await resp.json();
            }
            if (!j.ok) {
                btn.textContent = '✗';
                btn.title = j.error || 'failed';
            } else {
                btn.textContent = '✓';
            }
        } catch (e) {
            btn.textContent = '✗';
        }
        setTimeout(() => {
            btn.classList.remove('running');
            btn.textContent = '▶';
            btn.title = 'Re-run this task';
        }, 1500);
    }

    function esc(s) {
        const d = document.createElement('div');
        d.textContent = s;
        return d.innerHTML;
    }

    function stripHtml(s) {
        const d = document.createElement('div');
        d.innerHTML = s;
        return d.textContent || d.innerText || '';
    }

    function folderNameFromPath(p) {
        const parts = p.replace(/\/webdash\.config\.json$/, '').split('/');
        return parts[parts.length - 1] || p;
    }

    function setStatus(ok, transport) {
        document.getElementById('statusDot').classList.toggle('connected', ok);
        if (!ok) {
            document.getElementById('statusLabel').textContent = 'Disconnected';
        } else if (transport === 'ws') {
            document.getElementById('statusLabel').textContent = 'Connected (WebSocket)';
        } else {
            document.getElementById('statusLabel').textContent = 'Connected (HTTP)';
        }
    }

    // ── Boot ── fetch editor config, then connect
    fetchEditorConfig().then(() => connectWs());
