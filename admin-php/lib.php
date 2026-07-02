<?php
// Shared helpers for the guardrail admin app.
declare(strict_types=1);

require_once __DIR__ . '/config.php';

// ---------------------------------------------------------------------------
// Session / auth
// ---------------------------------------------------------------------------

function start_session(): void {
    if (session_status() === PHP_SESSION_ACTIVE) {
        return;
    }
    session_name(SESSION_NAME);
    session_set_cookie_params([
        'httponly' => true,
        'samesite' => 'Strict',
        // 'secure' => true,  // enable if serving over HTTPS
    ]);
    session_start();
}

function admin_password_is_set(): bool {
    return is_file(ADMIN_HASH_FILE) && trim((string) @file_get_contents(ADMIN_HASH_FILE)) !== '';
}

function set_admin_password(string $plain): bool {
    $hash = password_hash($plain, PASSWORD_DEFAULT);
    if ($hash === false) {
        return false;
    }
    return atomic_write(ADMIN_HASH_FILE, $hash . "\n");
}

function verify_admin_password(string $plain): bool {
    if (!admin_password_is_set()) {
        return false;
    }
    $hash = trim((string) file_get_contents(ADMIN_HASH_FILE));
    return password_verify($plain, $hash);
}

function is_logged_in(): bool {
    return !empty($_SESSION['authed']);
}

// Call at the top of every protected page.
function require_auth(): void {
    start_session();
    if (!admin_password_is_set()) {
        header('Location: setup.php');
        exit;
    }
    if (!is_logged_in()) {
        header('Location: login.php');
        exit;
    }
}

// ---------------------------------------------------------------------------
// Chat-user accounts (self-signup + admin approval)
//   Stored as JSON: {"users":[{id,name,hash,status,ip,macs,created,approved}]}
//   status: pending | active | rejected
// ---------------------------------------------------------------------------

function accounts_read(): array {
    if (!is_file(ACCOUNTS_FILE)) {
        return [];
    }
    $j = json_decode((string) @file_get_contents(ACCOUNTS_FILE), true);
    return (is_array($j) && isset($j['users']) && is_array($j['users'])) ? $j['users'] : [];
}

