# Installing Hyperion

**[Hyperion](https://hyperion.goblinreactor.com)** is a **Wt (C++)** multi-account
dashboard for **[jinghong](https://jinghong.goblinreactor.com)** order routers,
with live marks from **[goblin-slurp](https://slurp.goblinreactor.com)**.
Infrastructure paths and account lists are **not** compiled in; they come from
a master JSON config and per-account env files.

For architecture and how Hyperion talks to goblin-core, goblin-slurp, and
jinghong, see **[SETUP.md](SETUP.md)**.

---

## Security (read this first)

**Hyperion provides no authentication, authorization, or TLS by itself.**

Anyone who can reach the HTTP port can:

- View all configured accounts’ positions, cash, and robot state
- Send trade intents as source `hyperion` (via the Positions live-trade UI)
- Trigger block / liquidate / mute actions against jinghong HTTP APIs

**Install only on a trusted internal network** (localhost, private VPC, or
VPN). Do **not** expose the listen port to the public internet. Prefer binding
to an internal interface or `127.0.0.1` and reverse-proxy with auth if remote
access is required.

---

## Dependencies

| Component | Notes |
|-----------|--------|
| **C++23** compiler | GCC 13+ / Clang 17+ recommended |
| **CMake** | ≥ 3.24 |
| **Wt 4** | `Wt` + `HTTP` components (`find_package(Wt 4)`) |
| **libcurl** | jinghong HTTP client |
| **hiredis** | Redis / goblin-core UDS (RESP) |
| **nlohmann-json** | ≥ 3.11 |

Optional at runtime (for a full trading desk):

- **goblin-core** (or any Redis-compatible pub/sub) — market data + jinghong STATUS/ORDER
- **[goblin-slurp](https://slurp.goblinreactor.com)** — publishes `T:` / `Q:` trade and quote channels
- **[jinghong](https://jinghong.goblinreactor.com)** — one process (and env file) per brokerage account

---

## Build

From the hyperion source tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/wt   # if Wt is not in the default prefix
cmake --build build -j"$(nproc)"
```

Binary: `build/hyperion` (or `build-hail/hyperion` if you use a custom build dir).

Install into a prefix (optional):

```bash
cmake --install build --prefix /opt/hyperion
# installs:
#   bin/hyperion
#   share/hyperion/static/
#   share/hyperion/config/*.example
```

---

## Configuration before first run

### 1. Master config

Copy the example and edit paths for your host:

```bash
sudo mkdir -p /etc/hyperion
sudo cp config/hyperion.example.json /etc/hyperion/hyperion.json
sudo edit /etc/hyperion/hyperion.json
```

Minimal shape:

```json
{
  "redis_path": "/run/goblin-core/goblin.sock",
  "ui_hz": 4,
  "equity_hz": 1,
  "account_poll_sec": 60,
  "alpaca_limit_qpm": 200,
  "accounts": [
    "/etc/jinghong/paper1.env",
    { "env": "/etc/jinghong/paper3.env", "label": "Paper 3" }
  ]
}
```

| Field | Meaning |
|-------|---------|
| `redis_path` | Unix socket for goblin-core / Redis (RESP `SUBSCRIBE` / `PUBLISH`) |
| `accounts` | List of account **env file paths**, or `{ "env", "id?", "label?" }` |
| `ui_hz` / `equity_hz` | UI refresh and equity sampling rates |
| `account_poll_sec` | How often to `GET /v1/account` on each jinghong |
| `alpaca_limit_qpm` | Display limit for the Alpaca REST rate chart (default 200) |

You can also set `HYPERION_CONFIG=/etc/hyperion/hyperion.json` instead of
passing `--config` every time.

### 2. Per-account env files

Each account is a jinghong-style `KEY=VALUE` file (often the same
`EnvironmentFile=` jinghong’s systemd unit uses). Hyperion reads **routing**
keys only; it does not need Alpaca secrets to run the UI (jinghong does).

Required:

```bash
JINGHONG_ORDER_CHANNEL=ACCOUNT:ORDER:paper3
JINGHONG_STATUS_CHANNEL=ACCOUNT:STATUS:paper3
JINGHONG_HTTP_PORT=8768
```

Recommended for the UI:

```bash
HYPERION_LABEL=Paper 3          # tab title
# HYPERION_ID=paper3            # optional; default = filename stem
JINGHONG_HTTP_BIND=0.0.0.0      # Hyperion still dials 127.0.0.1
JINGHONG_ALPACA_CHANNEL=ACCOUNT:ALPACA:paper3
```

See `config/account.env.example`.

**Permissions:** the Hyperion process user must be able to **read** each env
file (e.g. `root:adam` mode `0640` with service `User=adam` / `Group=adam`).

### 3. Static assets and Wt resources

Hyperion needs:

- **Docroot** — this repo’s `static/` (CSS), installed as
  `share/hyperion/static` or passed as `--docroot`
- **Wt resources** — Wt’s built-in resources tree, e.g.
  `/opt/wt/share/Wt/resources` or `/usr/share/Wt/resources`

---

## Run (foreground)

```bash
./build/hyperion \
  --config /etc/hyperion/hyperion.json \
  --docroot ./static \
  --resources-dir /opt/wt/share/Wt/resources \
  --http-listen 127.0.0.1:8110
```

Then open `http://127.0.0.1:8110/`.

Ad-hoc without a master file:

```bash
./build/hyperion \
  --redis /run/goblin-core/goblin.sock \
  --account-env /etc/jinghong/paper3.env \
  --docroot ./static \
  --resources-dir /opt/wt/share/Wt/resources \
  --http-listen 127.0.0.1:8110
```

Useful flags:

| Flag | Purpose |
|------|---------|
| `--config PATH` | Master JSON |
| `--redis PATH` | Override `redis_path` |
| `--account-env PATH` | Append one account env (repeatable) |
| `--server ID,HTTP,ORDER,STATUS[,ALPACA]` | Ad-hoc account without env |
| `--ui-hz` / `--equity-hz` / `--account-poll` | Rate overrides |

Everything after the first unknown flag, or after `--`, is passed to Wt
(e.g. `--http-listen`, `--docroot`, `--resources-dir`).

---

## Install as a systemd service (example)

This repository ships an **example** host install script and unit under
`deploy/` (paths, user, and NUMA affinity are host-specific — edit them).

```bash
# After editing deploy/hyperion.json and deploy/hyperion.service for your host:
sudo bash deploy/install-on-hail.sh
```

Typical layout after install:

| Path | Role |
|------|------|
| `/opt/hyperion/bin/hyperion` | Binary |
| `/opt/hyperion/share/hyperion/` | Docroot (`static/`) |
| `/etc/hyperion/hyperion.json` | Master config (not overwritten if already present) |
| `/etc/jinghong/*.env` | Account envs (shared with jinghong) |
| `/var/log/hyperion/hyperion.log` | stdout/stderr |

Service should:

1. Run as a user that can open the Redis UDS and read account env files
2. Pass `--config /etc/hyperion/hyperion.json`
3. Set `LD_LIBRARY_PATH` (or rpath) so `libwt` / `libwthttp` resolve
4. Listen only on a **trusted** address

```bash
sudo systemctl enable --now hyperion
sudo systemctl status hyperion
journalctl -u hyperion -f
# or: tail -f /var/log/hyperion/hyperion.log
```

### Verify

On success the log should list each account:

```text
hyperion: listening (redis=..., servers=N, config=/etc/hyperion/hyperion.json)
  paper3  label="Paper 3"  http=http://127.0.0.1:8768  order=ACCOUNT:ORDER:paper3  ...
```

Smoke checks:

```bash
curl -sS -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8110/
# each jinghong should answer:
curl -sS http://127.0.0.1:8768/v1/status | head
```

---

## Adding or removing accounts

1. Ensure a jinghong instance (and env file) exists for the account  
2. Add or remove the env path under `accounts` in the master JSON  
3. Optionally set `HYPERION_LABEL` in the env file for the tab title  
4. `sudo systemctl restart hyperion`

No rebuild required.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|----------------|
| Process exits immediately; help text in log | Config/env parse error (missing keys, bad JSON, unreadable env) |
| `cannot open account env` | Permissions: service group cannot read `0640` env files |
| `redis down` in UI | Wrong `redis_path`, goblin-core down, or socket permissions |
| Empty prices | Not subscribed / goblin-slurp not publishing `T:` / `Q:` for watched symbols |
| Empty book after trade | jinghong not on STATUS channel, or Hyperion not subscribed to that channel |
| Wt “could not load resources” | Wrong `--resources-dir` |
| `error while loading shared libraries: libwthttp` | Set `LD_LIBRARY_PATH` to Wt’s `lib` (as in the example unit) |

---

## Related docs

- **[SETUP.md](SETUP.md)** — systems Hyperion talks to, channels, HTTP API usage  
- **[README.md](README.md)** — feature overview and config quick reference  
- **[hyperion.goblinreactor.com](https://hyperion.goblinreactor.com)** — Hyperion product site  
- **[jinghong.goblinreactor.com](https://jinghong.goblinreactor.com)** — order router (see also jinghong `docs/HEARTBEATS.md`)  
- **[slurp.goblinreactor.com](https://slurp.goblinreactor.com)** — goblin-slurp market-data publisher  
