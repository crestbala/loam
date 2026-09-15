# Zeus WebSocket demo (wasm)

A tiny end-to-end WebSocket demo: a native Yuga server pushes `tick-N` text
frames every 500 ms; a wasm page receives them through the browser's
`WebSocket` and repaints a counter + last message, live.

```
examples/zeus/ws/
├── run.sh                 # start backend + frontend, Ctrl-C stops both
├── backend/
│   └── server.loam        # native ws server on :8080 (one client, ticks)
└── frontend/
    ├── app.loam           # wasm UI: http.ws_open(client(""), "/ws", ...)
    ├── vite.config.js     # compiles app.loam -> wasm; proxies /ws -> :8080
    └── index.html         # canvas + loader
```

## Run

```sh
./examples/zeus/ws/run.sh
```

then open http://127.0.0.1:5173 — the page counts `messages:` as each
`tick-N` arrives and shows the latest frame text. `npm install` runs
automatically on first start.

## How the wasm socket works

`http.ws_open(http.client(""), "/ws", on_text, on_done)` on wasm hands the
same-origin URL to the JS loader (`new WebSocket("/ws")`). Inbound text
messages queue in JS; every animation frame the engine's async tick drains
the queue (`net.ws_count` / `ws_copy`) and delivers each message to the
callback on the UI thread — the same stepper model as native TCP.

Native WebSockets (RFC 6455 in pure Yuga, `httpcore/ws.loam`) are covered by
`packages/yuga/tests/compile_pass/zeus_stream.loam`.