function accounts_write(array $users): bool {
    return atomic_write(ACCOUNTS_FILE, json_encode(['users' => array_values($users)], JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n");
}

function account_find(string $id): ?array {
    foreach (accounts_read() as $u) {
        if (($u['id'] ?? '') === $id) {
            return $u;
        }
    }
    return null;
}

function valid_user_id(string $id): bool {
    return (bool) preg_match('/^[a-zA-Z0-9._-]{2,32}$/', $id);
}

// Account id => display name (for labelling logs/usage from the request's 'user' tag).
function accounts_id_name_map(): array {
    $map = [];
    foreach (accounts_read() as $u) {
        $id = trim((string) ($u['id'] ?? ''));
        if ($id !== '') { $map[$id] = trim((string) ($u['name'] ?? '')) ?: $id; }
    }
    return $map;
}

// IP => account display name (for labelling logs/usage). Skips loopback and blanks;
// first account per IP wins.
function accounts_ip_name_map(): array {
    $map = [];
    foreach (accounts_read() as $u) {
        $ip = trim((string) ($u['ip'] ?? ''));
        $name = trim((string) ($u['name'] ?? ''));
        if ($ip === '' || $ip === '127.0.0.1' || $ip === '::1' || $name === '') { continue; }
        if (!isset($map[$ip])) { $map[$ip] = $name; }
    }
    return $map;
}

// IP => account id (for resolving a suspend target when a log row is identified
// only by IP, not by an OpenAI 'user' tag). Skips loopback; first account wins.
function accounts_ip_id_map(): array {
    $map = [];
    foreach (accounts_read() as $u) {
        $ip = trim((string) ($u['ip'] ?? ''));
        $id = trim((string) ($u['id'] ?? ''));
        if ($ip === '' || $ip === '127.0.0.1' || $ip === '::1' || $id === '') { continue; }
        if (!isset($map[$ip])) { $map[$ip] = $id; }
    }
    return $map;
}

// Public projection of an account (no password hash) for API responses.
function account_public(array $u): array {
    return [
        'id'       => (string) ($u['id'] ?? ''),
        'name'     => (string) ($u['name'] ?? ''),
        'status'   => (string) ($u['status'] ?? ''),
        'role'     => (string) ($u['role'] ?? 'user'),
        'ip'       => (string) ($u['ip'] ?? ''),
        'macs'     => array_values((array) ($u['macs'] ?? [])),
        'created'  => (string) ($u['created'] ?? ''),
        'approved' => (string) ($u['approved'] ?? ''),
    ];
}

// Create a pending account. Returns ['ok'=>true] or ['errors'=>[...]].
function account_signup(string $id, string $name, string $plain, string $ip, array $macs): array {
    $errs = [];
    if (!valid_user_id($id)) { $errs[] = 'ID must be 2–32 characters: letters, digits, dot, underscore or hyphen.'; }
    if (trim($name) === '') { $errs[] = 'Display name is required.'; }
    if (strlen($plain) < 8) { $errs[] = 'Password must be at least 8 characters.'; }
    if ($errs) { return ['errors' => $errs]; }
    $users = accounts_read();
    foreach ($users as $u) {
        if (strcasecmp((string) ($u['id'] ?? ''), $id) === 0) { return ['errors' => ['That ID is already taken.']]; }
    }
    $users[] = [
        'id'       => $id,
        'name'     => trim($name),
        'hash'     => password_hash($plain, PASSWORD_DEFAULT),
        'status'   => 'pending',
        'role'     => 'user',
        'ip'       => $ip,
        'macs'     => array_values($macs),
        'created'  => date('Y-m-d H:i'),
        'approved' => '',
    ];
    if (!accounts_write($users)) { return ['errors' => ['Could not save the account (server data file not writable).']]; }
    return ['ok' => true];
}

// Verify credentials. Returns ['ok'=>true,'user'=>{id,name}] | ['pending'=>true] | ['error'=>msg].
function account_verify(string $id, string $plain): array {
    $u = account_find($id);
    if (!$u || !password_verify($plain, (string) ($u['hash'] ?? ''))) { return ['error' => 'Incorrect ID or password.']; }
    $status = (string) ($u['status'] ?? '');
    if ($status === 'pending') { return ['pending' => true]; }
    if ($status !== 'active') { return ['error' => 'This account is not permitted. Contact the administrator.']; }
    return ['ok' => true, 'user' => ['id' => $u['id'], 'name' => $u['name'], 'role' => (string) ($u['role'] ?? 'user')]];
}

function account_set_status(string $id, string $status): bool {
    $users = accounts_read();
    $changed = false;
    foreach ($users as &$u) {
        if (($u['id'] ?? '') === $id) {
            $u['status'] = $status;
            if ($status === 'active' && empty($u['approved'])) { $u['approved'] = date('Y-m-d H:i'); }
            $changed = true;
        }
    }
    unset($u);
    return $changed ? accounts_write($users) : false;
}

// A user changing their own password. Returns ['ok'=>true] or ['error'=>msg].
function account_change_password(string $id, string $current, string $new): array {
    if (strlen($new) < 8) { return ['error' => 'New password must be at least 8 characters.']; }
    $users = accounts_read();
    foreach ($users as &$u) {
        if (($u['id'] ?? '') === $id) {
            if (!password_verify($current, (string) ($u['hash'] ?? ''))) { return ['error' => 'Current password is incorrect.']; }
            $u['hash'] = password_hash($new, PASSWORD_DEFAULT);
            unset($u);
            return accounts_write($users) ? ['ok' => true] : ['error' => 'Could not save the new password.'];
        }
    }
    unset($u);
    return ['error' => 'Account not found.'];
}

// ---- per-account API tokens (for OpenAI-compatible clients like Continue) ----
// Stored as a SHA-256 hash (the plaintext is shown once, at generation). One
// token per account; regenerating replaces the old one.

function account_token_info(string $id): array {
    $u = account_find($id);
    if (!$u) { return ['has' => false, 'created' => '']; }
    $has = trim((string) ($u['token_hash'] ?? '')) !== '';
    return ['has' => $has, 'created' => $has ? (string) ($u['token_created'] ?? '') : ''];
}

function account_generate_token(string $id): array {
    $plain = 'sk-guard-' . rtrim(strtr(base64_encode(random_bytes(24)), '+/', '-_'), '=');
    $when = date('Y-m-d H:i');
    $users = accounts_read();
    foreach ($users as &$u) {
        if (($u['id'] ?? '') === $id) {
            $u['token_hash'] = hash('sha256', $plain);
            $u['token_created'] = $when;
            unset($u);
            return accounts_write($users) ? ['ok' => true, 'token' => $plain, 'created' => $when] : ['error' => 'Could not save the token.'];
        }
    }
    unset($u);
    return ['error' => 'Account not found.'];
}

function account_revoke_token(string $id): array {
    $users = accounts_read();
    $changed = false;
    foreach ($users as &$u) {
        if (($u['id'] ?? '') === $id) { unset($u['token_hash'], $u['token_created']); $changed = true; }
    }
    unset($u);
    if (!$changed) { return ['error' => 'Account not found.']; }
    return accounts_write($users) ? ['ok' => true] : ['error' => 'Could not update the account.'];
}

// Resolve a presented bearer token to its (active) account, or null. Constant-time.
function account_by_token(string $token): ?array {
    $token = trim($token);
    if ($token === '') { return null; }
    $h = hash('sha256', $token);
    foreach (accounts_read() as $u) {
        $th = trim((string) ($u['token_hash'] ?? ''));
        if ($th !== '' && hash_equals($th, $h)) {
            return (($u['status'] ?? '') === 'active') ? $u : null;   // token valid only while the account is active
        }
    }
    return null;
}

function account_set_role(string $id, string $role): bool {
    if ($role !== 'admin' && $role !== 'user') { return false; }
    $users = accounts_read();
    $changed = false;
    foreach ($users as &$u) { if (($u['id'] ?? '') === $id) { $u['role'] = $role; $changed = true; } }
    unset($u);
    return $changed ? accounts_write($users) : false;
}

function account_delete(string $id): bool {
    $users = array_values(array_filter(accounts_read(), fn($u) => ($u['id'] ?? '') !== $id));
    return accounts_write($users);
}

// Best-effort MAC lookup for a client IP via the local ARP cache (LAN only).
function resolve_mac(string $ip): string {
    if ($ip === '' || $ip === '127.0.0.1' || $ip === '::1' || !function_exists('shell_exec')) {
        return '';
    }
    @shell_exec('ping -n 1 -w 200 ' . escapeshellarg($ip) . ' 2>NUL');  // prime the ARP cache
    $out = (string) @shell_exec('arp -a ' . escapeshellarg($ip) . ' 2>NUL');
    if (preg_match('/([0-9a-f]{2}[:-]){5}[0-9a-f]{2}/i', $out, $m)) {
        return normalize_mac(str_replace('-', ':', $m[0]));
    }
    return '';
}

// ---------------------------------------------------------------------------
// Per-user chat history (server-side saved chats)
// ---------------------------------------------------------------------------

function chats_file(string $owner): string {
    $safe = preg_replace('/[^a-zA-Z0-9._-]/', '_', $owner);
    return rtrim(CHATS_DIR, '/\\') . DIRECTORY_SEPARATOR . $safe . '.json';
}

function chats_load_user(string $owner): array {
    $f = chats_file($owner);
    if (!is_file($f)) { return []; }
    $j = json_decode((string) @file_get_contents($f), true);
    return is_array($j) ? $j : [];
}

function chats_save_user(string $owner, array $sessions): bool {
    return atomic_write(chats_file($owner), json_encode($sessions, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE));
}

// ---------------------------------------------------------------------------
// CSRF
// ---------------------------------------------------------------------------

function csrf_token(): string {
    start_session();
    if (empty($_SESSION['csrf'])) {
        $_SESSION['csrf'] = bin2hex(random_bytes(32));
    }
    return $_SESSION['csrf'];
}

function csrf_field(): string {
    return '<input type="hidden" name="csrf" value="' . h(csrf_token()) . '">';
}

function check_csrf(): void {
    start_session();
    $token = $_POST['csrf'] ?? '';
    if (!is_string($token) || empty($_SESSION['csrf']) || !hash_equals($_SESSION['csrf'], $token)) {
        http_response_code(400);
        exit('Invalid CSRF token. Go back and try again.');
    }
}

// ---------------------------------------------------------------------------
// Flash messages
// ---------------------------------------------------------------------------

function flash(string $type, string $msg): void {
    start_session();
    $_SESSION['flash'][] = ['type' => $type, 'msg' => $msg];
}

function take_flashes(): array {
    start_session();
    $f = $_SESSION['flash'] ?? [];
    unset($_SESSION['flash']);
    return $f;
}

// ---------------------------------------------------------------------------
// Output escaping
// ---------------------------------------------------------------------------

function h($v): string {
    return htmlspecialchars((string) $v, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

// Truncate a string to $max bytes without requiring the mbstring extension.
function truncate(string $s, int $max): string {
    return strlen($s) > $max ? substr($s, 0, $max) . ' …' : $s;
}

// ---------------------------------------------------------------------------
// Atomic file write (works on POSIX and Windows)
// ---------------------------------------------------------------------------

function atomic_write(string $path, string $data): bool {
    $dir = dirname($path);
    if (!is_dir($dir) && !@mkdir($dir, 0700, true) && !is_dir($dir)) {
        return false;
    }
    $tmp = @tempnam($dir, 'tmp');
    if ($tmp === false) {
        return false;
    }
    // No LOCK_EX: the temp file is private (unique name), and on Windows an
    // exclusive lock on a freshly written file can be briefly held by the AV
    // scanner, which blocks the rename below.
    if (@file_put_contents($tmp, $data) === false) {
        @unlink($tmp);
        return false;
    }
    // Replace the target. POSIX rename overwrites atomically. On Windows rename
    // does not overwrite, and the source can be momentarily locked (AV scan), so
    // retry with a short backoff, clearing the target between attempts. The
    // server keeps its last good rules if it reads during the tiny gap.
    for ($i = 0; $i < 40; $i++) {
        if (@rename($tmp, $path)) {
            return true;
        }
        if (is_file($path)) {
            @unlink($path);
        }
        usleep(50000); // 50 ms
    }
    @unlink($tmp);
    return false;
}

// ---------------------------------------------------------------------------
// Access-allow list (mirrors the server's CSV format and validation)
//   line format:  alias,ip,mac1[,mac2...]
//   '#' comments and blank lines are ignored.
// ---------------------------------------------------------------------------

function is_valid_ip(string $ip): bool {
    return filter_var($ip, FILTER_VALIDATE_IP) !== false;
}

function is_valid_mac(string $mac): bool {
    return (bool) preg_match('/^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/i', $mac);
}

function normalize_mac(string $mac): string {
    return strtolower(trim($mac));
}

// Parse macs from a free-form field (comma or whitespace separated).
function parse_macs(string $field): array {
    $parts = preg_split('/[\s,]+/', trim($field)) ?: [];
    $out = [];
    foreach ($parts as $p) {
        if ($p !== '') {
            $out[] = normalize_mac($p);
        }
    }
    return $out;
}

// Returns array of entries: ['alias'=>..,'ip'=>..,'macs'=>[..]]
function allowlist_read(string $path): array {
    $entries = [];
    if (!is_file($path)) {
        return $entries;
    }
    $lines = file($path, FILE_IGNORE_NEW_LINES) ?: [];
    foreach ($lines as $line) {
        $line = trim($line);
        if ($line === '' || $line[0] === '#') {
            continue;
        }
        $fields = array_map('trim', explode(',', $line));
        $alias = $fields[0] ?? '';
        $ip = $fields[1] ?? '';
        $macs = array_values(array_filter(array_map('normalize_mac', array_slice($fields, 2)), fn($m) => $m !== ''));
        $entries[] = ['alias' => $alias, 'ip' => $ip, 'macs' => $macs];
    }
    return $entries;
}

// Validate a single entry; returns [] on success or a list of error strings.
function entry_errors(array $e): array {
    $errs = [];
    $alias = trim((string) ($e['alias'] ?? ''));
    $ip = trim((string) ($e['ip'] ?? ''));
    $macs = $e['macs'] ?? [];
    if ($alias === '') {
        $errs[] = 'Alias is required.';
    }
    if (strpos($alias, ',') !== false || strpos($alias, '#') !== false) {
        $errs[] = 'Alias cannot contain "," or "#".';
    }
    if (!is_valid_ip($ip)) {
        $errs[] = "Invalid IP address: " . $ip;
    }
    if (empty($macs)) {
        $errs[] = 'At least one MAC address is required.';
    }
    foreach ($macs as $m) {
        if (!is_valid_mac($m)) {
            $errs[] = "Invalid MAC address: " . $m;
        }
    }
    return $errs;
}

// Validate the whole set (per-entry + unique IPs). Returns list of errors.
function allowlist_errors(array $entries): array {
    $errs = [];
    $seen_ips = [];
    foreach ($entries as $i => $e) {
        foreach (entry_errors($e) as $msg) {
            $errs[] = "Entry #" . ($i + 1) . ": " . $msg;
        }
        $ip = trim((string) ($e['ip'] ?? ''));
        if ($ip !== '') {
            if (isset($seen_ips[$ip])) {
                $errs[] = "Duplicate IP: " . $ip;
            }
            $seen_ips[$ip] = true;
        }
    }
    return $errs;
}

// Serialize entries back to the CSV format the server expects.
function allowlist_render(array $entries): string {
    $out = "# Managed by the guardrail admin app. Format: alias,ip,mac1[,mac2...]\n";
    foreach ($entries as $e) {
        $cols = array_merge([$e['alias'], $e['ip']], $e['macs']);
        $out .= implode(',', $cols) . "\n";
    }
    return $out;
}

// Validate then atomically write. Returns [] on success or a list of errors.
function allowlist_save(string $path, array $entries): array {
    $errs = allowlist_errors($entries);
    if (!empty($errs)) {
        return $errs;
    }
    if (!atomic_write($path, allowlist_render($entries))) {
        return ['Failed to write the access-allow file. Check permissions on ' . $path];
    }
    return [];
}

// Add (or merge into) an access-allow entry for an IP + MAC(s). Merges MACs when
// the IP already exists. Returns [] on success or a list of error strings
// (e.g. when no MAC is available, which the server requires for a match).
function allowlist_add_host(string $ip, string $alias, array $macs): array {
    if (!is_valid_ip($ip)) {
        return ['Cannot add to the allow-list: the account has no valid IP (' . ($ip !== '' ? $ip : 'none') . ').'];
    }
    $macs = array_values(array_filter(array_map(function ($m) { return normalize_mac(str_replace('-', ':', (string) $m)); }, $macs), 'is_valid_mac'));
    if (empty($macs)) {
        return ['Account approved, but not added to the allow-list: no MAC was captured (only same-LAN signups have one). Add the host manually under Users if needed.'];
    }
    $alias = trim(str_replace([',', '#'], ' ', $alias));
    if ($alias === '') { $alias = $ip; }
    $entries = allowlist_read(ACCESS_ALLOW_FILE);
    $found = false;
    foreach ($entries as &$e) {
        if (($e['ip'] ?? '') === $ip) {
            $e['macs'] = array_values(array_unique(array_merge($e['macs'], $macs)));
            if (trim((string) ($e['alias'] ?? '')) === '') { $e['alias'] = $alias; }
            $found = true;
        }
    }
    unset($e);
    if (!$found) {
        $entries[] = ['alias' => $alias, 'ip' => $ip, 'macs' => $macs];
    }
    return allowlist_save(ACCESS_ALLOW_FILE, $entries);
}

// Is a client host allowed to use the web chat? Mirrors the server's access
// control but is enforced by the proxy itself (chat requests otherwise reach
// llama-server over loopback, which always bypasses the list).
//   - loopback always allowed;
//   - an IP in the access-allow list is allowed (MAC verified when resolvable,
//     lenient for routed/VPN clients where no MAC can be resolved);
//   - otherwise, the IP of an approved (active) chat account is allowed — this is
//     how VPN/routed hosts (which cannot carry a MAC) get authorized.
function host_allowed(string $ip): bool {
    if ($ip === '' || $ip === '127.0.0.1' || $ip === '::1') { return true; }
    foreach (allowlist_read(ACCESS_ALLOW_FILE) as $e) {
        if (($e['ip'] ?? '') === $ip) {
            $macs = $e['macs'] ?? [];
            if (empty($macs)) { return true; }
            $cur = resolve_mac($ip);
            if ($cur === '') { return true; }              // can't resolve a MAC (routed/VPN) → IP match suffices
            return in_array($cur, $macs, true);            // on the LAN, require the MAC to match
        }
    }
    foreach (accounts_read() as $u) {
        if (($u['status'] ?? '') === 'active' && trim((string) ($u['ip'] ?? '')) === $ip) { return true; }
    }
    return false;
}

// Remove the access-allow entry for an IP (if present). Returns true if one was
// removed. Used when an account is suspended so its host loses direct API access.
function allowlist_remove_host(string $ip): bool {
    if ($ip === '') { return false; }
    $entries = allowlist_read(ACCESS_ALLOW_FILE);
    $kept = array_values(array_filter($entries, fn($e) => ($e['ip'] ?? '') !== $ip));
    if (count($kept) === count($entries)) { return false; }   // nothing matched
    return empty(allowlist_save(ACCESS_ALLOW_FILE, $kept));
}

// ---------------------------------------------------------------------------
// Logs
// ---------------------------------------------------------------------------

// Available log files, newest first: ['name'=>.., 'path'=>.., 'size'=>..].
function log_files(string $dir): array {
    $files = [];
    foreach (glob(rtrim($dir, '/\\') . DIRECTORY_SEPARATOR . 'llama-server-*.jsonl') ?: [] as $p) {
        $files[] = ['name' => basename($p), 'path' => $p, 'size' => (int) @filesize($p)];
    }
    usort($files, fn($a, $b) => strcmp($b['name'], $a['name']));
    return $files;
}

// Read the last $maxLines decoded records from a jsonl file (O(N), bounded memory).
function read_log_tail(string $path, int $maxLines): array {
    if (!is_file($path) || $maxLines <= 0) {
        return [];
    }
    $fp = @fopen($path, 'r');
    if (!$fp) {
        return [];
    }
    $ring = [];
    $count = 0;
    while (($line = fgets($fp)) !== false) {
        $line = trim($line);
        if ($line === '') {
            continue;
        }
        $ring[$count % $maxLines] = $line;
        $count++;
    }
    fclose($fp);
    $n = min($count, $maxLines);
    $start = $count - $n;
    $out = [];
    for ($i = $start; $i < $count; $i++) {
        $rec = json_decode($ring[$i % $maxLines], true);
        if (is_array($rec)) {
            $out[] = $rec;
        }
    }
    return array_reverse($out); // newest first
}

// ---------------------------------------------------------------------------
// Log backups: zip old log files (via PowerShell Compress-Archive, since this
// PHP lacks the zip extension) and remove the originals to reclaim space.
// ---------------------------------------------------------------------------

// Existing backup zips, newest first.
function logs_backups_list(): array {
    $out = [];
    foreach (glob(LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . '*.zip') ?: [] as $p) {
        $out[] = ['name' => basename($p), 'size' => (int) @filesize($p), 'when' => date('Y-m-d H:i', (int) @filemtime($p))];
    }
    usort($out, fn($a, $b) => strcmp($b['name'], $a['name']));
    return $out;
}

// Zip every log file dated before $before (Y-m-d) and delete the originals.
// Today's file is kept (llama-server has it open). Returns ['ok'=>..] or ['error'=>..].
function logs_backup(string $before): array {
    $files = [];
    foreach (log_files(LOG_DIR) as $f) {
        $d = log_date_from_name($f['name']);
        if ($d !== '' && $d < $before) { $files[] = $f; }
    }
    if (empty($files)) { return ['ok' => true, 'count' => 0, 'note' => 'No logs older than ' . $before . ' to back up.']; }
    if (!function_exists('shell_exec')) { return ['error' => 'shell_exec is disabled; cannot create the zip.']; }
    if (!is_dir(LOG_BACKUP_DIR) && !@mkdir(LOG_BACKUP_DIR, 0700, true) && !is_dir(LOG_BACKUP_DIR)) { return ['error' => 'Cannot create the backup directory.']; }
    $zipName = 'logs-backup-' . date('Ymd-His') . '.zip';
    $zipPath = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . $zipName;
    $list = implode(',', array_map(fn($f) => ps_quote($f['path']), $files));
    $ps1 = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . '.zip.ps1';
    $script = '$ErrorActionPreference = ' . ps_quote('Stop') . "\r\n"
        . 'try {' . "\r\n"
        . '  Compress-Archive -LiteralPath ' . $list . ' -DestinationPath ' . ps_quote($zipPath) . ' -Force -CompressionLevel Optimal' . "\r\n"
        . '  [Console]::Out.Write(' . ps_quote('OK') . ')' . "\r\n"
        . '} catch { [Console]::Out.Write(' . ps_quote('ERR:') . ' + $_.Exception.Message) }' . "\r\n";
    if (@file_put_contents($ps1, $script) === false) { return ['error' => 'Cannot write the backup script.']; }
    $out = trim((string) @shell_exec('powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' . escapeshellarg($ps1) . ' 2>NUL'));
    @unlink($ps1);
    if (strncmp($out, 'ERR:', 4) === 0) { return ['error' => 'Zip failed: ' . substr($out, 4)]; }
    if (!is_file($zipPath) || filesize($zipPath) === 0) { return ['error' => 'The backup zip was not created.']; }
    $removed = 0;
    foreach ($files as $f) { if (@unlink($f['path'])) { $removed++; } }
    return ['ok' => true, 'zip' => $zipName, 'count' => $removed, 'size' => (int) filesize($zipPath)];
}

// Like logs_backup() but fire-and-forget: launches the zip in a detached
// background process and returns at once, so it never adds latency to the
// caller (the monthly auto-backup runs on prompt requests). The background
// worker deletes the originals ONLY after it has verified a non-empty zip —
// same safety as logs_backup() — and appends the outcome to .auto-backup.log.
function logs_backup_detached(string $before): array {
    $files = [];
    foreach (log_files(LOG_DIR) as $f) {
        $d = log_date_from_name($f['name']);
        if ($d !== '' && $d < $before) { $files[] = $f; }
    }
    if (empty($files)) { return ['ok' => true, 'count' => 0, 'note' => 'No logs older than ' . $before . ' to back up.']; }
    if (!function_exists('shell_exec')) { return ['error' => 'shell_exec is disabled; cannot create the zip.']; }
    if (!is_dir(LOG_BACKUP_DIR) && !@mkdir(LOG_BACKUP_DIR, 0700, true) && !is_dir(LOG_BACKUP_DIR)) { return ['error' => 'Cannot create the backup directory.']; }
    $zipName = 'logs-backup-' . date('Ymd-His') . '.zip';
    $zipPath = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . $zipName;
    $runLog  = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . '.auto-backup.log';
    $worker  = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . '.auto-zip.ps1';
    $n = count($files);
    $list = implode(',', array_map(fn($f) => ps_quote($f['path']), $files));
    $qzip = ps_quote($zipPath);
    $qlog = ps_quote($runLog);
    // Worker script (runs after PHP has already returned): zip → verify → delete.
    $ws = '$ts = (Get-Date).ToString(' . ps_quote('yyyy-MM-dd HH:mm:ss') . ')' . "\r\n"
        . 'try {' . "\r\n"
        . '  Compress-Archive -LiteralPath ' . $list . ' -DestinationPath ' . $qzip . ' -Force -CompressionLevel Optimal' . "\r\n"
        . '  if ((Test-Path -LiteralPath ' . $qzip . ') -and ((Get-Item -LiteralPath ' . $qzip . ').Length -gt 0)) {' . "\r\n"
        . '    Remove-Item -LiteralPath ' . $list . ' -Force' . "\r\n"
        . '    Add-Content -LiteralPath ' . $qlog . ' -Value ($ts + ' . ps_quote('  OK  ' . $zipName . '  (' . $n . ' files, ') . ' + (Get-Item -LiteralPath ' . $qzip . ').Length + ' . ps_quote(' bytes)') . ')' . "\r\n"
        . '  } else {' . "\r\n"
        . '    Add-Content -LiteralPath ' . $qlog . ' -Value ($ts + ' . ps_quote('  ERR  zip missing or empty; originals kept') . ')' . "\r\n"
        . '  }' . "\r\n"
        . '} catch {' . "\r\n"
        . '  Add-Content -LiteralPath ' . $qlog . ' -Value ($ts + ' . ps_quote('  ERR  ') . ' + $_.Exception.Message + ' . ps_quote('; originals kept') . ')' . "\r\n"
        . '}' . "\r\n";
    if (@file_put_contents($worker, $ws) === false) { return ['error' => 'Cannot write the backup worker script.']; }
    // Launch the worker detached via WMI Create (no inherited handles → returns at once).
    $cmdline = 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' . $worker . '"';
    $launcher = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . '.auto-launch.ps1';
    $ls = '$ErrorActionPreference = ' . ps_quote('Stop') . "\r\n"
        . 'try {' . "\r\n"
        . '  $r = ([wmiclass]' . ps_quote('Win32_Process') . ').Create(' . ps_quote($cmdline) . ')' . "\r\n"
        . '  if ($r.ReturnValue -eq 0) { [Console]::Out.Write($r.ProcessId) } else { [Console]::Out.Write(' . ps_quote('ERR:Win32_Process.Create failed, code ') . ' + $r.ReturnValue) }' . "\r\n"
        . '} catch { [Console]::Out.Write(' . ps_quote('ERR:') . ' + $_.Exception.Message) }' . "\r\n";
    if (@file_put_contents($launcher, $ls) === false) { return ['error' => 'Cannot write the backup launcher script.']; }
    $out = trim((string) @shell_exec('powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' . escapeshellarg($launcher) . ' 2>NUL'));
    @unlink($launcher);
    if (strncmp($out, 'ERR:', 4) === 0) { return ['error' => 'Background launch failed: ' . substr($out, 4)]; }
    $pid = (int) $out;
    if ($pid <= 0) { return ['error' => 'Background launch failed (no PID returned).']; }
    return ['ok' => true, 'started' => true, 'zip' => $zipName, 'count' => $n, 'pid' => $pid];
}

// Run once per calendar month: archive logs from before the current month.
// Cheap to call on every request — the fast path is a single file read, and the
// archive itself runs in the background so a prompt request is never blocked.
function logs_backup_auto(): ?array {
    if (!LOG_AUTO_BACKUP_MONTHLY) { return null; }
    $stateFile = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . '.last-auto.txt';
    $month = date('Y-m');
    // fast path: already archived this month → nothing to do
    if (is_file($stateFile) && trim((string) @file_get_contents($stateFile)) === $month) { return null; }
    if (!is_dir(LOG_BACKUP_DIR) && !@mkdir(LOG_BACKUP_DIR, 0700, true) && !is_dir(LOG_BACKUP_DIR)) { return null; }
    @file_put_contents($stateFile, $month);                // claim the month first, so concurrent prompts don't double-launch
    $r = logs_backup_detached(date('Y-m-01'));             // fire-and-forget; archives completed months in the background
    if (!empty($r['error'])) { @file_put_contents($stateFile, ''); }  // spawn failed → clear claim so it retries
    return $r;
}

// Read the JSONL request records out of a backup zip, newest-first, capped at
// $maxLines. This PHP has no zip extension, so PowerShell + System.IO.Compression
// dumps the entries (newest date first, newest line first within each) to a temp
// file, which we then parse. Returns records array, or null if the zip is absent.
function logs_backup_read(string $zipName, int $maxLines): ?array {
    if (!preg_match('/^[\w.\-]+\.zip$/', $zipName)) { return null; }
    $zipPath = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . $zipName;
    if (!is_file($zipPath)) { return null; }
    if (!function_exists('shell_exec')) { return []; }
    $tmp = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . '.view-' . date('Ymd-His') . '-' . substr(md5($zipName), 0, 6) . '.jsonl';
    $ps1 = LOG_BACKUP_DIR . DIRECTORY_SEPARATOR . '.view.ps1';
    $script = '$ErrorActionPreference = ' . ps_quote('Stop') . "\r\n"
        . 'try {' . "\r\n"
        . '  Add-Type -AssemblyName System.IO.Compression.FileSystem' . "\r\n"
        . '  $zip = [System.IO.Compression.ZipFile]::OpenRead(' . ps_quote($zipPath) . ')' . "\r\n"
        . '  $sw = New-Object System.IO.StreamWriter(' . ps_quote($tmp) . ', $false, (New-Object System.Text.UTF8Encoding($false)))' . "\r\n"
        . '  foreach ($e in ($zip.Entries | Sort-Object Name -Descending)) {' . "\r\n"
        . '    $rd = New-Object System.IO.StreamReader($e.Open())' . "\r\n"
        . '    $text = $rd.ReadToEnd(); $rd.Close()' . "\r\n"
        . '    $lines = $text -split ' . ps_quote('\r?\n') . "\r\n"
        . '    for ($i = $lines.Length - 1; $i -ge 0; $i--) { if ($lines[$i].Trim().Length -gt 0) { $sw.WriteLine($lines[$i]) } }' . "\r\n"
        . '  }' . "\r\n"
        . '  $sw.Close(); $zip.Dispose(); [Console]::Out.Write(' . ps_quote('OK') . ')' . "\r\n"
        . '} catch { [Console]::Out.Write(' . ps_quote('ERR:') . ' + $_.Exception.Message) }' . "\r\n";
    if (@file_put_contents($ps1, $script) === false) { return []; }
    $out = trim((string) @shell_exec('powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' . escapeshellarg($ps1) . ' 2>NUL'));
    @unlink($ps1);
    $recs = [];
    if (strncmp($out, 'ERR:', 4) !== 0 && is_file($tmp)) {
        $fp = @fopen($tmp, 'r');
        if ($fp) {
            while (($line = fgets($fp)) !== false && count($recs) < $maxLines) {
                if (substr($line, 0, 3) === "\xEF\xBB\xBF") { $line = substr($line, 3); }  // strip a stray UTF-8 BOM
                $line = trim($line);
                if ($line === '') { continue; }
                $rec = json_decode($line, true);
                if (is_array($rec)) { $recs[] = $rec; }
            }
            fclose($fp);
        }
    }
    @unlink($tmp);
    return $recs;
}

// Extract the YYYY-MM-DD date encoded in a log file name.
function log_date_from_name(string $name): string {
    return preg_match('/(\d{4}-\d{2}-\d{2})/', $name, $m) ? $m[1] : '';
}

// Read up to $maxLines newest records across all log files whose date is within
// [$from, $to] (inclusive; either may be ''). Each record gets a '_file' key.
function read_logs_range(string $dir, string $from, string $to, int $maxLines): array {
    $out = [];
    foreach (log_files($dir) as $f) {            // newest file first
        if (count($out) >= $maxLines) {
            break;
        }
        $d = log_date_from_name($f['name']);
        if ($d === '' || ($from !== '' && $d < $from) || ($to !== '' && $d > $to)) {
            continue;
        }
        foreach (read_log_tail($f['path'], $maxLines - count($out)) as $r) {
            $r['_file'] = $f['name'];
            $out[] = $r;
        }
    }
    return $out; // newest-first overall
}

// Turn one raw log record into a display row for the Request-logs table.
// Shared by the live logs endpoint and the backup-browse endpoint so both
// render identically.
function log_record_to_row(array $r, array $map, array $acctMap, array $idMap, array $acctIpIdMap = []): array {
    $ts = (string) ($r['timestamp'] ?? '');
    $remote = (string) ($r['remote_addr'] ?? '');
    $reqb = (string) ($r['request_body'] ?? '');
    $resb = (string) ($r['response_body'] ?? '');
    $prompt = extract_prompt($reqb);
    $status = (int) ($r['status'] ?? 0);
    $uid = request_user_id($reqb);
    $suspendId = ($uid !== '' && $uid !== '__admin__' && isset($idMap[$uid])) ? $uid : '';
    // Fall back to the account owning this IP, so IP-identified rows are suspendable too.
    if ($suspendId === '' && $remote !== '' && isset($acctIpIdMap[$remote])) { $suspendId = $acctIpIdMap[$remote]; }
    return [
        'sig'     => substr(md5($ts . ($r['method'] ?? '') . ($r['path'] ?? '') . $status . $reqb), 0, 12),
        'ts'      => $ts,
        'user'    => log_user_label($reqb, $remote, $map, $acctMap, $idMap),
        'userId'  => $suspendId,
        'remote'  => $remote,
        'method'  => (string) ($r['method'] ?? ''),
        'path'    => ($r['path'] ?? '') . ((($r['query_string'] ?? '') !== '') ? '?' . $r['query_string'] : ''),
        'status'  => $status,
        'prompt'  => $prompt,
        'hasPrompt' => $prompt !== '',
        'latency' => format_latency(extract_latency_ms($resb)),
        'conversation' => build_conversation($reqb, $resb),
        'requestRaw'   => truncate($reqb, 20000),
        'responseRaw'  => truncate($resb, 20000),
    ];
}

// Requests per day (newest $maxDays days), chronological [date => count].
function log_perday_counts(string $dir, int $maxDays = 14): array {
    $out = [];
    foreach (log_files($dir) as $f) {
        $d = log_date_from_name($f['name']);
        if ($d === '') {
            continue;
        }
        $out[$d] = $f['size'] > 0 ? substr_count((string) @file_get_contents($f['path']), "\n") : 0;
        if (count($out) >= $maxDays) {
            break;
        }
    }
    ksort($out);
    return $out;
}

// Requests per day for an explicit [from,to] date range (inclusive), chronological.
// Days with no log file are zero. Capped at 366 days to bound the loop.
function perday_counts_range(string $dir, string $from, string $to): array {
    $out = [];
    $d = strtotime($from); $end = strtotime($to); $n = 0;
    while ($d !== false && $end !== false && $d <= $end && $n < 366) {
        $date = date('Y-m-d', $d);
        $f = rtrim($dir, '/\\') . DIRECTORY_SEPARATOR . "llama-server-$date.jsonl";
        $out[$date] = is_file($f) ? substr_count((string) @file_get_contents($f), "\n") : 0;
        $d = strtotime('+1 day', $d); $n++;
    }
    return $out;
}

// Status-class breakdown over the newest $cap records.
function log_status_breakdown(string $dir, int $cap = 5000): array {
    $b = ['2xx' => 0, '4xx' => 0, '5xx' => 0, 'other' => 0];
    foreach (read_logs_range($dir, '', '', $cap) as $r) {
        $s = (int) ($r['status'] ?? 0);
        if ($s >= 200 && $s < 300) { $b['2xx']++; }
        elseif ($s >= 400 && $s < 500) { $b['4xx']++; }
        elseif ($s >= 500) { $b['5xx']++; }
        else { $b['other']++; }
    }
    return $b;
}

// Inline SVG bar chart from [label => value] (no external library).
function svg_bar(array $data): string {
    if (!$data) { return '<p class="muted">No data yet.</p>'; }
    $W = 560; $H = 200; $padL = 8; $padR = 8; $padB = 22; $padT = 16;
    $n = count($data); $max = max(1, max($data));
    $plotH = $H - $padT - $padB; $bw = ($W - $padL - $padR) / $n;
    $svg = ''; $i = 0;
    foreach ($data as $label => $v) {
        $bh = $plotH * ($v / $max);
        $x = $padL + $i * $bw + $bw * 0.15; $wid = $bw * 0.7; $y = $padT + $plotH - $bh;
        $svg .= '<rect class="bar" x="' . round($x, 1) . '" y="' . round($y, 1) . '" width="' . round($wid, 1) . '" height="' . round($bh, 1) . '" rx="3" style="fill:var(--accent)" data-tip="' . h($label . ': ' . $v) . '"/>';
        if ($v > 0) { $svg .= '<text x="' . round($x + $wid / 2, 1) . '" y="' . round($y - 4, 1) . '" text-anchor="middle" class="cval">' . $v . '</text>'; }
        $svg .= '<text x="' . round($x + $wid / 2, 1) . '" y="' . ($H - $padB + 14) . '" text-anchor="middle" class="cax">' . h(substr($label, 5)) . '</text>';
        $i++;
    }
    return '<svg viewBox="0 0 ' . $W . ' ' . $H . '" class="chart" preserveAspectRatio="xMidYMid meet">' . $svg . '</svg>';
}

// Inline SVG pie chart from [label => value] with [label => color] colors.
function svg_pie(array $data, array $colors): string {
    $total = array_sum($data);
    if ($total <= 0) { return '<p class="muted">No data yet.</p>'; }
    $cx = 90; $cy = 90; $r = 82; $ang = -M_PI / 2; $svg = '';
    foreach ($data as $label => $v) {
        if ($v <= 0) { continue; }
        $frac = $v / $total; $col = $colors[$label] ?? '#888';
        $tip = h($label . ': ' . number_format($v) . ' (' . round($frac * 100) . '%)');
        if ($frac >= 0.9999) {
            $svg .= '<circle class="slice" cx="' . $cx . '" cy="' . $cy . '" r="' . $r . '" style="fill:' . $col . '" data-tip="' . $tip . '"/>';
            continue;
        }
        $a2 = $ang + $frac * 2 * M_PI;
        $x1 = $cx + $r * cos($ang); $y1 = $cy + $r * sin($ang);
        $x2 = $cx + $r * cos($a2); $y2 = $cy + $r * sin($a2);
        $large = $frac > 0.5 ? 1 : 0;
        $svg .= '<path class="slice" d="M' . $cx . ' ' . $cy . ' L' . round($x1, 2) . ' ' . round($y1, 2) . ' A' . $r . ' ' . $r . ' 0 ' . $large . ' 1 ' . round($x2, 2) . ' ' . round($y2, 2) . ' Z" style="fill:' . $col . '" data-tip="' . $tip . '"/>';
        $ang = $a2;
    }
    // Donut hole + total in the centre.
    $svg .= '<circle cx="' . $cx . '" cy="' . $cy . '" r="47" style="fill:var(--panel)"/>';
    $svg .= '<text x="' . $cx . '" y="' . ($cy - 1) . '" text-anchor="middle" class="donut-center">' . number_format($total) . '</text>';
    $svg .= '<text x="' . $cx . '" y="' . ($cy + 14) . '" text-anchor="middle" class="donut-sub">total</text>';
    return '<svg viewBox="0 0 180 180" class="chart pie">' . $svg . '</svg>';
}

// Per-user usage aggregates over the newest $cap records, sorted by requests.
// Each entry: user, ip, requests, prompts, denied (4xx), errors (5xx),
// avg_lat (ms|null), first, last (timestamps).
function usage_by_user(string $dir, array $alias_map, int $cap = 20000, string $from = '', string $to = '', array $acct_map = [], array $acct_id_map = []): array {
    $u = [];
    foreach (read_logs_range($dir, $from, $to, $cap) as $r) {
        $remote = (string) ($r['remote_addr'] ?? '');
        $key = log_user_label((string) ($r['request_body'] ?? ''), $remote, $alias_map, $acct_map, $acct_id_map);
        if (!isset($u[$key])) {
            $u[$key] = ['user' => $key, 'ip' => $remote, 'requests' => 0, 'prompts' => 0,
                        'denied' => 0, 'errors' => 0, 'lat_sum' => 0.0, 'lat_n' => 0, 'first' => '', 'last' => ''];
        }
        $u[$key]['requests']++;
        if ($remote !== '') { $u[$key]['ip'] = $remote; }
        $s = (int) ($r['status'] ?? 0);
        if ($s >= 400 && $s < 500) { $u[$key]['denied']++; }
        elseif ($s >= 500) { $u[$key]['errors']++; }
        if (extract_prompt((string) ($r['request_body'] ?? '')) !== '') { $u[$key]['prompts']++; }
        $lat = extract_latency_ms((string) ($r['response_body'] ?? ''));
        if ($lat !== null) { $u[$key]['lat_sum'] += $lat; $u[$key]['lat_n']++; }
        $ts = (string) ($r['timestamp'] ?? '');
        if ($ts !== '') {
            if ($u[$key]['first'] === '' || $ts < $u[$key]['first']) { $u[$key]['first'] = $ts; }
            if ($ts > $u[$key]['last']) { $u[$key]['last'] = $ts; }
        }
    }
    foreach ($u as $k => $e) {
        $u[$k]['avg_lat'] = $e['lat_n'] ? $e['lat_sum'] / $e['lat_n'] : null;
    }
    uasort($u, fn($a, $b) => $b['requests'] <=> $a['requests']);
    return $u;
}

// Prompt counts bucketed by hour ('hour' => 24 buckets "00".."23") or by day
// ('day' => one bucket per date in [from,to]). Buckets are pre-filled with 0.
function prompt_timeseries(string $dir, string $from, string $to, string $bucket, int $cap = 20000): array {
    $series = [];
    if ($bucket === 'hour') {
        for ($hh = 0; $hh < 24; $hh++) { $series[sprintf('%02d', $hh)] = 0; }
    } elseif ($from !== '' && $to !== '') {
        for ($d = strtotime($from); $d !== false && $d <= strtotime($to); $d = strtotime('+1 day', $d)) {
            $series[date('Y-m-d', $d)] = 0;
        }
    }
    foreach (read_logs_range($dir, $from, $to, $cap) as $r) {
        if (extract_prompt((string) ($r['request_body'] ?? '')) === '') { continue; }
        $ts = (string) ($r['timestamp'] ?? '');
        if ($ts === '') { continue; }
        $key = $bucket === 'hour' ? substr($ts, 11, 2) : substr($ts, 0, 10);
        if (isset($series[$key])) { $series[$key]++; }
        else { $series[$key] = 1; }
    }
    if ($bucket === 'day') { ksort($series); }
    return $series;
}

// "Linked bars": a histogram with the bar tops joined by a line + dots.
function svg_timebars(array $data, string $xkind): string {
    if (!$data) { return '<p class="muted">No data.</p>'; }
    $W = 600; $H = 190; $padL = 8; $padR = 8; $padB = 22; $padT = 12;
    $n = count($data); $max = max(1, max($data));
    $plotH = $H - $padT - $padB; $bw = ($W - $padL - $padR) / $n;
    $bars = ''; $labels = ''; $pts = [];
    $labelEvery = $n > 16 ? (int) ceil($n / 12) : 1;
    $i = 0;
    foreach ($data as $label => $v) {
        $bh = $plotH * ($v / $max);
        $x = $padL + $i * $bw; $cx = $x + $bw / 2; $y = $padT + $plotH - $bh;
        $bars .= '<rect class="tb" x="' . round($x + $bw * 0.12, 1) . '" y="' . round($y, 1) . '" width="' . round($bw * 0.76, 1) . '" height="' . round($bh, 1) . '" rx="2" style="fill:var(--accent);opacity:.4" data-tip="' . h($label . ': ' . $v) . '"/>';
        $pts[] = round($cx, 1) . ',' . round($y, 1);
        if ($i % $labelEvery === 0) {
            $xl = $xkind === 'hour' ? $label . 'h' : substr($label, 5);
            $labels .= '<text x="' . round($cx, 1) . '" y="' . ($H - $padB + 14) . '" text-anchor="middle" class="cax">' . h($xl) . '</text>';
        }
        $i++;
    }
    $line = '<polyline points="' . implode(' ', $pts) . '" fill="none" style="stroke:var(--accent)" stroke-width="2"/>';
    $dots = '';
    foreach ($pts as $p) { [$px, $py] = explode(',', $p); $dots .= '<circle cx="' . $px . '" cy="' . $py . '" r="2.4" style="fill:var(--accent)"/>'; }
    return '<svg viewBox="0 0 ' . $W . ' ' . $H . '" class="chart" preserveAspectRatio="xMidYMid meet">' . $bars . $line . $dots . $labels . '</svg>';
}

// ---------------------------------------------------------------------------
// Mapping requests to known hosts and extracting the prompt
// ---------------------------------------------------------------------------

// IP => alias map from the access-allow list.
function allowlist_ip_alias_map(string $path): array {
    $map = [];
    foreach (allowlist_read($path) as $e) {
        if ($e['ip'] !== '') {
            $map[$e['ip']] = $e['alias'];
        }
    }
    return $map;
}

function alias_for_ip(string $ip, array $map): string {
    if ($ip === '127.0.0.1' || $ip === '::1') {
        return 'loopback';
    }
    return $map[$ip] ?? '';
}

// Best label for a request's IP: a matching chat account's display name first,
// then the allow-list alias (or 'loopback'), then the raw IP, then 'unknown'.
function user_label_for_ip(string $ip, array $alias_map, array $acct_map): string {
    if ($ip !== '' && isset($acct_map[$ip]) && $acct_map[$ip] !== '') { return $acct_map[$ip]; }
    $alias = alias_for_ip($ip, $alias_map);
    if ($alias !== '') { return $alias; }
    return $ip !== '' ? $ip : 'unknown';
}

// The account id the web proxy tagged onto a request (OpenAI 'user' field), or ''.
function request_user_id(string $reqb): string {
    if ($reqb === '' || strpos($reqb, '"user"') === false) { return ''; }
    $j = json_decode($reqb, true);
    return (is_array($j) && isset($j['user']) && is_string($j['user'])) ? $j['user'] : '';
}

// Best label for a whole log entry. Prefers the account the web proxy tagged onto
// the request (OpenAI 'user' field) — this is how chat traffic (all loopback) is
// attributed to a real person — then falls back to the IP-based label.
function log_user_label(string $reqb, string $ip, array $alias_map, array $acct_ip_map, array $acct_id_map): string {
    if ($reqb !== '' && strpos($reqb, '"user"') !== false) {
        $j = json_decode($reqb, true);
        if (is_array($j) && isset($j['user']) && is_string($j['user']) && $j['user'] !== '') {
            $uid = $j['user'];
            if ($uid === '__admin__') { return 'admin'; }
            return $acct_id_map[$uid] ?? $uid;
        }
    }
    return user_label_for_ip($ip, $alias_map, $acct_ip_map);
}

// Best-effort prompt extraction from a request body (completion / chat / embeddings).
function extract_prompt(string $body): string {
    if ($body === '') {
        return '';
    }
    $j = json_decode($body, true);
    if (!is_array($j)) {
        return '';
    }
    if (isset($j['prompt'])) {
        return is_string($j['prompt']) ? $j['prompt'] : json_encode($j['prompt']);
    }
    if (isset($j['messages']) && is_array($j['messages'])) {
        for ($i = count($j['messages']) - 1; $i >= 0; $i--) {
            $m = $j['messages'][$i];
            if (($m['role'] ?? '') !== 'user') {
                continue;
            }
            $c = $m['content'] ?? '';
            if (is_array($c)) { // OpenAI content parts
                $t = '';
                foreach ($c as $part) {
                    if (($part['type'] ?? '') === 'text') {
                        $t .= $part['text'] ?? '';
                    }
                }
                return $t;
            }
            return is_string($c) ? $c : json_encode($c);
        }
    }
    foreach (['input', 'content'] as $k) {
        if (isset($j[$k])) {
            return is_string($j[$k]) ? $j[$k] : json_encode($j[$k]);
        }
    }
    return '';
}

// Inference latency in milliseconds, summed from the response timings
// (prompt eval + generation). Works on both plain JSON and streamed SSE bodies
// by taking the last timings block. Returns null when no timings are present.
function extract_latency_ms(string $body): ?float {
    if ($body === '') {
        return null;
    }
    $sum = 0.0;
    $found = false;
    foreach (['prompt_ms', 'predicted_ms'] as $key) {
        if (preg_match_all('/"' . $key . '":\s*([0-9.]+)/', $body, $m) && $m[1]) {
            $sum += (float) end($m[1]);
            $found = true;
        }
    }
    return $found ? $sum : null;
}

function format_latency(?float $ms): string {
    if ($ms === null) {
        return '';
    }
    return $ms >= 1000 ? number_format($ms / 1000, 2) . 's' : number_format($ms, 0) . 'ms';
}

// ---------------------------------------------------------------------------
// Reconstruct the conversation (for the detail popup), like the web UI shows it
// ---------------------------------------------------------------------------

function msg_content_to_text($c): string {
    if (is_string($c)) {
        return $c;
    }
    if (is_array($c)) { // OpenAI content parts
        $t = '';
        foreach ($c as $part) {
            if (is_array($part) && ($part['type'] ?? '') === 'text') {
                $t .= $part['text'] ?? '';
            }
        }
        return $t;
    }
    return '';
}

// The request's messages (chat) or prompt (completion) as [{role, content}].
function extract_request_messages(string $body): array {
    $j = json_decode($body, true);
    if (!is_array($j)) {
        return [];
    }
    $out = [];
    if (isset($j['messages']) && is_array($j['messages'])) {
        foreach ($j['messages'] as $m) {
            $out[] = ['role' => (string) ($m['role'] ?? 'user'), 'content' => msg_content_to_text($m['content'] ?? '')];
        }
    } elseif (isset($j['prompt'])) {
        $out[] = ['role' => 'user', 'content' => is_string($j['prompt']) ? $j['prompt'] : json_encode($j['prompt'])];
    }
    return $out;
}

function response_content_from_obj(array $j): string {
    if (isset($j['choices'][0]['message']['content'])) {
        return (string) $j['choices'][0]['message']['content'];
    }
    if (isset($j['choices'][0]['text'])) {
        return (string) $j['choices'][0]['text'];
    }
    if (isset($j['content'])) {
        return (string) $j['content'];
    }
    return '';
}

// Final assistant text, assembling streamed (SSE) deltas when needed.
function assemble_response_content(string $body): string {
    $body = trim($body);
    if ($body === '') {
        return '';
    }
    $j = json_decode($body, true);
    if (is_array($j)) {
        return response_content_from_obj($j);
    }
    $content = '';
    foreach (preg_split('/\r?\n/', $body) as $line) {
        $line = trim($line);
        if (strncmp($line, 'data:', 5) !== 0) {
            continue;
        }
        $payload = trim(substr($line, 5));
        if ($payload === '' || $payload === '[DONE]') {
            continue;
        }
        $o = json_decode($payload, true);
        if (!is_array($o)) {
            continue;
        }
        if (isset($o['choices'][0]['delta']['content'])) {
            $content .= $o['choices'][0]['delta']['content'];
        } elseif (isset($o['choices'][0]['text'])) {
            $content .= $o['choices'][0]['text'];
        } elseif (isset($o['content'])) {
            $content .= $o['content'];
        }
    }
    return $content;
}

// Full conversation = request messages + the assembled assistant reply.
function build_conversation(string $reqb, string $resb): array {
    $conv = extract_request_messages($reqb);
    $ans = assemble_response_content($resb);
    if ($ans !== '') {
        $conv[] = ['role' => 'assistant', 'content' => $ans];
    }
    return $conv;
}

// ---------------------------------------------------------------------------
// Server status (HTTP probe)
// ---------------------------------------------------------------------------

function http_probe(string $url, float $timeout = 1.5): array {
    if (function_exists('curl_init')) {
        $ch = curl_init($url);
        curl_setopt_array($ch, [
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_TIMEOUT_MS     => (int) ($timeout * 1000),
            CURLOPT_CONNECTTIMEOUT => 2,
        ]);
        $body = curl_exec($ch);
        $code = (int) curl_getinfo($ch, CURLINFO_HTTP_CODE);
        curl_close($ch);
        return ['code' => $code, 'body' => $body === false ? '' : $body];
    }
    $ctx = stream_context_create(['http' => ['timeout' => $timeout, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $code = 0;
    if (isset($http_response_header) && is_array($http_response_header)) {
        foreach ($http_response_header as $hdr) {
            if (preg_match('#^HTTP/\S+\s+(\d{3})#', $hdr, $m)) {
                $code = (int) $m[1];
            }
        }
    }
    return ['code' => $code, 'body' => $body === false ? '' : $body];
}

// Returns ['status'=>'online|loading|offline', 'code'=>int, 'model'=>string, 'url'=>string].
function server_health(): array {
    $base = rtrim(SERVER_URL, '/');
    $r = http_probe($base . '/health');
    $status = $r['code'] === 200 ? 'online' : ($r['code'] === 503 ? 'loading' : 'offline');
    $model = '';
    if ($status === 'online') {
        $p = http_probe($base . '/props');
        if ($p['code'] === 200) {
            $j = json_decode($p['body'], true);
            $raw = $j['model_path'] ?? ($j['default_generation_settings']['model'] ?? '');
            $model = is_string($raw) && $raw !== '' ? basename(str_replace('\\', '/', $raw)) : '';
        }
    }
    return ['status' => $status, 'code' => $r['code'], 'model' => $model, 'url' => $base];
}

// ---------------------------------------------------------------------------
// llama-server process control (start / stop from the dashboard)
//   Safety: stop only ever targets a specific PID (the one we started, or the
//   process currently listening on the configured port) AND only after verifying
//   its image name looks like llama-server. It never does a kill-by-name.
// ---------------------------------------------------------------------------

function server_port(): int {
    return preg_match('/:(\d+)/', SERVER_URL, $m) ? (int) $m[1] : 8080;
}

function default_server_args(): string {
    return '-m <path-to-model.gguf> --host 127.0.0.1 --port ' . server_port()
        . ' --access-allow-file "' . ACCESS_ALLOW_FILE . '"'
        . ' --log-dir "' . LOG_DIR . '"';
}

function server_launch_config(): array {
    $def = ['exe' => '', 'args' => default_server_args()];
    if (!is_file(SERVER_LAUNCH_FILE)) { return $def; }
    $j = json_decode((string) @file_get_contents(SERVER_LAUNCH_FILE), true);
    if (!is_array($j)) { return $def; }
    return ['exe' => (string) ($j['exe'] ?? ''), 'args' => (string) ($j['args'] ?? default_server_args())];
}

function server_launch_config_save(string $exe, string $args): bool {
    return atomic_write(SERVER_LAUNCH_FILE, json_encode(['exe' => $exe, 'args' => $args], JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n");
}

function server_pid_read(): int { return is_file(SERVER_PID_FILE) ? (int) trim((string) @file_get_contents(SERVER_PID_FILE)) : 0; }
function server_pid_write(int $pid): void { @file_put_contents(SERVER_PID_FILE, (string) $pid); }
function server_pid_clear(): void { if (is_file(SERVER_PID_FILE)) { @unlink(SERVER_PID_FILE); } }

// Image name for a PID via tasklist, or '' if not running.
function pid_image(int $pid): string {
    if ($pid <= 0 || !function_exists('shell_exec')) { return ''; }
    $out = trim((string) @shell_exec('tasklist /FI "PID eq ' . $pid . '" /FO CSV /NH 2>NUL'));
    return preg_match('/^"([^"]+)"/', $out, $m) ? $m[1] : '';
}
function pid_running(int $pid): bool { return $pid > 0 && pid_image($pid) !== ''; }

// PID of the process LISTENING on a TCP port, or 0.
function port_pid(int $port): int {
    if (!function_exists('shell_exec')) { return 0; }
    $out = (string) @shell_exec('netstat -ano -p tcp 2>NUL');
    foreach (preg_split('/\r?\n/', $out) as $line) {
        $c = preg_split('/\s+/', trim($line));
        if (count($c) >= 5 && strtoupper($c[3]) === 'LISTENING' && preg_match('/:(\d+)$/', $c[1], $m) && (int) $m[1] === $port) {
            return (int) $c[4];
        }
    }
    return 0;
}

// PowerShell single-quoted literal.
function ps_quote(string $s): string { return "'" . str_replace("'", "''", $s) . "'"; }

// Snapshot for the dashboard: config + whether it is running + PID.
// Prefers the PID actually listening on the port (the real llama-server) for
// display; falls back to our stored launcher PID.
function server_ctl_status(): array {
    $cfg = server_launch_config();
    $stored = server_pid_read();
    $portPid = port_pid(server_port());
    $pid = $portPid > 0 ? $portPid : (pid_running($stored) ? $stored : 0);
    return [
        'exe'     => $cfg['exe'],
        'args'    => $cfg['args'],
        'port'    => server_port(),
        'pid'     => $pid,
        'running' => $pid > 0,
    ];
}

// Launch the configured executable fully detached, capturing its console output.
//
// Uses WMI Win32_Process.Create (not Start-Process): Start-Process with
// -RedirectStandard* forces handle inheritance, so the child inherits PHP's pipe
// and shell_exec blocks until it exits (request hangs on "Working…"). WMI Create
// spawns an independent process with no inherited handles and returns at once.
// The command is wrapped in `cmd /c ... > log 2>&1` so console output is written
// to SERVER_CONSOLE_LOG (which the dashboard tails live). The stored PID is cmd's;
// stopping kills its whole tree, which takes the server with it.
function server_start(): array {
    $cfg = server_launch_config();
    $exe = trim($cfg['exe']);
    if ($exe === '') { return ['ok' => false, 'error' => 'Set the llama-server.exe path first, then Save.']; }
    if (!is_file($exe)) { return ['ok' => false, 'error' => 'Executable not found: ' . $exe]; }
    if (!function_exists('shell_exec')) { return ['ok' => false, 'error' => 'shell_exec is disabled on this PHP.']; }
    $existing = port_pid(server_port());
    if ($existing > 0) { return ['ok' => false, 'error' => 'Port ' . server_port() . ' is already in use (PID ' . $existing . '). Stop it first.']; }
    @file_put_contents(SERVER_CONSOLE_LOG, '');   // fresh console for this launch
    $inner   = '"' . $exe . '" ' . $cfg['args'] . ' > "' . SERVER_CONSOLE_LOG . '" 2>&1';
    $cmdline = 'cmd.exe /c "' . $inner . '"';
    $ps1 = dirname(ACCESS_ALLOW_FILE) . DIRECTORY_SEPARATOR . '.launch.ps1';
    $script = '$ErrorActionPreference = ' . ps_quote('Stop') . "\r\n"
        . 'try {' . "\r\n"
        . '  $r = ([wmiclass]' . ps_quote('Win32_Process') . ').Create(' . ps_quote($cmdline) . ')' . "\r\n"
        . '  if ($r.ReturnValue -eq 0) { [Console]::Out.Write($r.ProcessId) } else { [Console]::Out.Write(' . ps_quote('ERR:Win32_Process.Create failed, code ') . ' + $r.ReturnValue) }' . "\r\n"
        . '} catch { [Console]::Out.Write(' . ps_quote('ERR:') . ' + $_.Exception.Message) }' . "\r\n";
    if (@file_put_contents($ps1, $script) === false) { return ['ok' => false, 'error' => 'Cannot write launch script.']; }
    $out = trim((string) @shell_exec('powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' . escapeshellarg($ps1) . ' 2>NUL'));
    @unlink($ps1);
    if (strncmp($out, 'ERR:', 4) === 0) { return ['ok' => false, 'error' => substr($out, 4)]; }
    $pid = (int) $out;
    if ($pid <= 0) { return ['ok' => false, 'error' => 'Launch failed (no PID returned). Check the executable path and arguments.']; }
    server_pid_write($pid);
    return ['ok' => true, 'pid' => $pid];
}

// Stop the running server. If we started it (stored PID alive) kill its whole tree
// — that is provenance-safe. Otherwise fall back to the process on the port, and
// only kill it if its image name looks like llama-server (never a kill-by-name).
function server_stop(): array {
    $port = server_port();
    $stored = server_pid_read();
    if (pid_running($stored)) {
        @shell_exec('taskkill /PID ' . $stored . ' /T /F 2>NUL');
        server_pid_clear();
        return ['ok' => true, 'stopped' => $stored];
    }
    $portPid = port_pid($port);
    if ($portPid <= 0) { server_pid_clear(); return ['ok' => false, 'error' => 'No running server found on port ' . $port . '.']; }
    $img = pid_image($portPid);
    if (stripos($img, 'llama-server') === false) {
        return ['ok' => false, 'error' => 'Refusing to stop PID ' . $portPid . ' (' . ($img ?: 'unknown') . '): it does not look like llama-server. Stop it manually if that is intended.'];
    }
    @shell_exec('taskkill /PID ' . $portPid . ' /F 2>NUL');
    server_pid_clear();
    return ['ok' => true, 'stopped' => $portPid, 'image' => $img];
}

// Tail the captured console log. $from is a byte offset the client passes back for
// incremental fetches. Returns the new bytes since $from plus the current size.
function server_log_tail(int $from, int $maxBytes = 200000): array {
    $f = SERVER_CONSOLE_LOG;
    if (!is_file($f)) { return ['exists' => false, 'text' => '', 'size' => 0]; }
    $size = (int) @filesize($f);
    if ($from < 0 || $from > $size) { $from = 0; }              // truncated/rotated → restart
    $start = ($from === 0 && $size > $maxBytes) ? $size - $maxBytes : $from;
    $len = $size - $start;
    if ($len <= 0) { return ['exists' => true, 'text' => '', 'size' => $size]; }
    $fh = @fopen($f, 'rb');
    if (!$fh) { return ['exists' => true, 'text' => '', 'size' => $size]; }
    @fseek($fh, $start);
    $data = (string) @fread($fh, $len);
    @fclose($fh);
    return ['exists' => true, 'text' => $data, 'size' => $size];
}

// ---------------------------------------------------------------------------
// Machine performance metrics (Windows / PowerShell)
// ---------------------------------------------------------------------------

// Returns: cpu (%), memTotal, memFree (bytes), serverMem (llama-server working
// set), and gpu => {util, memUsed, memTotal, name} when an NVIDIA GPU is present.
// Uses wmic (fast, ~0.1s/call) plus nvidia-smi directly when available.
function get_system_stats(): array {
    if (!function_exists('shell_exec')) {
        return ['error' => 'shell_exec disabled'];
    }
    $out = ['cpu' => null, 'memTotal' => null, 'memFree' => null, 'serverMem' => 0, 'gpu' => null];

    $cpu = (string) @shell_exec('wmic cpu get loadpercentage /value 2>NUL');
    if (preg_match('/LoadPercentage=(\d+)/', $cpu, $m)) {
        $out['cpu'] = (int) $m[1];
    }

    $mem = (string) @shell_exec('wmic OS get FreePhysicalMemory,TotalVisibleMemorySize /value 2>NUL');
    if (preg_match('/FreePhysicalMemory=(\d+)/', $mem, $m)) {
        $out['memFree'] = (int) $m[1] * 1024;
    }
    if (preg_match('/TotalVisibleMemorySize=(\d+)/', $mem, $m)) {
        $out['memTotal'] = (int) $m[1] * 1024;
    }

    $proc = (string) @shell_exec('wmic process where name=\'llama-server.exe\' get WorkingSetSize /value 2>NUL');
    if (preg_match_all('/WorkingSetSize=(\d+)/', $proc, $m)) {
        $out['serverMem'] = array_sum(array_map('intval', $m[1])); // sum if multiple
    }

    $smi = (getenv('SystemRoot') ?: 'C:\\Windows') . '\\System32\\nvidia-smi.exe';
    if (is_file($smi)) {
        $g = (string) @shell_exec('"' . $smi . '" --query-gpu=utilization.gpu,memory.used,memory.total,name --format=csv,noheader,nounits 2>NUL');
        $parts = array_map('trim', explode(',', trim($g)));
        if (count($parts) >= 4 && is_numeric($parts[0])) {
            $out['gpu'] = [
                'util'     => (int) $parts[0],
                'memUsed'  => (int) $parts[1] * 1048576,
                'memTotal' => (int) $parts[2] * 1048576,
                'name'     => $parts[3],
            ];
        }
    }

    return $out;
}
