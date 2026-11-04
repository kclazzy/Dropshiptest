#!/usr/bin/env bash
# Сборка ядра и расширения. Используется и при установке, и при обновлении.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S core-cpp -B core-cpp/build -DCMAKE_BUILD_TYPE=Release -DDROPSHIP_BUILD_TESTS=OFF
cmake --build core-cpp/build --target dropship_core_app -j"$(nproc 2>/dev/null || echo 2)"
if command -v npm >/dev/null 2>&1; then
  (cd extension-mv3 && npm ci --no-audit --no-fund && npm run build)
else
  echo "npm не найден: расширение не собрано (нужен Node.js 20+)" >&2
fi
