# Site analytics plan

Analytics for the www site, moving from Vercel Functions to a Loam backend on
Fly.io: async gRPC and HTTP on `std:http`, with C SQLite reached through FFI.

This is a plan. None of the backend exists yet.

## Decision

Replace the Vercel Functions and Neon approach with a Loam server. It keeps the
whole stack in Loam, and it pushes the language where it needs to grow: FFI,
an async server, and crypto.

| Choice | Decision |
|---|---|
| Host | One always-on Fly.io machine |
| Database | C SQLite through FFI, in a file on a Fly volume |
| API | gRPC-Web for the dashboard, one JSON route for beacons |
| TLS | Terminated by Fly's proxy |
| Packaging | No Docker and no Dockerfile: a Linux binary placed on a stock base image |

## Where things stand

| Piece | State |
|---|---|
| `std:analytics` and the web host | Committed in `aff6124`. Stays |
| Vercel functions, API test, password tool | Removed in phase 0. Readable in `2b9723f` |
| Vault page, footer door, `analytics.start` in www | Uncommitted. Backend-agnostic |
| `http.post_async`, `on_submit` on inputs | Uncommitted |

The vault page was tested against the old functions, in a browser, for sign in
and the tables. Still unchecked: the chart and stat grid fix, the footer door
click, the range switch, sign out, and narrow screens.

## What Loam has today

Checked in this repo on 2026-11-06.

| Need | Today | Gap |
|---|---|---|
| gRPC and JSON routes | `http.app`, `rpc`, `app.json`, gRPC-Web | None for the routes themselves |
| Async client | `call_async` over wasm fetch slots | None |
| Async server | `http.listen` is a non-blocking `poll` loop on one thread, with timeouts and limits | A long handler still holds everyone up |
| Public binding | `ListenProps.host`, 127.0.0.1 by default | None |
| C FFI | `#[c]`, `#![c_source]`, and `#![c_link]`, native builds only. See `docs/boundary.md` | None |
| SQLite | `std:sqlite`, vendored 3.54.1 in `packages/sqlite`, WAL by default | None |
| Closures | A capturing closure's environment is never freed | Avoid them on per-request paths, or fix the language |
| Crypto | `std:crypto`, vendored Monocypher 4.0.3 in `packages/crypto` | None |
| Sessions | `req_cookie`, `req_header`, `set_cookie`, and `clear_cookie` on HTTP/1.1 | None |
| Environment | `sys.env` reads a variable | None |
| Linux build | `zeus_plat.c` has Linux branches, but a server build on Linux is untested. `loamc` has no Linux cross target | Build on a Linux runner, and prove it in phase 7 |
| Deploy tools | `flyctl` is installed | None |

## Phases

### 0. Remove the Vercel code

- Delete `www/public/api/`, `www/public/package.json`,
  `www/tests/api_test.mjs`, and `www/tools/hash-password.mjs`.
- Remove the `api/` exclusion from `compress_web` in `zeli.loam`.
- Keep zeli's copy of `public/`. It is generic.
- Keep `vercel_config` in zeli while the static site stays on Vercel.
- Do not deploy www with `analytics.start` until phase 5: its posts would 404.

The removed SQL stays readable in `2b9723f`, as the reference for phase 5. One
later change to it is recorded under phase 5.

### 1. C FFI

Done. A compile_pass test calls vendored C, and a client build refuses a reachable
`#[c]` fn and keeps nothing of one only a `#[server]` body calls. `#[c]` covers what
phases 2 and 4 need. Calls from C back into Loam were left out, as nothing needs
them yet. The `std:objc` and Loam host work in the stash was not taken.

### 2. `std:sqlite`

Done. `packages/sqlite` vendors the amalgamation, and a compile_pass test covers
round trips, transactions, and text with quotes and NULs. It also covers the
recursive CTE and window functions that phase 5 needs, and `VACUUM INTO`. Writing
500,000 rows in one transaction and reading them back took 0.72 s on the dev Mac.

### 3. Server work in `std:http`

Done. `http_listen.loam` holds stalled clients open while others are served. In a
real process on the dev Mac, 50 stalled clients left about 7,600 requests a second,
and 127.0.0.1 was not reachable from the LAN unless the server opted in to 0.0.0.0.

