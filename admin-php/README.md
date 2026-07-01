# Guardrail admin

A small admin tool to manage `llama-server`'s **access-allow list** (allowed
hosts), view its **request logs**, and watch **machine performance / usage**. It
runs **separately** from `llama-server` on the same machine and edits the same
files — no changes to the server and no rebuild required.

**Architecture:** a **standalone HTML/JS frontend** (`index.html` + `app.js` +
`style.css`) talking to a **PHP JSON backend** (`api.php`, using `lib.php`). The
frontend is static; PHP only serves data.

## How it works

- **Users** = entries in the `--access-allow-file` CSV (`alias,ip,mac1[,mac2...]`).
  The app reads/edits this file. `llama-server` reloads it automatically when it
  changes (it checks the file's modification time on each request) — **no restart
  needed**. Writes are atomic (temp file + rename), and an invalid file is never
  produced because every entry is validated first.
- **Logs** = the `llama-server-<date>.jsonl` files in `--log-dir`. The app reads
  them (read-only) and shows the newest records with a client-side filter.

## Setup

1. Install PHP 7.4+ (8.x recommended). No extensions beyond the defaults.
2. Edit `config.php`:
   - `ACCESS_ALLOW_FILE` — same path you pass to `--access-allow-file`.
   - `LOG_DIR` — same path you pass to `--log-dir`.
   The PHP process must have **read/write** on the CSV and **read** on the log dir.
3. Serve it. Two options:

   **Built-in server (simplest, localhost-only):**
   ```
   php -S 127.0.0.1:9000 -t admin-php
   ```
   Open <http://127.0.0.1:9000/>.

   **XAMPP / Apache:** copy the `admin-php` folder into `D:\xampp\htdocs\` (e.g.
   `htdocs\admin`), start Apache, and open <http://localhost/admin/>. The bundled
   `.htaccess` restricts access to the local machine (`Require local`) — adjust it
   if you intend to allow specific remote admins.

   On first visit you'll be asked to set an admin password. It is stored hashed in
   the **data directory** (next to the access-allow file, e.g.
   `C:\dev\llama-data\.admin-password.hash`) — deliberately **outside** the
   web-served folder so it can't be fetched by URL.

Run `llama-server` with matching flags, e.g.:
```
llama-server.exe -m model.gguf --host 0.0.0.0 --port 8080 ^
  --access-allow-file C:\dev\llama-data\access-allow.csv ^
  --log-dir C:\dev\llama-data\logs
```

## Security — read this

This panel **controls who may access llama-server**. Anyone who can reach it can
grant/revoke access, so:

- **Bind to localhost** (`127.0.0.1`) as shown, or otherwise restrict it. Note
  that **XAMPP's Apache listens on all interfaces by default**, so the included
  `.htaccess` (`Require local`) is what keeps the panel local — keep it.
- The password hash lives in the data directory, outside the web root, and the
  `.htaccess` also denies serving dotfiles — so the secret is not URL-reachable.
- Serve over **HTTPS** if it's reachable beyond localhost, and set
  `'secure' => true` in `start_session()` (see `lib.php`).
- Remember **loopback (`127.0.0.1` / `::1`) always bypasses the allow-list** in
  `llama-server`, so entries only gate non-loopback clients.

## Files

| File | Purpose |
| --- | --- |
| `index.html` | standalone SPA shell (static) |
| `app.js` | all UI: auth, dashboard, users, logs, usage, charts, live updates |
| `style.css` | styling (dark/light themes) |
| `api.php` | JSON backend: `?action=session\|setup\|login\|logout\|overview\|health\|stats\|users\|users_save\|logs\|usage` |
| `lib.php` | auth, CSRF, atomic CSV read/write/validate, log reading, stats |
| `config.php` | paths + settings (edit this) |

The frontend is served as static files and calls `api.php`. To host the UI
elsewhere, point `app.js`'s `api()` calls at wherever `api.php` runs (CORS/session
permitting).
