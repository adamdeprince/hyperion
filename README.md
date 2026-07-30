# Hyperion

**[Hyperion](https://hyperion.goblinreactor.com)** is a **Wt (C++)** dashboard for
one or more **[jinghong](https://jinghong.goblinreactor.com)** order servers,
with marks from **[goblin-slurp](https://slurp.goblinreactor.com)** on the
goblin-core bus.

**No authentication** — install only on a trusted internal network. See
**[SETUP.md](SETUP.md)** (security + what Hyperion talks to) and
**[INSTALL.md](INSTALL.md)** (build, config, systemd).

Licensed under the **Apache License 2.0** — see [LICENSE](LICENSE).

Infrastructure (Redis UDS, account list, channels, HTTP ports) is **not**
compiled in. It comes from a **master JSON config** that points at per-account
**env files** (the same files jinghong systemd uses).

## Config

### Master config (`--config` / `HYPERION_CONFIG`)

```json
{
  "redis_path": "/run/goblin-core/goblin.sock",
  "ui_hz": 4,
  "equity_hz": 1,
  "account_poll_sec": 60,
  "alpaca_limit_qpm": 200,
  "accounts": [
    "/etc/jinghong/live.env",
    { "env": "/etc/jinghong/paper3.env", "label": "Paper 3" }
  ]
}
```

See `config/hyperion.example.json`. Deploy-specific copies live outside the
open-source tree (e.g. `/etc/hyperion/hyperion.json`).

### Account env (one jinghong server)

Required:

| Key | Purpose |
|-----|---------|
| `JINGHONG_ORDER_CHANNEL` | Intent channel |
| `JINGHONG_STATUS_CHANNEL` | Status channel |
| `JINGHONG_HTTP_PORT` | HTTP port for status/account |

Optional:

| Key | Purpose |
|-----|---------|
| `HYPERION_LABEL` / `TAB_NAME` / `LABEL` | UI tab title |
| `HYPERION_ID` / `JINGHONG_ID` | Stable account id (default: env filename stem) |
| `JINGHONG_HTTP_BIND` | Bind address (`0.0.0.0` → Hyperion uses `127.0.0.1`) |
| `JINGHONG_ALPACA_CHANNEL` | Alpaca admin/QPM channel (else derived from STATUS) |
| `HYPERION_HTTP_BASE` | Full base URL override |

Example: `config/account.env.example`.

## What it shows

One **tab per account** in the master config. Each tab has Positions (live trade),
Charts, Robots, Locks.

**Prices** come from [goblin-slurp](https://slurp.goblinreactor.com) channels on
the Redis UDS:

- trades `T:<SYM>`
- quotes `Q:<SYM>`

**Book state** comes from [jinghong](https://jinghong.goblinreactor.com):

1. HTTP bootstrap: `GET {jinghong}/status`
2. Redis STATUS — fills, order_submit, heartbeats, …
3. Redis ORDER — agent intents / heartbeats

## Project sites

| Component | Site |
|-----------|------|
| Hyperion | https://hyperion.goblinreactor.com |
| jinghong | https://jinghong.goblinreactor.com |
| goblin-slurp | https://slurp.goblinreactor.com |

## Docs

| Doc | Contents |
|-----|----------|
| **[INSTALL.md](INSTALL.md)** | Dependencies, build, config files, run, systemd, troubleshooting |
| **[SETUP.md](SETUP.md)** | goblin-core / Redis, goblin-slurp `T:`/`Q:`, jinghong ORDER + STATUS + HTTP, security |

## Quick build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/wt
cmake --build build -j
```

Dependencies: C++23, Wt 4, libcurl, hiredis, nlohmann-json.

```bash
./build/hyperion \
  --config /etc/hyperion/hyperion.json \
  --docroot ./static \
  --resources-dir /opt/wt/share/Wt/resources \
  --http-listen 127.0.0.1:8110
```

Full install: **[INSTALL.md](INSTALL.md)**. Ops topology: **[SETUP.md](SETUP.md)**.
