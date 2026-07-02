# Install & Deploy — Guardrail Admin + Chat

This bundle is a PHP app served by **XAMPP/Apache** that sits next to a running
`llama-server` and provides:

- **Chat** (`index.html`) — a web chat UI for approved accounts.
- **Admin panel** (`admin.html`) — dashboard, request logs, usage, account
  management, allow-list editing, and llama-server start/stop.
- **Editor API** (`v1.php`) — an OpenAI-compatible, token-authenticated endpoint
  so editors like the Continue VS Code extension can use the model.

It edits the same files `llama-server` uses (access-allow CSV, log dir) — no
server changes and no rebuild required.

---

## 1. Requirements

- **Windows** with **XAMPP** (Apache + PHP 8.x).
- PHP **curl** extension enabled (default in XAMPP) — used to proxy chat and
  probe the server.
- **PowerShell** available (default on Windows) — used for llama-server
  start/stop, log-zip backups, and rendering. *No `zip`/`ZipArchive` extension is
  required* (backups use `Compress-Archive`).
- A running **`llama-server`** (this repo's guardrail build).

---

## 2. Deploy the files

1. Copy the `admin-php` folder into the web root as **`admin`**, e.g.
   `D:\xampp\htdocs\admin\`.
2. Start Apache (XAMPP Control Panel). For GPU access when starting llama-server
   from the dashboard, launch Apache from the Control Panel **in your own desktop
   session**, not as a Windows service.

The app lives at `http://localhost/admin/` (chat) and
`http://localhost/admin/admin.html` (admin panel).

---

## 3. Configure paths (`config.php`)

Edit `D:\xampp\htdocs\admin\config.php` so it matches how you launch
`llama-server`:

| Constant | Meaning |
| --- | --- |
| `ACCESS_ALLOW_FILE` | same path you pass to `--access-allow-file` |
| `LOG_DIR` | same path you pass to `--log-dir` |
| `SERVER_URL` | base URL of llama-server (default `http://127.0.0.1:8080`) |
| `ENFORCE_CHAT_HOST_ALLOWLIST` | `true` = chat obeys the host allow-list |
| `LOG_AUTO_BACKUP_MONTHLY` | `true` = auto-zip completed months' logs |

Everything else (accounts file, admin-password hash, chat history, server launch
config, log backups) is derived from the **data directory** =
`dirname(ACCESS_ALLOW_FILE)`. That directory must be **outside the web root**, and
Apache/PHP must have **read/write** on it and **read** on the log dir.

> The admin-password hash, accounts, and chats are stored in the data dir, not
> under `htdocs` — so they can't be fetched by URL. The `.htaccess` also denies
> serving dotfiles as a second layer.

---

## 4. First run — set the admin password

Open **`http://localhost/admin/admin.html`** *on the server machine*. On first
run it prompts you to set an admin password (min 8 chars), stored hashed in the
data dir. There are no default credentials.

The admin panel is **localhost-only** by default (see Access model below), so
first-run setup must be done from the server itself.

---

## 5. Run llama-server

Launch with matching flags (or set the executable + args in the dashboard's
**Edit launch command** and use Start/Stop):

```bat
llama-server.exe -m model.gguf --host 0.0.0.0 --port 8080 ^
  --access-allow-file D:\data\access-allow.csv ^
  --log-dir D:\data\log
```

Allow-list changes are hot-reloaded by llama-server (no restart). Loopback
(`127.0.0.1` / `::1`) always bypasses the allow-list.

---

## 6. Access model (who can reach what, from where)

- **Admin panel** — only from the **server itself** (`localhost`/`127.0.0.1`),
  **or** from an **administrator account** signed in from the exact IP registered
  to that account. The initial admin-**password** login is localhost-only.
- **Chat** — approved (active) accounts. A message is proxied to llama-server only
  if the client host passes the allow-list (an allow-list entry, or the IP of an
  active account). Sign-ups appear in the admin panel as *pending*.
- **Editor API (`v1.php`)** — reachable from the network but gated by a
  per-account **bearer token** *and* the same host allow-list.

By default the panel's `.htaccess` is `Require all granted` (network-reachable);
tighten it to specific subnets by replacing that line, e.g.:

```apache
Require local
Require ip 192.168.1.0/24
```

---

## 7. Connect an editor (Continue)

1. In the chat, open the account menu → **API token** → **Generate token** (shown
   once; copy it).
2. In Continue's `config.yaml`:

```yaml
name: Local Config
version: 1.0.0
schema: v1
models:
  - name: Coding Agent
    provider: openai
    model: coder
    apiBase: http://<server-host>/admin/v1.php
    apiKey: sk-guard-...(your token)
    roles:
      - chat
      - edit
      - apply
```

The token maps to your account (used for Logs/Usage attribution). No
llama-server `--api-key` is needed — the proxy handles auth.

---

## 8. Security checklist

- **Use HTTPS** if the app is reachable beyond a trusted LAN/VPN — tokens and
  passwords are sent in the `Authorization`/POST body and are otherwise in the
  clear.
- Keep the **admin password strong**; the initial admin login stays
  localhost-only as the hardened master key.
- Keep the **data directory outside the web root**; keep the dotfile deny in
  `.htaccess`.
- Suspending an account removes its host from the allow-list, cutting off access.

---

## 9. File map

| File | Purpose |
| --- | --- |
| `index.html`, `chat.js` | chat UI (login/signup, chat, attachments, HTML render) |
| `admin.html`, `app.js` | admin panel (dashboard, logs, usage, accounts, allow-list) |
| `style.css` | shared styling (dark/light) |
| `api.php` | JSON backend (session/chat/admin actions) |
| `v1.php` | OpenAI-compatible, token-authenticated editor endpoint |
| `lib.php` | auth, CSRF, accounts, allow-list, logs, backups, server control |
| `config.php` | paths + settings (edit this) |
| `.htaccess` | access control + Authorization pass-through + no-cache for HTML |
