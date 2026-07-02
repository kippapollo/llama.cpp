<?php
// OpenAI-compatible, bearer-token-authenticated proxy for editor clients
// (Continue, Cursor, etc.). Reached via PATH_INFO, e.g.
//   POST /admin/v1.php/chat/completions
//   GET  /admin/v1.php/models
// so a client's apiBase is  http://<host>/admin/v1.php  and apiKey is the
// per-account token a user generates from the chat page (Account → API token).
//
// This mirrors the browser chat proxy (api.php ?action=chat): it enforces the
// host allow-list, tags the request with the account id so Logs/Usage attribute
// it correctly, and streams the response straight through from llama-server.
require_once __DIR__ . '/lib.php';
header('Cache-Control: no-store');

function v1_json($d, int $code = 200) {
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($d);
    exit;
}
function v1_err(string $msg, int $code, string $type = 'invalid_request_error', ?string $ecode = null) {
    v1_json(['error' => ['message' => $msg, 'type' => $type] + ($ecode ? ['code' => $ecode] : [])], $code);
}
// The Bearer token, read defensively (mod_php, CGI, and rewrite pass-through all differ).
function v1_bearer(): string {
    $h = (string) ($_SERVER['HTTP_AUTHORIZATION'] ?? $_SERVER['REDIRECT_HTTP_AUTHORIZATION'] ?? '');
    if ($h === '' && function_exists('apache_request_headers')) {
        foreach (apache_request_headers() as $k => $v) { if (strcasecmp($k, 'Authorization') === 0) { $h = (string) $v; break; } }
    }
    return (stripos($h, 'Bearer ') === 0) ? trim(substr($h, 7)) : '';
}

// Which sub-path? Prefer PATH_INFO; fall back to parsing the URI after /v1.php.
$path = (string) ($_SERVER['PATH_INFO'] ?? '');
if ($path === '') {
    $uri = (string) ($_SERVER['REQUEST_URI'] ?? '');
    $pos = strpos($uri, '/v1.php');
    if ($pos !== false) { $path = (string) strtok(substr($uri, $pos + strlen('/v1.php')), '?'); }
}
$path = '/' . ltrim($path, '/');

// Authenticate every request by bearer token → active account.
$acct = account_by_token(v1_bearer());
if (!$acct) { v1_err('Invalid API key. Generate one on the chat page under “API token”.', 401, 'invalid_request_error', 'invalid_api_key'); }

// Model list (Continue calls this to validate the connection).
if ($path === '/models') {
    v1_json(['object' => 'list', 'data' => [['id' => 'coder', 'object' => 'model', 'owned_by' => 'guardrail']]]);
}
if ($path !== '/chat/completions') { v1_err('Not found: ' . $path, 404); }
if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') { v1_err('POST required', 405); }

// Same host guardrail as the browser chat proxy.
$ip = (string) ($_SERVER['REMOTE_ADDR'] ?? '');
if (ENFORCE_CHAT_HOST_ALLOWLIST && !host_allowed($ip)) {
    v1_err('Access denied: your device (' . $ip . ') is not on the allowed-hosts list. Ask an administrator to approve it.', 403);
}
logs_backup_auto();   // monthly log archive (fast no-op unless the month rolled over)

$raw = (string) file_get_contents('php://input');
$req = json_decode($raw, true);
if (!is_array($req) || empty($req['messages']) || !is_array($req['messages'])) { v1_err('messages required', 400); }
if (empty($req['model'])) { $req['model'] = 'coder'; }
$req['user'] = (string) ($acct['id'] ?? '');   // attribute to the account for Logs/Usage
$stream = !empty($req['stream']);
if ($stream && empty($req['stream_options'])) { $req['stream_options'] = ['include_usage' => true]; }
$payload = json_encode($req);

if (!function_exists('curl_init')) { v1_err('curl not available on this server', 500, 'server_error'); }
$url = rtrim(SERVER_URL, '/') . '/v1/chat/completions';

if ($stream) {
    while (ob_get_level() > 0) { ob_end_flush(); }
    header('Content-Type: text/event-stream; charset=utf-8');
    header('Cache-Control: no-cache');
    header('X-Accel-Buffering: no');
    ignore_user_abort(true);
    $lastPing = 0.0;
    $ch = curl_init($url);
    curl_setopt_array($ch, [
        CURLOPT_POST       => true,
        CURLOPT_POSTFIELDS => $payload,
        CURLOPT_HTTPHEADER => ['Content-Type: application/json', 'Accept: text/event-stream'],
        CURLOPT_TIMEOUT    => 600,
        CURLOPT_WRITEFUNCTION => function ($ch, $data) {
            echo $data; @ob_flush(); @flush();
            return connection_aborted() ? 0 : strlen($data);   // client hung up → abort upstream, free the slot
        },
        CURLOPT_NOPROGRESS       => false,
        CURLOPT_PROGRESSFUNCTION => function ($c, $dt, $dn, $ut, $un) use (&$lastPing) {
            $now = microtime(true);
            if ($now - $lastPing >= 1.0) { $lastPing = $now; echo ": keep-alive\n\n"; @ob_flush(); @flush(); }
            return connection_aborted() ? 1 : 0;
        },
    ]);
    if (curl_exec($ch) === false && !connection_aborted()) {
        echo 'data: ' . json_encode(['error' => ['message' => 'cannot reach the coding agent server: ' . curl_error($ch)]]) . "\n\n";
    }
    curl_close($ch);
    exit;
}

// Non-streaming: pass the JSON response straight back with llama-server's status.
$ch = curl_init($url);
curl_setopt_array($ch, [
    CURLOPT_POST           => true,
    CURLOPT_POSTFIELDS     => $payload,
    CURLOPT_HTTPHEADER     => ['Content-Type: application/json'],
    CURLOPT_TIMEOUT        => 600,
    CURLOPT_RETURNTRANSFER => true,
]);
$resp = curl_exec($ch);
$code = (int) curl_getinfo($ch, CURLINFO_HTTP_CODE);
$cerr = curl_error($ch);
curl_close($ch);
if ($resp === false) { v1_err('cannot reach the coding agent server: ' . $cerr, 502, 'server_error'); }
http_response_code($code ?: 200);
header('Content-Type: application/json; charset=utf-8');
echo $resp;
