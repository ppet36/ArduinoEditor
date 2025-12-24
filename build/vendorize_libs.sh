#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: $0 <AppBundle.app>"
  exit 1
fi

APP="$1"
BIN="$APP/Contents/MacOS/ArduinoEditor"
FW="$APP/Contents/Frameworks"

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
      for d in \
        "/opt/homebrew/lib" \
        "/usr/local/lib" \
        "/opt/homebrew/opt/brotli/lib" \
        "/opt/homebrew/opt/curl/lib"
      do
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
QUEUE_FILE="$TMPDIR/queue.txt"
SEEN_FILE="$TMPDIR/seen.txt"

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

echo "$(abs_path "$BIN")" > "$QUEUE_FILE"
: > "$SEEN_FILE"

enqueue_if_new() {
  local p="$1"
  p="$(abs_path "$p")"
  if ! grep -Fxq "$p" "$SEEN_FILE" 2>/dev/null; then
    echo "$p" >> "$SEEN_FILE"
    echo "$p" >> "$QUEUE_FILE"
  fi
}

# initial mark
echo "$(abs_path "$BIN")" >> "$SEEN_FILE"

echo "PASS 1: collecting & copying dependency closure..."
while IFS= read -r macho || [ -n "$macho" ]; do
  [ -z "$macho" ] && continue
  [ -f "$macho" ] || continue

  # iterate deps
  otool -L "$macho" | tail -n +2 | awk '{print $1}' | while IFS= read -r dep; do
    [ -z "$dep" ] && continue
    if is_system_lib "$dep"; then
      continue
    fi

    resolved="$(resolve_dep_path "$dep" "$macho")"
    if is_system_lib "$resolved"; then
      continue
    fi

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
      enqueue_if_new "$resolved"
    else
      # collision sanity check (same basename from different path)
      if ! cmp -s "$resolved" "$dest"; then
        echo "  WARNING: basename collision for $base" >&2
        echo "           existing: $dest" >&2
        echo "           new src : $resolved" >&2
      fi
    fi
  done
done < "$QUEUE_FILE"

# ---------- PASS 2: rewrite ids + deps + rpaths ----------

echo "PASS 2: rewriting install names and dependency paths..."

# 1) Ensure main binary can find Frameworks
install_name_tool -add_rpath "@executable_path/../Frameworks" "$BIN" 2>/dev/null || true

# 2) For each bundled dylib: set id + add @loader_path rpath (so dylibs can resolve @rpath deps too)
shopt -s nullglob
for lib in "$FW"/*.dylib; do
  base="$(basename "$lib")"
  install_name_tool -id "@rpath/$base" "$lib"
  install_name_tool -add_rpath "@loader_path" "$lib" 2>/dev/null || true
done
shopt -u nullglob

# 3) Rewrite dependencies in BIN + all bundled dylibs to @rpath/<basename> if that basename exists in Frameworks
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
shopt -s nullglob
for lib in "$FW"/*.dylib; do
  rewrite_macho "$lib"
done
shopt -u nullglob

echo "Done. Now codesign the app (Frameworks first, then the .app)."

