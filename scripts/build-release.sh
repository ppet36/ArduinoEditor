#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_DIR/build"

ARM_HOST="petrzel@offline2"
ARM_REPO="/Volumes/XCODE/Users/petrzel/develop/git/ArduinoEditor"
RPI_HOST="petrzel@192.168.1.234"
RPI_REPO="/home/petrzel/develop/git/ArduinoEditor"
DOCKER_IMAGE="arduinoeditor-appimg:ubuntu2004"
BRANCH="main"

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10)
TEMP_DIR=""
CLEAN_LOCALIZATION=0

die() {
    echo "ERROR: $*" >&2
    exit 1
}

step() {
    echo
    echo "==> $*"
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

require_file() {
    [ -s "$1" ] || die "Expected artifact is missing or empty: $1"
}

run_remote() {
    host="$1"
    remote_command="$2"
    quoted_command="$(printf '%q' "$remote_command")"

    # A login shell loads ~/.bash_profile, which provides the Homebrew PATH on
    # the Apple Silicon builder (notably wx-config).
    ssh "${SSH_OPTS[@]}" "$host" "bash -lc $quoted_command"
}

cleanup() {
    status=$?
    trap - EXIT INT TERM

    if [ "$CLEAN_LOCALIZATION" -eq 1 ]; then
        git -C "$REPO_DIR" checkout -- \
            resources/localization/ArduinoEditor.pot \
            resources/localization/cs_CZ.po >/dev/null 2>&1 || true
    fi

    if [ -n "$TEMP_DIR" ] && [ -d "$TEMP_DIR" ]; then
        rm -rf "$TEMP_DIR"
    fi

    exit "$status"
}

trap cleanup EXIT INT TERM

read_version() {
    VERSION="$(sed -n 's/^AE_VERSION[[:space:]]*:=[[:space:]]*//p' "$BUILD_DIR/version.mk")"
    VERSION_MAJOR="$(sed -n 's/^AE_VERSION_MAJOR[[:space:]]*:=[[:space:]]*//p' "$BUILD_DIR/version.mk")"
    VERSION_MINOR="$(sed -n 's/^AE_VERSION_MINOR[[:space:]]*:=[[:space:]]*//p' "$BUILD_DIR/version.mk")"
    VERSION_PATCH="$(sed -n 's/^AE_VERSION_PATCH[[:space:]]*:=[[:space:]]*//p' "$BUILD_DIR/version.mk")"

    [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
        die "Invalid AE_VERSION in build/version.mk: $VERSION"
    [[ "$VERSION_MAJOR" =~ ^[0-9]+$ ]] || die "Invalid AE_VERSION_MAJOR: $VERSION_MAJOR"
    [[ "$VERSION_MINOR" =~ ^[0-9]+$ ]] || die "Invalid AE_VERSION_MINOR: $VERSION_MINOR"
    [[ "$VERSION_PATCH" =~ ^[0-9]+$ ]] || die "Invalid AE_VERSION_PATCH: $VERSION_PATCH"

    [ "$VERSION" = "$VERSION_MAJOR.$VERSION_MINOR.$VERSION_PATCH" ] ||
        die "AE_VERSION ($VERSION) does not match its major/minor/patch fields"
}

remote_preflight() {
    host="$1"
    repo="$2"
    expected_os="$3"
    expected_arch="$4"
    shift 4

    remote_commands=""
    for command_name in "$@"; do
        remote_commands="$remote_commands command -v '$command_name' >/dev/null 2>&1 || { echo 'Missing remote command: $command_name' >&2; exit 1; };"
    done

    run_remote "$host" \
        "set -e; test -d '$repo/.git'; test \"\$(uname -s)\" = '$expected_os'; test \"\$(uname -m)\" = '$expected_arch'; $remote_commands"
}

sync_remote() {
    host="$1"
    repo="$2"

    run_remote "$host" \
        "set -e; cd '$repo'; git checkout '$BRANCH'; git fetch origin '$BRANCH'; git reset --hard 'origin/$BRANCH'; git pull --ff-only origin '$BRANCH'; test \"\$(git rev-parse HEAD)\" = '$COMMIT'; test -z \"\$(git status --porcelain --untracked-files=all)\""
}

cd "$REPO_DIR"

step "Preflight"
[ "$(uname -s)" = "Darwin" ] || die "Release orchestrator must run on macOS"
[ "$(uname -m)" = "x86_64" ] || die "Release orchestrator must run on an Intel Mac"

for command_name in git make sed docker ssh scp ditto file unzip x86_64-w64-mingw32-g++ makensis osslsigncode xcrun pkgutil; do
    require_command "$command_name"
done

[ "$(git branch --show-current)" = "$BRANCH" ] || die "The local branch must be $BRANCH"
[ -z "$(git status --porcelain --untracked-files=all)" ] || die "The local working tree is not clean"

git fetch origin "$BRANCH"
COMMIT="$(git rev-parse HEAD)"
[ "$COMMIT" = "$(git rev-parse "origin/$BRANCH")" ] || die "Local HEAD does not match origin/$BRANCH"

read_version
[ -f "$BUILD_DIR/winsign/cert.pem" ] || die "Windows signing certificate is missing"
[ -f "$BUILD_DIR/winsign/key.pem" ] || die "Windows signing key is missing"

docker info >/dev/null
[ "$(docker image inspect "$DOCKER_IMAGE" --format '{{.Architecture}}')" = "amd64" ] ||
    die "Docker image $DOCKER_IMAGE is missing or is not amd64"

remote_preflight "$ARM_HOST" "$ARM_REPO" Darwin arm64 git make ditto file
remote_preflight "$RPI_HOST" "$RPI_REPO" Linux aarch64 git make appimagetool dpkg-deb file

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/arduinoeditor-release.XXXXXX")"
CLEAN_LOCALIZATION=1

MAC_PKG="$BUILD_DIR/ArduinoEditor-$VERSION-macos.pkg"
WIN_EXE="$BUILD_DIR/ArduinoEditorSetup-x86_64-$VERSION.exe"
WIN_ZIP="$BUILD_DIR/ArduinoEditor-Windows-x86_64-$VERSION.zip"
LINUX_APPIMAGE="$BUILD_DIR/ArduinoEditor-Linux-x86_64-$VERSION.AppImage"
RPI_APPIMAGE="$BUILD_DIR/ArduinoEditor-rpi-aarch64-$VERSION.AppImage"
RPI_DEB="$BUILD_DIR/arduino-editor_rpi_arm64_$VERSION.deb"

step "macOS Intel bundle"
make -C "$BUILD_DIR" -f Makefile.macos clean
make -C "$BUILD_DIR" -f Makefile.macos bundle
[ -x "$BUILD_DIR/ArduinoEditor-x86_64.app/Contents/MacOS/ArduinoEditor" ] || die "Intel app bundle was not created"
file "$BUILD_DIR/ArduinoEditor-x86_64.app/Contents/MacOS/ArduinoEditor" | grep -q 'x86_64' || die "Intel app bundle has the wrong architecture"

step "macOS Apple Silicon bundle"
sync_remote "$ARM_HOST" "$ARM_REPO"
ARM_ARCHIVE="/tmp/ArduinoEditor-arm64-$VERSION-$COMMIT.zip"
run_remote "$ARM_HOST" \
    "set -e; cd '$ARM_REPO/build'; make -f Makefile.macos clean; make -f Makefile.macos bundle; test -x ArduinoEditor-arm64.app/Contents/MacOS/ArduinoEditor; file ArduinoEditor-arm64.app/Contents/MacOS/ArduinoEditor | grep -q arm64; rm -f '$ARM_ARCHIVE'; ditto -c -k --keepParent ArduinoEditor-arm64.app '$ARM_ARCHIVE'"
scp "${SSH_OPTS[@]}" "$ARM_HOST:$ARM_ARCHIVE" "$TEMP_DIR/ArduinoEditor-arm64.zip"
run_remote "$ARM_HOST" "rm -f '$ARM_ARCHIVE'"
rm -rf "$BUILD_DIR/ArduinoEditor-arm64.app"
ditto -x -k "$TEMP_DIR/ArduinoEditor-arm64.zip" "$BUILD_DIR"
[ -x "$BUILD_DIR/ArduinoEditor-arm64.app/Contents/MacOS/ArduinoEditor" ] || die "Apple Silicon app bundle was not transferred"
file "$BUILD_DIR/ArduinoEditor-arm64.app/Contents/MacOS/ArduinoEditor" | grep -q 'arm64' || die "Apple Silicon app bundle has the wrong architecture"

step "Signed and notarized macOS package"
rm -f "$MAC_PKG"
make -C "$BUILD_DIR" -f Makefile.macos release-pkg-dual
require_file "$MAC_PKG"
pkgutil --check-signature "$MAC_PKG" >/dev/null
xcrun stapler validate "$MAC_PKG" >/dev/null

step "Windows installer and ZIP"
make -C "$BUILD_DIR" -f Makefile.win64 clean
make -C "$BUILD_DIR" -f Makefile.win64 release_win64
require_file "$WIN_EXE"
require_file "$WIN_ZIP"
unzip -tq "$WIN_ZIP" >/dev/null

step "Linux AppImage"
rm -f "$LINUX_APPIMAGE"
docker run --rm -v "$REPO_DIR:/work" "$DOCKER_IMAGE" \
    bash -lc 'set -e; test "$(uname -m)" = x86_64; cd /work/build; make -f Makefile.linux clean; make -f Makefile.linux appimage'
require_file "$LINUX_APPIMAGE"
file "$LINUX_APPIMAGE" | grep -q 'x86-64' || die "Linux AppImage has the wrong architecture"

step "Raspberry Pi AppImage and DEB"
sync_remote "$RPI_HOST" "$RPI_REPO"
run_remote "$RPI_HOST" \
    "set -e; cd '$RPI_REPO/build'; make -f Makefile.rpi clean; make -f Makefile.rpi; test -s 'ArduinoEditor-rpi-aarch64-$VERSION.AppImage'; test -s 'arduino-editor_rpi_arm64_$VERSION.deb'; file 'ArduinoEditor-rpi-aarch64-$VERSION.AppImage' | grep -q aarch64; dpkg-deb --info 'arduino-editor_rpi_arm64_$VERSION.deb' >/dev/null"
rm -f "$RPI_APPIMAGE" "$RPI_DEB"
scp "${SSH_OPTS[@]}" \
    "$RPI_HOST:$RPI_REPO/build/ArduinoEditor-rpi-aarch64-$VERSION.AppImage" \
    "$RPI_HOST:$RPI_REPO/build/arduino-editor_rpi_arm64_$VERSION.deb" \
    "$BUILD_DIR/"
require_file "$RPI_APPIMAGE"
require_file "$RPI_DEB"

git checkout -- \
    resources/localization/ArduinoEditor.pot \
    resources/localization/cs_CZ.po
CLEAN_LOCALIZATION=0
[ -z "$(git status --porcelain --untracked-files=all)" ] ||
    die "The release build left unexpected changes in the working tree"

step "Release $VERSION completed"
for artifact in "$MAC_PKG" "$WIN_EXE" "$WIN_ZIP" "$LINUX_APPIMAGE" "$RPI_APPIMAGE" "$RPI_DEB"; do
    echo "  ${artifact#$REPO_DIR/}"
done
