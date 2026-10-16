#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -x "$REPO_DIR/build_monitor/bhuman_monitor" ]]; then
  echo '请先运行 scripts/build_monitor.sh 编译监视器。' >&2
  exit 1
fi
exec "$REPO_DIR/build_monitor/bhuman_monitor" "$@"
