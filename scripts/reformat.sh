#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
SRC_DIR="${SCRIPT_DIR}/../src"

if [[ ! -d "${SRC_DIR}" ]]; then
  echo "ERROR: src dir not found: ${SRC_DIR}" >&2
  exit 1
fi

find "${SRC_DIR}" \
  \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.c" \) \
  -print0 \
  | xargs -0 clang-format -i

