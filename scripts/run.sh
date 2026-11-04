#!/usr/bin/env bash
# Запуск ядра. Если ядро завершилось с кодом 75 (кнопка «Обновить» в расширении),
# скрипт обновляет программу и запускает её снова.
cd "$(dirname "$0")/.."
export DROPSHIP_LAUNCHER=1
BIN=core-cpp/build/dropship_core_app
while true; do
  "$BIN" "$@"
  code=$?
  if [ "$code" -eq 75 ]; then
    echo "== Обновление =="
    if scripts/update.sh; then echo "== Обновлено, запускаю заново =="; else echo "== Обновление не удалось, запускаю прежнюю версию =="; fi
    continue
  fi
  exit "$code"
done
