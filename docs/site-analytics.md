# Site analytics plan

Analytics for the www site, moving from Vercel Functions to a Loam backend on
Fly.io: async gRPC and HTTP on `std:http`, with C SQLite reached through FFI.

Live since 2026-11-06. The site is on Vercel at https://loam-lang.vercel.app, which
rewrites `/api/*` and `/Vault/*` to the server on Fly.io. What is left is at the end
of phase 7.

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
| `std:analytics` and the web host | Done |
| Vercel functions, API test, password tool | Removed. The server replaces them |
| Vault page, footer door, `analytics.start` in www | Done. The page uses the gRPC calls |
| `on_submit` on inputs | Done |
| Analytics server | `www/server/`, tested by `www_analytics.loam`. Deployed |

The vault page was tested in a browser against the real server, behind a local
same-origin proxy. What is left is to repeat that on the live site, in phase 7.

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
| Linux build | A static x86-64 server, built by `analytics-server.yml`, which runs its tests on Linux. `loamc` has no Linux cross target | None |
| Deploy tools | `flyctl` is installed | None |

## Phases

### 0. Remove the Vercel code

- Delete `www/public/api/`, `www/public/package.json`,
  `www/tests/api_test.mjs`, and `www/tools/hash-password.mjs`.
- Remove the `api/` exclusion from `compress_web` in `zeli.loam`.
- Keep zeli's copy of `public/`. It is generic.
- Keep `vercel_config` in zeli while the static site stays on Vercel.
- Do not deploy www with `analytics.start` until phase 5: its posts would 404.

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

Done. It lives in `www/server/` and is built as a native binary. The wasm never
imports it. `main.loam` lists its environment, and `hash_password.loam` prints the
hash for `fly secrets set`, reading the password from standard input.

| Call | Kind | Why |
|---|---|---|
| `/api/track` | `app.json` route | `sendBeacon` cannot set gRPC headers or frames |
| `Vault.Login`, `Vault.Logout`, `Vault.Stats` | gRPC-Web | Typed contract shared with the page |
| `/healthz` | `app.json` route | Fly's health check |

- `www_analytics.loam` drives it through a real socket, with the fixture and numbers
  the Postgres functions passed. A 30-day report has 30 rows, with zeros on quiet days.
- Times are integer milliseconds. Ids are stored as 60-bit integers, the last 15 hex
  digits of each id, which keeps the table small and grouping fast.
- Reports stop at 90 days, the widest the page offers.
- Events older than 13 months are deleted once a day, on the next beacon.
- 20 failed logins in 15 minutes lock sign in for everyone, as no addresses are
  stored. A flood can lock the owner out, but a session already signed in still
  works, and a locked attempt costs no hash.
- With `SITE_ORIGIN` set, a beacon's `Origin` must equal it. A rewrite changes `Host`,
  so production has to set it.
- The server is one thread, and a panic stops it. Bytes from the network must not
  reach arithmetic that can overflow. Fuzzing 6,000 mutated HTTP/1.1, gRPC-Web, h2c,
  and WebSocket requests against the real binary found no crash or stall.

Measured on the dev Mac in a real process, with 4.4 million rows, a year at about
2,000 sessions a day:

| Measure | Result |
|---|---|
| Sign in | 40 ms |
| Report, 7 / 30 / 90 days | 0.33 s / 1.5 s / 4.9 s |
| Beacons, 10 events each | About 1,250 a second |
| Memory | 35 MB after 40,000 beacons, flat. After 8,000 reports on a small database, a physical footprint of 10 MB, with report time flat and `leaks` finding none |
| Database file | 473 MB |

A report costs about 3.3 µs for each row in its window, spread evenly over its
queries, so an index will not help. Daily rollups were not added: visitors and
sessions do not add up across days, so rollups would change the numbers. Only a
signed in owner can start a report. Connections wait in the listen backlog meanwhile,
and of 150 beacons sent during a 90-day report, 128 were stored and 22 timed out at
the backlog's limit. A site with a tenth of that traffic reports in about 0.5 s here.
A shared CPU will be slower, so measure a report on the Fly machine. If a report ever
takes minutes or fills the backlog, reconsider rollups or shorten the window.

### 6. www

Done. The vault page imports `www/server/api.loam` for the contract, and only that
file: the rest of `www/server/` is native. It calls the server through
`http.client("").call_async`. `http.post_async` went with the old routes.

Replies carry `status` from `api.loam`, where 0 means no answer came, so a proxy
error or a stopped server shows as a failure and never as an empty report. The page
only looks signed out once the server has confirmed it.

Checked in headless Chrome, with the built wasm, a real server, and a same-origin
proxy: a wrong password, then a right one, the stat grid and charts, the 7, 30, and
90 day switch, the tables, staying signed in across a reload, sign out, the lockout
message, a stopped server, the footer door, and a 390 px screen. The session cookie
is hidden from page scripts, and the owner's browser opts out of counting.

A real browser's beacons were not checked, as `analytics.start` does not report from
localhost or from automated browsers. The server takes the fields `loader.js`
sends, checked by reading it, so check real beacons on the live site in phase 7.

### 7. Deploy to Fly.io

Needs the owner: a Fly account with a card, `fly auth login`, the login email
and password, and the region.

We do not use Docker: no Dockerfile, no image of our own, and no Docker on any
machine. Fly does start every machine from an image, so `www/server/fly.toml` names
a stock one. Nothing is built on Fly.

`.github/workflows/analytics-server.yml` builds the binary on a Linux runner, on
`ubuntu-latest`, and links it statically with `LOAM_LINK_EXTRA=-static`, or it would
depend on the base image's C library. It runs the server's tests and
`www/server/smoke.sh` on Linux, and keeps the binary as an artifact for 14 days. It
runs by hand, not on pushes or PRs: `gh workflow run analytics-server.yml --ref <branch>`.

The plan to place the binary with `[[files]]` failed: Fly answers `request body too
large` to the 2.5 MB binary, and to 1.2 MB gzipped. So the binary lives on the volume.
The machine runs `/data/server`, and waits if it is not there yet.

To deploy, use `www/deploy.sh site`, `server`, or `all`. `check` tests that a deploy
could run, and changes nothing. `server` refuses a build older than the server code,
so commit, push, run the workflow, and wait for it first.

The app, volume, and secrets are set up in `sin`, the closest region to India. A real
visit through Vercel has been counted, and sign in works on the live site.

The first browser sign in got 431: `curl` had passed, but a browser behind Vercel
sends over 40 headers. Test paths a browser uses with a browser's headers.

Left to do:

1. Back up the database. See "Running it".
2. Measure sign in and a report on the Fly machine.

Running it:

- One machine and one volume, because a volume attaches to a single machine and
  SQLite has one writer. A deploy restarts it, and beacons sent during the restart
  are lost. That is acceptable for analytics.
- `HOST` must be `0.0.0.0`. The server binds 127.0.0.1 unless told otherwise, and
  Fly's proxy could not reach it there.
- A `/healthz` check that lands during a long report can fail once. That only
  matters if checks start restarting the machine.
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

256 MB has room: see the memory measured in phase 5.

## Open decisions

1. **Same origin.** Recommended: keep the static site on Vercel and rewrite
   `/api/*` and `/Vault/*` to the Fly app, through the `vercel.json` zeli writes, from `backend` and `backend_paths` in
   `www/zeus.toml`. Same origin keeps cookies and `sendBeacon` simple. The alternative is to serve the whole
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
