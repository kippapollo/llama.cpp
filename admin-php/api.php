<?php
// JSON API backend for the standalone admin frontend (index.html + app.js).
// All HTML rendering happens client-side; this only returns data.
require_once __DIR__ . '/lib.php';
start_session();
header('Cache-Control: no-store');

function out($d) { header('Content-Type: application/json; charset=utf-8'); echo json_encode($d); exit; }
function fail(string $msg, int $code = 400) { http_response_code($code); out(['error' => $msg]); }

$action = (string) ($_GET['action'] ?? '');
$isPost = $_SERVER['REQUEST_METHOD'] === 'POST';

$body = [];
if ($isPost) {
    $raw = file_get_contents('php://input');
    $j = $raw !== '' ? json_decode($raw, true) : null;
    $body = is_array($j) ? $j : $_POST;
}

function require_post() { if ($_SERVER['REQUEST_METHOD'] !== 'POST') { fail('POST required', 405); } }
// Admin = the admin-password session OR a logged-in chat user whose role is 'admin'.
function is_admin() {
    if (!empty($_SESSION['authed'])) { return true; }
    if (!empty($_SESSION['user'])) {
        $u = account_find((string) $_SESSION['user']);
        if ($u && ($u['status'] ?? '') === 'active' && ($u['role'] ?? '') === 'admin') { return true; }
    }
    return false;
}
function require_auth_api() { if (!is_admin()) { fail('unauthorized', 401); } }
// Chat endpoints: allow either a logged-in chat user or the admin.
function require_user_api() { if (empty($_SESSION['user']) && empty($_SESSION['authed'])) { fail('unauthorized', 401); } }
function client_ip() { return (string) ($_SERVER['REMOTE_ADDR'] ?? ''); }
// Key under which the current session's chats are stored.
function current_chat_owner() {
    if (!empty($_SESSION['user'])) { return (string) $_SESSION['user']; }
    if (!empty($_SESSION['authed'])) { return '__admin__'; }
    return '';
}
// {id,name} for the logged-in chat user, or null. Drops the session if the
// account was removed or suspended since login.
function current_user_public() {
    if (empty($_SESSION['user'])) { return null; }
    $u = account_find((string) $_SESSION['user']);
    if (!$u || ($u['status'] ?? '') !== 'active') { unset($_SESSION['user']); return null; }
    return ['id' => $u['id'], 'name' => $u['name'], 'role' => (string) ($u['role'] ?? 'user')];
}
function require_csrf(array $body) {
    $t = $_SERVER['HTTP_X_CSRF'] ?? ($body['csrf'] ?? '');
    if (empty($_SESSION['csrf']) || !is_string($t) || !hash_equals($_SESSION['csrf'], $t)) { fail('bad csrf token', 403); }
}

$valid_date = fn($d) => is_string($d) && preg_match('/^\d{4}-\d{2}-\d{2}$/', $d);
$date_bounds = function () {
    $d = array_values(array_filter(array_map(fn($f) => log_date_from_name($f['name']), log_files(LOG_DIR))));
    return ['minDate' => $d ? min($d) : '', 'maxDate' => $d ? max($d) : date('Y-m-d')];
};

