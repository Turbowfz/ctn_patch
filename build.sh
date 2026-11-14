#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Debian 构建入口；必须明确选择内核 profile 和独立工作目录。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
exec python3 -I "$HERE/tools/build_profile.py" "$@"
