#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: $0 <AppBundle.app>"
  exit 1
fi

APP="$1"
BIN_DIR="$APP/Contents/MacOS"
BIN="$BIN_DIR/ArduinoEditor"
FW="$APP/Contents/Frameworks"

# Add any other binaries you ship next to ArduinoEditor here:
EXTRA_BINS=(
  "$BIN_DIR/clang-format"
  "$BIN_DIR/arduino-cli"
)

if [ ! -f "$BIN" ]; then
  echo "ERROR: binary not found: $BIN"
  exit 2
fi

mkdir -p "$FW"

abs_path() {
  # works if the file exists
  local p="$1"
  local d b
  d="$(cd "$(dirname "$p")" && pwd)"
  b="$(basename "$p")"
  echo "$d/$b"
}

get_rpaths() {
  local macho="$1"
  otool -l "$macho" 2>/dev/null |
    awk '$1=="cmd" && $2=="LC_RPATH"{in_rpath=1; next}
         in_rpath && $1=="path"{print $2; in_rpath=0}'
}

expand_special_path() {
  local p="$1"
  local from="$2"
  local from_dir
  from_dir="$(cd "$(dirname "$from")" && pwd)"

  case "$p" in
    @loader_path/*) echo "$from_dir/${p#@loader_path/}" ;;
    @executable_path/*)
      local bin_dir
      bin_dir="$(cd "$(dirname "$BIN")" && pwd)"
      echo "$bin_dir/${p#@executable_path/}"
      ;;
    *) echo "$p" ;;
  esac
}

resolve_dep_path() {
  local dep="$1"
  local from="$2"

  case "$dep" in
    @rpath/*)
      local base
      base="$(basename "$dep")"

      # 1) RPATHy z referencing souboru
      while read -r rp; do
        [ -z "$rp" ] && continue
        local rp_exp candidate
        rp_exp="$(expand_special_path "$rp" "$from")"
        candidate="$rp_exp/$base"
        if [ -f "$candidate" ]; then
          echo "$candidate"
          return 0
        fi
      done < <(get_rpaths "$from")

      # 2) fallbacky (Homebrew/klasika)
      # 2) fallbacky (Homebrew/klasika + LLVM)
      #    - pokud máš LLVM z Homebrew, knihovny bývají tady
      #    - pokud máš LLVM jinde, nastav env LLVM_PREFIX=/cesta/k/llvm
      EXTRA_SEARCH_DIRS=(
        "/opt/homebrew/lib"
        "/usr/local/lib"
        "/opt/homebrew/opt/brotli/lib"
        "/opt/homebrew/opt/curl/lib"
        "/opt/homebrew/opt/llvm/lib"
        "/usr/local/opt/llvm/lib"
      )

      if [ -n "${LLVM_PREFIX:-}" ]; then
        EXTRA_SEARCH_DIRS+=("$LLVM_PREFIX/lib")
      fi

      # pokud je v PATH systémový clang-format, přidej jeho ../lib (typicky Homebrew layout)
      if command -v clang-format >/dev/null 2>&1; then
        cf="$(command -v clang-format)"
        cf_dir="$(cd "$(dirname "$cf")" && pwd)"
        EXTRA_SEARCH_DIRS+=("$cf_dir/../lib")
      fi

      for d in "${EXTRA_SEARCH_DIRS[@]}"; do
        if [ -f "$d/$base" ]; then
          echo "$d/$base"
          return 0
        fi
      done

      echo "$dep"
      return 0
      ;;

    @loader_path/*|@executable_path/*)
      echo "$(expand_special_path "$dep" "$from")"
      return 0
      ;;

    *)
      echo "$dep"
      return 0
      ;;
  esac
}

is_system_lib() {
  case "$1" in
    /usr/lib/*|/System/*) return 0 ;;
    *) return 1 ;;
  esac
}

# ---------- PASS 1: copy closure (NO install_name_tool) ----------

TMPDIR="$(mktemp -d)"
SEEN_FILE="$TMPDIR/seen.txt"

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

: > "$SEEN_FILE"

QUEUE=()

enqueue_if_new() {
  local p="$1"
  p="$(abs_path "$p")"
  if ! grep -Fxq "$p" "$SEEN_FILE" 2>/dev/null; then
    echo "$p" >> "$SEEN_FILE"
    QUEUE+=("$p")
  fi
}

# Seed queue with main binary + extra binaries
enqueue_if_new "$BIN"
for b in "${EXTRA_BINS[@]}"; do
  if [ -f "$b" ]; then
    enqueue_if_new "$b"
  else
    echo "WARNING: extra binary not found (skipping): $b" >&2
  fi
done

echo "PASS 1: collecting & copying dependency closure..."
qi=0
while [ $qi -lt ${#QUEUE[@]} ]; do
  macho="${QUEUE[$qi]}"
  qi=$((qi + 1))

  [ -f "$macho" ] || continue

  otool -L "$macho" | tail -n +2 | awk '{print $1}' | while IFS= read -r dep; do
    [ -n "$dep" ] || continue
    is_system_lib "$dep" && continue

    resolved="$(resolve_dep_path "$dep" "$macho")"
    is_system_lib "$resolved" && continue

    if [ ! -f "$resolved" ]; then
      echo "  WARNING: can't resolve $dep (resolved to '$resolved') from '$macho'" >&2
      continue
    fi

    base="$(basename "$resolved")"
    dest="$FW/$base"

    if [ ! -f "$dest" ]; then
      echo "  copy: $resolved -> $dest"
      cp -L "$resolved" "$dest"
      chmod 755 "$dest"
    fi

    # enqueue the resolved dylib itself so we also scan ITS deps
    enqueue_if_new "$resolved"
  done
done


# ---------- PASS 2: rewrite ids + deps + rpaths ----------

echo "PASS 2: rewriting install names and dependency paths..."

ensure_bin_rpath() {
  local macho="$1"
  # Ensure binary can find Frameworks
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$macho" 2>/dev/null || true
}

# 1) Ensure all shipped executables can find Frameworks
ensure_bin_rpath "$BIN"
for b in "${EXTRA_BINS[@]}"; do
  [ -f "$b" ] && ensure_bin_rpath "$b"
done

# 2) For each bundled dylib: set id + add @loader_path rpath (so dylibs can resolve @rpath deps too)
shopt -s nullglob
for lib in "$FW"/*.dylib; do
  base="$(basename "$lib")"
  install_name_tool -id "@rpath/$base" "$lib"
  install_name_tool -add_rpath "@loader_path" "$lib" 2>/dev/null || true
done
shopt -u nullglob

# 3) Rewrite dependencies in BIN + extra bins + all bundled dylibs to @rpath/<basename> if that basename exists in Frameworks
rewrite_macho() {
  local macho="$1"
  otool -L "$macho" | tail -n +2 | awk '{print $1}' | while IFS= read -r dep; do
    [ -z "$dep" ] && continue
    if is_system_lib "$dep"; then
      continue
    fi
    local base
    base="$(basename "$dep")"
    if [ -f "$FW/$base" ]; then
      install_name_tool -change "$dep" "@rpath/$base" "$macho" 2>/dev/null || true
    fi
  done
}

rewrite_macho "$BIN"
for b in "${EXTRA_BINS[@]}"; do
  [ -f "$b" ] && rewrite_macho "$b"
done

shopt -s nullglob
for lib in "$FW"/*.dylib; do
  rewrite_macho "$lib"
done
shopt -u nullglob

echo "Done. Now codesign the app (Frameworks first, then the .app)."

