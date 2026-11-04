#!/usr/bin/env bash
# Обновление до последней версии: забирает изменения и пересобирает. Данные (~/.dropship) не трогает.
set -euo pipefail
cd "$(dirname "$0")/.."
git pull --ff-only
"$(dirname "$0")/build.sh"
