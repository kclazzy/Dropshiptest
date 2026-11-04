#!/usr/bin/env bash
# Команды управления: scripts/dropship.sh store-add|store-list|variants|link ...  (справка: scripts/dropship.sh help)
exec "$(dirname "$0")/../core-cpp/build/dropship_core_app" "$@"