switch ($action) {

case 'session':
    out([
        'authed'     => !empty($_SESSION['authed']),      // admin-password session
        'user'       => current_user_public(),            // chat user {id,name,role} or null
        'isAdmin'    => is_admin(),                        // admin via password OR admin-role user
        'needsSetup' => !admin_password_is_set(),          // admin first-run
        'csrf'       => csrf_token(),
    ]);

// ---- chat-user signup / login / logout ------------------------------------
case 'signup':
    require_post(); require_csrf($body);
    $ip = client_ip();
    $macs = array_values(array_filter([resolve_mac($ip)]));
    $r = account_signup((string) ($body['id'] ?? ''), (string) ($body['name'] ?? ''), (string) ($body['password'] ?? ''), $ip, $macs);
    if (!empty($r['errors'])) { out(['errors' => $r['errors']]); }
    out(['ok' => true, 'pending' => true]);

case 'user_login':
    require_post(); require_csrf($body);
    $r = account_verify((string) ($body['id'] ?? ''), (string) ($body['password'] ?? ''));
    if (!empty($r['ok'])) {
        session_regenerate_id(true);
        $_SESSION['user'] = $r['user']['id'];
        out(['ok' => true, 'user' => $r['user'], 'csrf' => csrf_token()]);
    }
    if (!empty($r['pending'])) { usleep(200000); fail('Your account is pending administrator approval.', 403); }
    usleep(300000);
    fail($r['error'] ?? 'Login failed.', 401);

case 'user_logout':
    require_post(); require_csrf($body);
    unset($_SESSION['user']);
    out(['ok' => true]);

case 'user_password':   // a signed-in chat user changes their own password
    require_post(); require_csrf($body);
    if (empty($_SESSION['user'])) { fail('not signed in', 401); }
    $r = account_change_password((string) $_SESSION['user'], (string) ($body['current'] ?? ''), (string) ($body['new'] ?? ''));
    if (!empty($r['error'])) { fail($r['error']); }
    out(['ok' => true]);

case 'user_token_status':   // does the signed-in user have an API token? (no plaintext)
    if (empty($_SESSION['user'])) { fail('not signed in', 401); }
    out(account_token_info((string) $_SESSION['user']));

case 'user_token':   // a signed-in user generates or revokes their own API token
    require_post(); require_csrf($body);
    if (empty($_SESSION['user'])) { fail('not signed in', 401); }
    $uid = (string) $_SESSION['user'];
    $op = (string) ($body['op'] ?? '');
    if ($op === 'generate') { $r = account_generate_token($uid); }
    elseif ($op === 'revoke') { $r = account_revoke_token($uid); }
    else { fail('unknown op'); }
    if (!empty($r['error'])) { fail($r['error']); }
    out($r + account_token_info($uid));   // includes plaintext 'token' on generate (shown once)

// ---- per-user chat history -------------------------------------------------
case 'chats_load':
    require_user_api();
    out(['sessions' => chats_load_user(current_chat_owner())]);

case 'chats_save':
    require_user_api(); require_post(); require_csrf($body);
    $sessions = $body['sessions'] ?? [];
    if (!is_array($sessions)) { fail('sessions must be an array'); }
    if (strlen((string) json_encode($sessions)) > 8 * 1024 * 1024) { fail('chat history too large', 413); }
    if (!chats_save_user(current_chat_owner(), $sessions)) { fail('could not save chat history', 500); }
    out(['ok' => true]);

// ---- admin: chat-user account management -----------------------------------
case 'accounts':
    require_auth_api();
    out(['accounts' => array_map('account_public', accounts_read())]);

case 'account_action':
    require_auth_api(); require_post(); require_csrf($body);
    $id = (string) ($body['id'] ?? ''); $act = (string) ($body['act'] ?? '');
    if ($id === '') { fail('id required'); }
    $acct = account_find($id);
    $warn = [];
    if ($act === 'approve') { account_set_status($id, 'active'); }
    elseif ($act === 'approve_allow') {
        account_set_status($id, 'active');
        if ($acct) { $warn = allowlist_add_host((string) ($acct['ip'] ?? ''), (string) ($acct['name'] ?? $id), (array) ($acct['macs'] ?? [])); }
    }
    elseif ($act === 'allow_host') {
        if ($acct) { $warn = allowlist_add_host((string) ($acct['ip'] ?? ''), (string) ($acct['name'] ?? $id), (array) ($acct['macs'] ?? [])); }
    }
    elseif ($act === 'suspend') {
        account_set_status($id, 'pending');
        if ($acct && !empty($acct['ip']) && allowlist_remove_host((string) $acct['ip'])) {
            $warn[] = 'Removed ' . $acct['ip'] . ' from the access-allow list.';
        }
    }
    elseif ($act === 'make_admin') { account_set_role($id, 'admin'); }
    elseif ($act === 'revoke_admin') { account_set_role($id, 'user'); }
    elseif ($act === 'reject' || $act === 'delete') { account_delete($id); }
    else { fail('bad act'); }
    out(['ok' => true, 'warn' => $warn, 'accounts' => array_map('account_public', accounts_read())]);

case 'setup':
    require_post(); require_csrf($body);
    if (admin_password_is_set()) { fail('already configured'); }
    $pw = (string) ($body['password'] ?? '');
    if (strlen($pw) < 8) { fail('Password must be at least 8 characters.'); }
    if (!set_admin_password($pw)) { fail('Could not write the password file (' . ADMIN_HASH_FILE . ').', 500); }
    out(['ok' => true]);

case 'login':
    require_post(); require_csrf($body);
    if (verify_admin_password((string) ($body['password'] ?? ''))) {
        session_regenerate_id(true);
        $_SESSION['authed'] = true;
        out(['ok' => true, 'csrf' => csrf_token()]);
    }
    usleep(300000);
    fail('Incorrect password.', 401);

case 'logout':
    require_post(); require_csrf($body);
    $_SESSION = [];
    session_destroy();
    out(['ok' => true]);

case 'overview':
    require_auth_api();
    session_write_close();   // release the session lock before slow work (shell/HTTP probes)
    logs_backup_auto();      // monthly: archive completed months' logs (no-op unless the month rolled over)
    $today = date('Y-m-d');
    $todayFile = rtrim(LOG_DIR, '/\\') . DIRECTORY_SEPARATOR . "llama-server-$today.jsonl";
    $perday = [];
    foreach (log_perday_counts(LOG_DIR, 14) as $k => $v) { $perday[] = ['label' => $k, 'count' => $v]; }
    $accts = accounts_read();
    $acctSummary = ['total' => count($accts), 'active' => 0, 'pending' => 0, 'admins' => 0];
    foreach ($accts as $u) {
        $s = (string) ($u['status'] ?? '');
        if ($s === 'active') { $acctSummary['active']++; } elseif ($s === 'pending') { $acctSummary['pending']++; }
        if (($u['role'] ?? '') === 'admin') { $acctSummary['admins']++; }
    }
    out([
        'health'        => server_health(),
        'allowedHosts'  => count(allowlist_read(ACCESS_ALLOW_FILE)),
        'accounts'      => $acctSummary,
        'requestsToday' => is_file($todayFile) ? substr_count((string) @file_get_contents($todayFile), "\n") : 0,
        'logFiles'      => count(log_files(LOG_DIR)),
        'perday'        => $perday,
        'config'        => ['access_allow_file' => ACCESS_ALLOW_FILE, 'log_dir' => LOG_DIR, 'allow_found' => is_file(ACCESS_ALLOW_FILE), 'log_found' => is_dir(LOG_DIR)],
        'serverCtl'     => server_ctl_status(),
    ]);

case 'activity':   // requests-per-day for a chosen number of days OR a date range
    require_auth_api();
    session_write_close();
    $af = $valid_date($_GET['from'] ?? '') ? $_GET['from'] : '';
    $at = $valid_date($_GET['to'] ?? '') ? $_GET['to'] : '';
    $rows = [];
    if ($af !== '' && $at !== '') {
        if ($af > $at) { $tmp = $af; $af = $at; $at = $tmp; }
        foreach (perday_counts_range(LOG_DIR, $af, $at) as $k => $v) { $rows[] = ['label' => $k, 'count' => $v]; }
        out(['perday' => $rows, 'from' => $af, 'to' => $at]);
    }
    $days = isset($_GET['days']) ? max(1, min(90, (int) $_GET['days'])) : 14;
    foreach (log_perday_counts(LOG_DIR, $days) as $k => $v) { $rows[] = ['label' => $k, 'count' => $v]; }
    out(['perday' => $rows, 'days' => $days]);

// ---- llama-server launch control (start / stop / edit command line) --------
case 'server_config':
    require_auth_api();
    out(server_ctl_status());

case 'server_status':   // light poll for the dashboard: health + run state
    require_auth_api();
    session_write_close();
    out(['health' => server_health(), 'ctl' => server_ctl_status()]);

case 'server_log':      // tail the captured llama-server console (incremental via ?from=)
    require_auth_api();
    session_write_close();
    out(server_log_tail(isset($_GET['from']) ? (int) $_GET['from'] : 0));

case 'server_config_save':
    require_auth_api(); require_post(); require_csrf($body);
    if (!server_launch_config_save(trim((string) ($body['exe'] ?? '')), (string) ($body['args'] ?? ''))) { fail('Could not save the launch config.', 500); }
    out(['ok' => true] + server_ctl_status());

case 'server_start':
    require_auth_api(); require_post(); require_csrf($body);
    session_write_close();
    out(server_start() + server_ctl_status());

case 'server_stop':
    require_auth_api(); require_post(); require_csrf($body);
    session_write_close();
    out(server_stop() + server_ctl_status());

case 'health':
    require_auth_api();
    out(server_health());

case 'stats':
    require_auth_api();
    out(get_system_stats());

case 'config':
    require_auth_api();
    out([
        'access_allow_file' => ACCESS_ALLOW_FILE, 'log_dir' => LOG_DIR,
        'allow_found' => is_file(ACCESS_ALLOW_FILE), 'log_found' => is_dir(LOG_DIR),
    ]);

case 'users':
    require_auth_api();
    out(['users' => allowlist_read(ACCESS_ALLOW_FILE)]);

case 'users_save':
    require_auth_api(); require_post(); require_csrf($body);
    $act = (string) ($body['act'] ?? '');
    $entries = allowlist_read(ACCESS_ALLOW_FILE);
    if ($act === 'delete') {
        $ip = (string) ($body['ip'] ?? '');
        $entries = array_values(array_filter($entries, fn($e) => $e['ip'] !== $ip));
    } elseif ($act === 'add' || $act === 'update') {
        $new = [
            'alias' => trim((string) ($body['alias'] ?? '')),
            'ip'    => trim((string) ($body['ip'] ?? '')),
            'macs'  => parse_macs((string) ($body['macs'] ?? '')),
        ];
        if ($act === 'update') {
            $oip = (string) ($body['original_ip'] ?? '');
            $entries = array_values(array_map(fn($e) => $e['ip'] === $oip ? $new : $e, $entries));
        } else {
            $entries[] = $new;
        }
    } else {
        fail('bad act');
    }
    $errs = allowlist_save(ACCESS_ALLOW_FILE, $entries);
    if ($errs) { out(['errors' => $errs]); }
    out(['ok' => true, 'users' => allowlist_read(ACCESS_ALLOW_FILE)]);

case 'logs':
    require_auth_api();
    $from = $valid_date($_GET['from'] ?? '') ? $_GET['from'] : '';
    $to   = $valid_date($_GET['to'] ?? '') ? $_GET['to'] : '';
    $after = (string) ($_GET['after'] ?? '');
    $map = allowlist_ip_alias_map(ACCESS_ALLOW_FILE);
    $acctMap = accounts_ip_name_map();
    $idMap = accounts_id_name_map();
    $acctIpIdMap = accounts_ip_id_map();
    $rows = [];
    foreach (read_logs_range(LOG_DIR, $from, $to, LOG_TAIL_LINES) as $r) {
        $ts = (string) ($r['timestamp'] ?? '');
        if ($after !== '' && $ts < $after) { continue; }
        $rows[] = log_record_to_row($r, $map, $acctMap, $idMap, $acctIpIdMap);
    }
    out(['rows' => $rows, 'cap' => LOG_TAIL_LINES, 'backups' => logs_backups_list()] + $date_bounds());

case 'logs_backup_view':   // browse the request logs inside a backup zip (same table as live logs)
    require_auth_api();
    $name = basename((string) ($_GET['name'] ?? ''));
    $recs = logs_backup_read($name, LOG_TAIL_LINES);
    if ($recs === null) { fail('backup not found', 404); }
    $map = allowlist_ip_alias_map(ACCESS_ALLOW_FILE);
    $acctMap = accounts_ip_name_map();
    $idMap = accounts_id_name_map();
    $acctIpIdMap = accounts_ip_id_map();
    $rows = [];
    foreach ($recs as $r) { $rows[] = log_record_to_row($r, $map, $acctMap, $idMap, $acctIpIdMap); }
    out(['rows' => $rows, 'cap' => LOG_TAIL_LINES, 'zip' => $name]);

case 'logs_backup':   // zip logs older than today and remove them
    require_auth_api(); require_post(); require_csrf($body);
    session_write_close();
    $r = logs_backup(date('Y-m-d'));
    if (!empty($r['error'])) { fail($r['error']); }
    out($r + ['backups' => logs_backups_list()]);

case 'logs_backup_download':   // stream a backup zip
    require_auth_api();
    $name = basename((string) ($_GET['name'] ?? ''));
    $path = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . $name;
    if (!preg_match('/^[\w.\-]+\.zip$/', $name) || !is_file($path)) { fail('backup not found', 404); }
    while (ob_get_level() > 0) { ob_end_flush(); }
    header('Content-Type: application/zip');
    header('Content-Disposition: attachment; filename="' . $name . '"');
    header('Content-Length: ' . filesize($path));
    header('Cache-Control: no-store');
    readfile($path);
    exit;

case 'usage':
    require_auth_api();
    $from = $valid_date($_GET['from'] ?? '') ? $_GET['from'] : '';
    $to   = $valid_date($_GET['to'] ?? '') ? $_GET['to'] : '';
    $map = allowlist_ip_alias_map(ACCESS_ALLOW_FILE);
    $acctMap = accounts_ip_name_map();
    $idMap = accounts_id_name_map();
    $users = array_map(function ($e) {
        $e['avgLatency'] = $e['avg_lat'] !== null ? format_latency($e['avg_lat']) : '';
        unset($e['lat_sum'], $e['lat_n'], $e['avg_lat']);
        return $e;
    }, array_values(usage_by_user(LOG_DIR, $map, 20000, $from, $to, $acctMap, $idMap)));

    if ($from !== '' && $from === $to) {
        $bucket = 'hour'; $cfrom = $from; $cto = $to;
    } elseif ($from === '' && $to === '') {
        $bucket = 'day'; $b = $date_bounds(); $cfrom = $b['minDate'] ?: $b['maxDate']; $cto = $b['maxDate'];
    } else {
        $bucket = 'day'; $cfrom = $from; $cto = $to;
    }
    $series = [];
    foreach (prompt_timeseries(LOG_DIR, $cfrom, $cto, $bucket) as $k => $v) {
        $series[] = ['label' => $k, 'count' => $v];
    }
    out(['users' => $users, 'timeseries' => $series, 'bucket' => $bucket] + $date_bounds());

case 'chat':
    // Proxy a streaming chat completion to llama-server and pipe the SSE back.
    require_user_api(); require_post(); require_csrf($body);
    session_write_close();   // release the session lock so other requests aren't blocked during the (long) stream
    if (ENFORCE_CHAT_HOST_ALLOWLIST && !host_allowed(client_ip())) {
        while (ob_get_level() > 0) { ob_end_flush(); }
        header('Content-Type: text/event-stream; charset=utf-8');
        echo 'data: ' . json_encode(['error' => ['message' => 'Access denied: your device (' . client_ip() . ') is not on the allowed-hosts list. Ask an administrator to approve it.']]) . "\n\n";
        exit;
    }
    logs_backup_auto();   // monthly log archive — checked on every prompt request (fast no-op unless the month rolled over)
    $messages = $body['messages'] ?? [];
    if (!is_array($messages) || !$messages) { fail('messages required'); }
    $payload = json_encode([
        'model'          => 'coder',
        'messages'       => $messages,
        'stream'         => true,
        'stream_options' => ['include_usage' => true],   // emit a final usage chunk (tokens + timings)
        'temperature'    => isset($body['temperature']) ? (float) $body['temperature'] : 0.7,
        'max_tokens'     => isset($body['max_tokens']) ? (int) $body['max_tokens'] : 2048,
        'user'           => current_chat_owner(),         // logged so Logs/Usage can show the real account
    ]);
    while (ob_get_level() > 0) { ob_end_flush(); }
    header('Content-Type: text/event-stream; charset=utf-8');
    header('Cache-Control: no-cache');
    header('X-Accel-Buffering: no');
    if (!function_exists('curl_init')) {
        echo "data: " . json_encode(['error' => ['message' => 'curl not available']]) . "\n\n";
        exit;
    }
    // Keep the script alive on client disconnect so WE decide when to bail (below),
    // instead of PHP killing us mid-request and leaking the upstream connection.
    ignore_user_abort(true);
    $lastPing = 0.0;
    $ch = curl_init(rtrim(SERVER_URL, '/') . '/v1/chat/completions');
    curl_setopt_array($ch, [
        CURLOPT_POST          => true,
        CURLOPT_POSTFIELDS    => $payload,
        CURLOPT_HTTPHEADER    => ['Content-Type: application/json', 'Accept: text/event-stream'],
        CURLOPT_TIMEOUT       => 600,
        // If the browser hit Stop, the client connection is aborted. Returning a byte
        // count != strlen($data) makes curl abort the transfer, which closes the socket
        // to llama-server; llama-server then stops generation and frees the slot.
        CURLOPT_WRITEFUNCTION => function ($ch, $data) {
            echo $data; @ob_flush(); @flush();
            return connection_aborted() ? 0 : strlen($data);
        },
        // Fires periodically even while no tokens flow (e.g. during prompt processing);
        // a throttled heartbeat probes the client link so a Stop is noticed within ~1s.
        // Returning non-zero aborts the transfer.
        CURLOPT_NOPROGRESS       => false,
        CURLOPT_PROGRESSFUNCTION => function ($ch, $dltotal, $dlnow, $ultotal, $ulnow) use (&$lastPing) {
            $now = microtime(true);
            if ($now - $lastPing >= 1.0) { $lastPing = $now; echo ": keep-alive\n\n"; @ob_flush(); @flush(); }
            return connection_aborted() ? 1 : 0;
        },
    ]);
    if (curl_exec($ch) === false && !connection_aborted()) {
        echo "data: " . json_encode(['error' => ['message' => 'cannot reach the coding agent server: ' . curl_error($ch)]]) . "\n\n";
    }
    curl_close($ch);
    exit;

case 'props':
    // Report the model's context window (n_ctx) so the UI can show context usage.
    require_auth_api();
    $ctx = 0; $model = '';
    if (function_exists('curl_init')) {
        $ch = curl_init(rtrim(SERVER_URL, '/') . '/props');
        curl_setopt_array($ch, [CURLOPT_RETURNTRANSFER => true, CURLOPT_TIMEOUT => 5]);
        $resp = curl_exec($ch);
        curl_close($ch);
        if (is_string($resp) && $resp !== '') {
            $j = json_decode($resp, true);
            if (is_array($j)) {
                $ctx = (int) ($j['default_generation_settings']['n_ctx'] ?? $j['n_ctx'] ?? 0);
                $model = (string) ($j['model_path'] ?? $j['default_generation_settings']['model'] ?? '');
                if ($model !== '') { $model = basename($model); }
            }
        }
    }
    out(['n_ctx' => $ctx, 'model' => $model]);

default:
    fail('unknown action: ' . $action, 404);
}
