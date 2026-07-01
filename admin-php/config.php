<?php
// ============================================================================
// Configuration for the llama-server guardrail admin app.
//
// Edit the paths below to match how you launch llama-server:
//   llama-server.exe ... --access-allow-file <ACCESS_ALLOW_FILE> --log-dir <LOG_DIR>
// ============================================================================

// Absolute path to the access-allow CSV passed to --access-allow-file.
// Created on first save if it does not exist.
const ACCESS_ALLOW_FILE = 'C:\\dev\\llama-data\\access-allow.csv';

// Absolute path to the directory passed to --log-dir (contains
// llama-server-<date>.jsonl files).
const LOG_DIR = 'C:\\dev\\llama-data\\logs';

// Base URL of the running llama-server (for the dashboard status check).
// Match --host/--port; loopback is fine (it bypasses the allow-list).
const SERVER_URL = 'http://127.0.0.1:8080';

// Where the admin password hash is stored. On first run the app prompts you to
// set a password (no default credentials). It is deliberately placed in the
// data directory (next to the access-allow file) so it is NOT inside the
// web-served folder and cannot be fetched by URL. Keep it private.
define('ADMIN_HASH_FILE', dirname(ACCESS_ALLOW_FILE) . DIRECTORY_SEPARATOR . '.admin-password.hash');

// Where chat-user accounts (signup + approval) are stored. Kept in the private
// data directory (not web-served). JSON: {"users":[{id,name,hash,status,...}]}.
define('ACCOUNTS_FILE', dirname(ACCESS_ALLOW_FILE) . DIRECTORY_SEPARATOR . '.accounts.json');

// Directory holding one chat-history file per user (server-side saved chats).
define('CHATS_DIR', dirname(ACCESS_ALLOW_FILE) . DIRECTORY_SEPARATOR . 'chats');

// llama-server launch control (editable command line + start/stop from the dashboard).
define('SERVER_LAUNCH_FILE', dirname(ACCESS_ALLOW_FILE) . DIRECTORY_SEPARATOR . '.server-launch.json');
define('SERVER_PID_FILE',    dirname(ACCESS_ALLOW_FILE) . DIRECTORY_SEPARATOR . '.server.pid');
define('SERVER_CONSOLE_LOG', dirname(ACCESS_ALLOW_FILE) . DIRECTORY_SEPARATOR . 'server-console.log');

// Max number of log lines to show per file (newest kept).
const LOG_TAIL_LINES = 500;

// Where zipped log backups are stored (a subfolder of the log directory).
define('LOG_BACKUP_DIR', rtrim(LOG_DIR, '/\\') . DIRECTORY_SEPARATOR . 'backups');
// Automatically archive completed months' logs into a zip once per month.
const LOG_AUTO_BACKUP_MONTHLY = true;

// Session cookie name.
const SESSION_NAME = 'llama_admin_sid';

// When true, the web chat proxy checks the client's host against the access-allow
// list (and approved-account IPs) before forwarding to llama-server — so chat
// obeys the same host access control as the direct API. Loopback always bypasses.
const ENFORCE_CHAT_HOST_ALLOWLIST = true;
