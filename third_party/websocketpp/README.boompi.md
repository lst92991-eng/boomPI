# WebSocket++ 0.8.2

This directory contains the unmodified public headers from Ubuntu package
`libwebsocketpp-dev_0.8.2-4_amd64.deb`.

- Upstream: https://github.com/zaphoyd/websocketpp
- Version: 0.8.2
- Package SHA-256: `8cf80efca15a184e9ed2676c013eaed771046c377c79e4b53d7b64824649f9a2`
- License: BSD-3-Clause, with bundled MIT/Zlib components as recorded in
  `NOTICE.debian`

The library is header-only. boomPI uses external Boost 1.74 headers and the
pinned OpenSSL package selected by CMake; no WebSocket++ or Boost runtime
library is installed on the board.
