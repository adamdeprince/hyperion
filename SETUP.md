# Hyperion setup (devops)

This document describes **what [Hyperion](https://hyperion.goblinreactor.com)
connects to**, the **protocols and channel names** it expects, and how it fits
next to goblin-core, **[goblin-slurp](https://slurp.goblinreactor.com)**, and
**[jinghong](https://jinghong.goblinreactor.com)**. For build steps, see
**[INSTALL.md](INSTALL.md)**.

| Component | Site |
|-----------|------|
| Hyperion | https://hyperion.goblinreactor.com |
| jinghong | https://jinghong.goblinreactor.com |
| goblin-slurp | https://slurp.goblinreactor.com |

---

## Security model

**Hyperion has no built-in authentication, authorization, multi-tenancy, or TLS.**

The HTTP UI is a full control surface for every configured account: positions,
cash, robot muting, trading freezes, liquidations, and live intents as source
`hyperion`. Treat the listen address like a **console on a trading host**.

**Intended deployment:** trusted internal network only (private rack, VPN,
localhost). Do not put Hyperion on a public IP without an authenticating reverse
proxy and network policy that you control.

---

## System context

```text
                    ┌─────────────────────────────────────┐
                    │           Trusted host / VPC          │
                    │                                       │
  Browsers ────────►│  Hyperion  (Wt HTTP, e.g. :8110)      │
                    │     │              │                  │
                    │     │ UDS RESP     │ HTTP             │
                    │     ▼              ▼                  │
                    │  goblin-core    jinghong × N           │
                    │  (or Redis)     (one process / account)│
                    │     ▲              ▲                  │
                    │     │ PUBLISH      │ ORDER subscribe  │
                    │     │ T:/Q:        │ STATUS publish   │
                    │  goblin-slurp   trading robots / UI    │
                    └─────────────────────────────────────┘
```

Hyperion is **stateless with respect to the broker**. The book of record for
orders is jinghong (+ Alpaca). Market marks come from the pub/sub bus.
Hyperion’s job is to **subscribe, poll lightly, and present** a live desk UI.

---

## 1. Pub/sub bus (goblin-core or Redis-compatible)

### Role

Hyperion opens a **Unix domain socket** and speaks **RESP** (Redis protocol):

- `SUBSCRIBE` to many channels at once (hiredis multi-channel mux)
- `PUBLISH` heartbeats / intents on each account’s **ORDER** channel

### Config

Master JSON:

```json
"redis_path": "/run/goblin-core/goblin.sock"
```

Or CLI: `--redis PATH` / env `HYPERION_REDIS_PATH`.

### Requirements

| Requirement | Notes |
|-------------|--------|
| RESP over UDS (or TCP if you point hiredis there via a path your build supports) | Production path is **UDS to goblin-core** |
| Pub/sub | `SUBSCRIBE` / `PUBLISH` / multi-channel subscription |
| Permissions | Hyperion process must open the socket (group membership is typical) |

**goblin-core** is the usual bus in this stack: a high-performance Redis-compatible
server used by market data and order routing. Any Redis 6+ style pub/sub that
accepts the same channel names can work for Hyperion’s purposes.

Hyperion does **not** use Redis for durable storage. If the bus is down, the UI
shows redis down and **does not** fall back to inventing fills over HTTP alone
for the live path (bootstrap still uses jinghong HTTP once at session start).

---

## 2. Market data — goblin-slurp → bus → Hyperion

### Role

**[goblin-slurp](https://slurp.goblinreactor.com)** ingests venue/market feeds and
**publishes** onto the same bus Hyperion reads.

Hyperion does **not** connect to Massive/Polygon/etc. directly. It only
subscribes to channels that slurp (or an equivalent publisher) already feeds.

### Channel names

| Channel | Direction | Payload (expected) |
|---------|-----------|-------------------|
| `T:<SYM>` | bus → Hyperion | Trade JSON (Massive-style): `{"ev":"T","sym":"IBM","p":…,"t":…}` — price in `p`, time in `t` (ms) |
| `Q:<SYM>` | bus → Hyperion | Quote JSON: bid/ask in `bp` / `ap` (numbers or numeric strings) |

Examples: `T:SPY`, `Q:IBM`.

### When Hyperion subscribes

On each UI session, Hyperion subscribes to `T:` / `Q:` for symbols that appear
in that account’s book (holdings, intents, inflight) plus any symbol the user
is actively trading or charting. Channel set is refreshed as the book changes.

### Ops checklist

- goblin-slurp (or equivalent) is running and **PUBLISH**ing to the same
  `redis_path` Hyperion uses
- Symbols you care about are in slurp’s universe / allowlist
- No second “shadow” bus — Hyperion and slurp must share the socket/namespace

If trades/quotes never move prices in the UI but redis is “up”, debug slurp
publish and channel spelling first (`T:SPY` vs `t:spy`).

---

## 3. Order management — jinghong

Each **account tab** is one **[jinghong](https://jinghong.goblinreactor.com)**
process (one Alpaca key / one book). Hyperion talks to jinghong in **two** ways.

### 3a. Outgoing intents — Redis ORDER channel

**Hyperion → bus → jinghong**

| Item | Detail |
|------|--------|
| Channel | From account env: `JINGHONG_ORDER_CHANNEL` (e.g. `ACCOUNT:ORDER:paper3`) |
| Direction | Hyperion **PUBLISH**es; jinghong **SUBSCRIBE**s |
| Purpose | Live trade UI and ~5s source heartbeats so jinghong keeps robot `hyperion` alive |

Intent JSON (position target, not buy/sell):

```json
{"source": "hyperion", "symbol": "IBM", "position": 50}
```

Heartbeat (no position change):

```json
{"source": "hyperion"}
```

Conventions (must match jinghong / robot docs):

- `source` identifies the robot (`hyperion` for this UI)
- `position` is a **signed share count** (+ long, − short, `0` flat for that source)
- Heartbeats about every **5s**; jinghong silence TTL is typically **15s**
- Same-size re-intent (already holding the target) should be a **no-op** at jinghong

Other trading systems publish to the **same ORDER channel** with their own
`source` ids (e.g. `FRI_SAFE_45S`). Hyperion **subscribes** to ORDER as well so
the Robots list and “pending intent” UI stay live.

### 3b. Incoming status — Redis STATUS channel

**jinghong → bus → Hyperion**

| Item | Detail |
|------|--------|
| Channel | `JINGHONG_STATUS_CHANNEL` (e.g. `ACCOUNT:STATUS:paper3`) |
| Direction | jinghong **PUBLISH**es; Hyperion **SUBSCRIBE**s |
| Purpose | Live book updates without polling positions every tick |

Important `type` values Hyperion understands (non-exhaustive):

| `type` | Meaning |
|--------|---------|
| `intent` | Book changed / heartbeat rebroadcast with sources |
| `order_submit` / `order_reject` | Market order lifecycle |
| `fill_applied` | Local cash/qty after fill (preferred for UI) |
| `alpaca_ws` | Raw Alpaca trade_updates passthrough |
| `reconcile` / `reconcile_skip` | Planner activity / no-op |
| `heartbeat` | Source liveness for Robots screen |
| `liquidator_lock` | Blocks / mutes snapshot |

### 3c. Alpaca admin rate channel (optional)

| Item | Detail |
|------|--------|
| Channel | `JINGHONG_ALPACA_CHANNEL` or derived as `ACCOUNT:ALPACA` + STATUS suffix |
| Purpose | Per-request Alpaca REST audit events for the QPM chart |

### 3d. HTTP to jinghong

**Hyperion → jinghong HTTP** (base URL from env: port + bind, or
`HYPERION_HTTP_BASE`).

| When | Method / path | Why |
|------|----------------|-----|
| Session / process bootstrap | `GET /status` or `GET /v1/status` | Seed actual, intended, inflight, cash, sources, locks |
| ~every `account_poll_sec` (default 60) | `GET /v1/account` | Broker cash / equity reconcile (mismatch detection) |
| User actions | `POST /v1/intent`, `/v1/block/*`, `/v1/liquidate/*`, `/v1/mute/*`, … | Control plane from the UI |

HTTP is **not** the live fill path. Fills should arrive on **STATUS** (and
optionally `fill_applied`). If STATUS is broken, the UI will look stale even
when HTTP bootstrap succeeded once.

Base URL construction:

- Prefer loopback to the jinghong HTTP port (`JINGHONG_HTTP_PORT`)
- If env has `JINGHONG_HTTP_BIND=0.0.0.0`, Hyperion still dials `127.0.0.1`
- Override with `HYPERION_HTTP_BASE=http://host:port` when needed

---

## Channel map (typical multi-account host)

Names are **not** hardcoded in the Hyperion binary; they come from each account
env. A conventional layout:

| Account | ORDER (in) | STATUS (out) | ALPACA (audit) | HTTP |
|---------|------------|--------------|----------------|------|
| live | `ACCOUNT:ORDER:live` | `ACCOUNT:STATUS:live` | `ACCOUNT:ALPACA:live` | `:8765` |
| paper1 | `ACCOUNT:ORDER:paper1` | `ACCOUNT:STATUS:paper1` | `ACCOUNT:ALPACA:paper1` | `:8766` |
| paper2 | … | … | … | `:8767` |
| paper3 | … | … | … | `:8768` |

**Rules:**

1. One jinghong process ↔ one ORDER/STATUS pair ↔ one env file ↔ one UI tab  
2. Never share an unsuffixed `ACCOUNT:ORDER` across multiple jinghong processes  
3. Robots must publish to the **same** ORDER channel the account’s jinghong subscribes to  
4. Market data `T:` / `Q:` are **global** on the bus (not per-account)

---

## Master config and account env (ops view)

```text
/etc/hyperion/hyperion.json     ← master: redis_path + list of env paths
/etc/jinghong/paper3.env        ← one account: channels, HTTP port, tab label
/etc/jinghong/paper1.env
…
```

Master `accounts` array entries:

```json
"/etc/jinghong/paper3.env"
```

or

```json
{ "env": "/etc/jinghong/paper3.env", "id": "paper3", "label": "Paper 3" }
```

Tab title resolution order: master `label` override → env `HYPERION_LABEL` /
`TAB_NAME` / `LABEL` → account id (filename stem).

Deploy-specific JSON (host paths, NUMA, users) belongs under `/etc/hyperion/`
or your config management system — not in the open-source defaults of the
binary. Example templates: `config/hyperion.example.json`,
`config/account.env.example`.

---

## Startup sequence (what a healthy stack looks like)

1. **goblin-core** (or Redis) listening on the configured UDS  
2. **goblin-slurp** publishing `T:` / `Q:` for the symbol set you trade  
3. **jinghong@account** running, subscribed to its ORDER channel, publishing STATUS  
4. **Hyperion** starts, loads master config + env files, SUBSCRIBEs to all ORDER /
   STATUS / ALPACA channels, `GET /status` per account, then serves HTTP  

Browser: open the UI → one tab per account → Positions shows marks when
`T:`/`Q:` flow; trades update when STATUS `fill_applied` / book events land.

---

## Network and process isolation notes

| Concern | Guidance |
|---------|----------|
| HTTP listen | Internal only; `127.0.0.1` or private interface |
| Redis UDS | Restrict socket mode/group; Hyperion and slurp and jinghong need access |
| jinghong HTTP | Often `0.0.0.0` on the host; firewall so only local Hyperion/ops tools reach it |
| Secrets | Alpaca keys stay in jinghong env files; Hyperion only needs read access to routing keys in those files |
| Multi-host | If jinghong is remote, set `HYPERION_HTTP_BASE` and ensure the bus is shared or duplicated carefully |

---

## Failure modes (ops)

| Observation | Check |
|-------------|--------|
| UI “redis down” | UDS path, goblin-core up, permissions, SUBSCRIBE errors in logs |
| Prices frozen | goblin-slurp publish; channel `T:SYM` / `Q:SYM`; symbol subscribed |
| Intent sent, no fill | jinghong ORDER subscribe; global block; market hours; STATUS path |
| Cash mismatch flash | Account poll vs fill-tracked cash (see product docs); STATUS `fill_applied` |
| Empty Robots list | Nothing publishing heartbeats on ORDER; or wrong channel in env |
| Tab missing | Account not listed in master JSON, or env unreadable at start |

---

## Related components

| Component | Site | Responsibility |
|-----------|------|----------------|
| **Hyperion** | [hyperion.goblinreactor.com](https://hyperion.goblinreactor.com) | Desk UI, light HTTP poll, bus subscriber/publisher for UI robot |
| **jinghong** | [jinghong.goblinreactor.com](https://jinghong.goblinreactor.com) | Multi-source intent book → Alpaca market orders; STATUS publisher |
| **goblin-core** | — | Redis-compatible pub/sub bus (UDS) |
| **goblin-slurp** | [slurp.goblinreactor.com](https://slurp.goblinreactor.com) | Market data → `T:` / `Q:` on the bus |
| **Trading robots** | — | Publish intents/heartbeats to `ACCOUNT:ORDER:<account>` |

---

## Related docs

- **[INSTALL.md](INSTALL.md)** — build, install, systemd, config files  
- **[README.md](README.md)** — product overview  
- [jinghong.goblinreactor.com](https://jinghong.goblinreactor.com) — order router (repo: `docs/HEARTBEATS.md` for robot contract)  
- [slurp.goblinreactor.com](https://slurp.goblinreactor.com) — market-data publisher  
- [hyperion.goblinreactor.com](https://hyperion.goblinreactor.com) — this product  
