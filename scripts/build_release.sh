#!/bin/sh
# 在 Linux/WSL 中生成同一版本的客户端、服务端和可安装目录；不连接开发板。
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

preset=${1:-rv1106-release}
build=build/rv1106
release=build/release
cmake --preset "$preset" -B "$build" -G "Unix Makefiles"
cmake --build "$build" --parallel
compiler=$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' "$build/CMakeCache.txt")
[ -n "$compiler" ] || compiler=$(sed -n 's/^CMAKE_CXX_COMPILER:STRING=//p' "$build/CMakeCache.txt")
readelf="${compiler%g++}readelf"
python3 scripts/verify_rv1106_elf.py "$build/client/boompi-client" \
  --readelf "$readelf" --max-glibcxx 3.4.25
mkdir -p "$release"
DESTDIR="$(pwd)/$release/rootfs" cmake --install "$build"
cp "$build/client/boompi-client" "$release/boompi-client"
go_command="${BOOMPI_GO:-go}"
(
  cd server
  CGO_ENABLED=0 GOOS=windows GOARCH=amd64 "$go_command" build -trimpath -buildvcs=false \
    -ldflags="-s -w" -o "../$release/boompi-server.exe" ./cmd/boompi-server
)
sha256sum "$release/boompi-client" "$release/boompi-server.exe"
echo "Release prepared in $release; board deployment remains manual."