- `app.listen(port, ListenProps { ... })` takes `host`, `max_body`, `timeout_ms`,
  and `max_conns`. A slow client gets 408, and a large body gets 413.
- `server_start` and `server_step` run the same loop one turn at a time, for tests.
- Cookies work on HTTP/1.1, which is what gRPC-Web uses here, not on h2c. Cookies
  default to `HttpOnly`, `Secure`, and `SameSite=Lax`.
- Replies still close the connection: there is no keep-alive.
- Non-blocking accepted sockets rely on an explicit call on Linux, where they do
  not inherit it from the listener. Prove the server on Linux in phase 7.

### 4. Crypto

Done. `packages/crypto` vendors Monocypher 4.0.3, which is CC0 or 2-clause BSD. Cure53
audited it in June 2020, before this version.

- `hash_password` and `verify_password`: Argon2id, 19 MiB and 2 passes, as OWASP
  recommends, in the standard `$argon2id$...` form that other tools read.
- `mac`: keyed BLAKE2b, for signing sessions. `digest`: plain BLAKE2b.
- `equal`: constant time, at any length. Never compare secrets with `==`.
- `random` and `random_token`: from the OS. They panic rather than return weak bytes.

Tests use the RFC 7693 and RFC 9106 vectors, and hashes from the reference Argon2
library, which also verified hashes made here. One password check took 35 ms on the
dev Mac and fits a 256 MB machine, but it blocks the server while it runs, and a
shared CPU will be slower.

### 5. Analytics server

Lives in `www/server/`, built as a native binary. The wasm never imports it.
It reads the port and database path from the environment.

| Call | Kind | Why |
|---|---|---|
| `/api/track` | `app.json` route | `sendBeacon` cannot set gRPC headers or frames |
| `Vault.Login`, `Vault.Logout`, `Vault.Stats` | gRPC-Web | Typed contract shared with the page |
| `/healthz` | `app.json` route | Fly's health check |

- Validate as the old `track.js` did: size cap, id format, known kinds, trimmed
  text, and browser clocks that are off.
- Write each batch in one transaction, to the `events` table.
- Port the report queries. Postgres `unnest`, `DISTINCT ON`, `generate_series`,
  and `make_interval` need SQLite forms, using window functions and a recursive
  CTE. Store times as integer milliseconds.
- The daily report returns every day of the window, with zero counts on quiet
  days, not gaps. `2b9723f` predates this: its query skips them. Test it: a
  30-day report has 30 rows.
- `?` needs the enclosing fn to return the same `Result` type, so `open` and
  `prepare` fill a `&mut` value, and return `Result<bool>` like every other call.
- Do not create capturing closures per request or per batch: each leaks its
  environment. Use `sqlite.begin` and `sqlite.finish`, and register handlers once.
- SQLite calls block, so keep each one short. Measure the report queries on a
  year of data, and add indexes or daily rollups if they stall other requests.
- Limit failed logins, and store no addresses. Each attempt runs Argon2id, which
  blocks every other request, so the limit also protects the server. Measure a
  check on the Fly machine.
- The password tool went with the Vercel code. Write one in Loam that prints a
  `crypto.hash_password` hash for `fly secrets set`.
- Secrets stay in the environment. No email, password, or hash in the repo.

### 6. www

- Point the vault page at the gRPC calls through `http.client("").call_async`.
- Drop `http.post_async` if nothing else needs it.
- Finish the unchecked items listed under "Where things stand".

### 7. Deploy to Fly.io

Needs the owner: a Fly account with a card, `fly auth login`, the login email
and password, and the region.

We do not use Docker: no Dockerfile, no image of our own, and no Docker on any
machine. Fly does start every machine from an image, so `fly.toml` names a stock
one, and `[[files]]` places our binary on it at deploy. Nothing is built on Fly.

Build the binary on a Linux runner, using GitHub Actions on `ubuntu-latest`:
`make`, then compile `www/server` with `loamc`. Link it statically, or it depends
on the base image's C library. The same job can run `fly deploy` with a
`FLY_API_TOKEN` secret.

`fly.toml`, in `www/server/`:

```toml
app = "loam-analytics"
primary_region = "<region>"

[build]
  image = "debian:bookworm-slim"

[experimental]
  cmd = ["/bin/sh", "-c", "chmod +x /app/server && exec /app/server"]

[[files]]
  guest_path = "/app/server"
  local_path = "server"

[env]
  PORT = "8080"
  DATABASE_PATH = "/data/analytics.db"

[http_service]
  internal_port = 8080
  force_https = true
  auto_stop_machines = "off"
  min_machines_running = 1

  [[http_service.checks]]
    method = "GET"
    path = "/healthz"
    interval = "30s"
    timeout = "5s"

[[vm]]
  size = "shared-cpu-1x"
  memory = "256mb"

[mounts]
  source = "analytics_data"
  destination = "/data"
  snapshot_retention = 14
```

Check these field names against Fly's `fly.toml` reference when writing the file.

Prove these first, with a small test binary. Fly's docs show `[[files]]` for
config files, not programs:

- A binary this size fits through `[[files]]`.
- It runs: whether the file arrives executable is unconfirmed, hence the `chmod`.

If either fails, copy the binary to the volume with `fly ssh sftp` and run it
from there. If Fly proves too awkward without images, the fallback is a plain
VPS, such as Hetzner at about $4.50-5 a month: copy the binary, and run it
under systemd.

Steps:

1. `fly apps create loam-analytics`
2. `fly volumes create analytics_data --size 1 --region <region>`
3. `fly secrets set` for the login email, password hash, and session secret.
4. Build the Linux binary, then `fly deploy` beside it. Keep it to one machine
   with `fly scale count 1`.
5. Point the site's `/api/*` at the server, and turn on `analytics.start` in the
   published build.
6. Run the browser checks against the live site.

Running it:

- One machine and one volume, because a volume attaches to a single machine and
  SQLite has one writer. A deploy restarts it, and beacons sent during the restart
  are lost. That is acceptable for analytics.
- Fly takes daily volume snapshots, but its docs say not to rely on them as the
  primary backup. Add a scheduled `VACUUM INTO`, and copy that file off the
  machine.
- Delete events older than 13 months, so the table cannot fill the disk. Watch
  the volume's use, as it can be extended.

### Cost

About $2-3 a month at low traffic. Checked 2026-11-06. Confirm on Fly's pricing
page, as sources disagree on some of these.

| Item | Per month |
|---|---|
| `shared-cpu-1x`, 256 MB | $1.94 on Fly's page, or $2.19 reported after an October rate change |
| 1 GB volume | $0.15 |
| Snapshots | $0.08/GB, first 10 GB free |
| Outbound traffic | $0.02/GB in North America and Europe, $0.04 in Asia Pacific, $0.12 in India |
| Shared IPv4, IPv6, first 10 certificates | Free |

256 MB is an assumption. Measure the server's memory in phase 5.

## Open decisions

1. **Same origin.** Recommended: keep the static site on Vercel and rewrite
   `/api/*` to the Fly app, through the `vercel.json` zeli writes. Same origin
   keeps cookies and `sendBeacon` simple. The alternative is to serve the whole
   site from the Loam server, which drops Vercel but means building static file
   serving and caching. Test that cookies and the host check pass through the rewrite.
2. **Region.** Near the visitors, but traffic from India costs the most per GB.
   Decide from who visits the site.

## Rules that hold for any backend

- The password is checked on the server. Anything in the wasm is public.
- The stats call requires a signed in session.
- No IP addresses or fingerprints are stored.
- The footer tells visitors that the site counts visits.
- Test each area with `./bin/nob test <filter>`, not the full suite.

## Sources for the Fly details

- [Resource pricing](https://fly.io/docs/about/pricing/)
- [Volume snapshots](https://fly.io/docs/volumes/snapshots/)
- [Public network services](https://fly.io/docs/networking/services/)
- [Listening ports](https://fly.io/docs/js/the-basics/listening-ports/)
- [App configuration](https://fly.io/docs/reference/configuration/)
- [Deploy an app](https://fly.io/docs/launch/deploy/)
