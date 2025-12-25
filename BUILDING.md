# Building Arduino Editor

This document describes how to build Arduino Editor from source on each
supported platform.

The project uses platform-specific build logic in the `build/` directory.
Most builds are expected to be run on the target platform, with one exception:
**Windows is cross-compiled on macOS**.

## Repository layout

```
src/        Application source code
build/      Makefiles and platform-specific build logic (run builds from here)
resources/  Icons and bundled assets
doc/        Documentation and design notes
```

## General notes

- Build commands are executed from the `build/` directory.
- The build outputs and packaging artifacts are typically written to a
  platform-specific output directory.
- Some packaging steps may require platform credentials (code signing,
  notarization).

---

## macOS (native)

### Requirements

* macOS with Xcode Command Line Tools installed (`xcode-select --install`)
* Homebrew toolchain and dependencies (the build uses the Homebrew-provided versions)

Install required packages:

```bash
brew install wxwidgets llvm curl nlohmann-json
```

Notes:

* wxWidgets 3.3.x is provided by `brew install wxwidgets`
* LLVM/clang is provided by `brew install llvm`
* libcurl is provided by `brew install curl`
* nlohmann-json is provided by `brew install nlohmann-json`

### Developer workflow (local build)

For local development, it is sufficient to run the default Makefile target.

From the `build/` directory:

```bash
make
```

This produces a runnable `ArduinoEditor` binary for the current architecture, without creating an app bundle or performing any signing.

### Build app bundle (single-arch)

Intel and Apple Silicon bundles are built on separate machines (or separate environments), one per architecture.

On each machine, run:

```bash
cd build
make -f Makefile.macos bundle
```

This produces an app bundle next to the Makefile:

* `ArduinoEditor-x86_64.app` on Intel
* `ArduinoEditor-arm64.app` on Apple Silicon

### Dual-arch build (universal packaging input)

To create a dual/universal release, you need **both** single-arch `.app` bundles:

1. Build `ArduinoEditor-x86_64.app` on an Intel Mac (`make -f Makefile.macos bundle`)
2. Build `ArduinoEditor-arm64.app` on an Apple Silicon Mac (`make -f Makefile.macos bundle`)
3. Copy both bundles into `build/` on one of the machines (so both `.app` directories are present side-by-side)

After that, you can run the dual-arch packaging targets on that machine.

### Signing / notarization prerequisites

Creating distributable DMG/PKG releases requires Apple Developer Program membership and signing certificates:

* **Developer ID Application**
* **Developer ID Installer**

These must be installed in your Keychain on the machine performing the final packaging/signing.

The certificate identities and Team ID can be overridden on the command line (otherwise the Makefile defaults are used), e.g.:

```bash
make -f Makefile.macos release-pkg-dual \
  SIGN_ID_APP="Developer ID Application: Someone Else (ABCDE12345)" \
  SIGN_ID_PKG="Developer ID Installer: Someone Else (ABCDE12345)" \
  TEAM_ID="ABCDE12345"
```

### Release packaging (dual-arch)

From `build/` (where both `ArduinoEditor-x86_64.app` and `ArduinoEditor-arm64.app` are available):

Create a DMG:

```bash
make -f Makefile.macos release-dmg-dual
```

Create a PKG:

```bash
make -f Makefile.macos release-pkg-dual
```

---

## Linux (native)

### Developer workflow (local build)

For local development on Linux, install a reasonably recent system wxWidgets and Clang, then build using the default
Makefile target.

Minimum recommended versions:

* wxWidgets: **3.2.8** or newer (wxGTK)
* Clang/LLVM: **19** or newer

Build:

```bash
cd build
make
```

This produces a runnable binary at:

* `Linux-x86_64/ArduinoEditor`

### Release packaging (AppImage)

The official Linux AppImage is built in a container to ensure a consistent toolchain and runtime compatibility.
The repository includes `Dockerfile.appimage`, which uses Ubuntu 20.04 and installs/builds the required components
into `/opt` inside the image:

* wxWidgets 3.3.1 (built from source)
* prebuilt Clang/LLVM 19.1.7
* arduino-cli 1.3.1

Build the Docker image (from the repository root):

```bash
docker build -t arduinoeditor-appimg:ubuntu2004 -f Dockerfile.appimage .
```

Run an interactive container with the repository mounted at `/work`:

```bash
docker run --rm -it -v "$PWD":/work arduinoeditor-appimg:ubuntu2004
```

Inside the container, build the AppImage:

```bash
cd /work/build
make -f Makefile.linux appimage
```

Notes:

* This workflow is commonly run on macOS using a lightweight Linux VM runtime such as **Colima**.
  (The build itself happens inside the Linux container.)

### Packaging targets

From `build/`:

```bash
make -f Makefile.linux appimage
make -f Makefile.linux tarball
```
---

## Raspberry Pi (Linux ARM64)

### Requirements

* Raspberry Pi OS (64-bit) or another Debian-based ARM64 distro
* Standard development toolchain

Install build dependencies:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build \
  libgtk-3-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libjpeg-dev libtiff-dev libpng-dev \
  libnotify-dev libxtst-dev \
  libcurl4-openssl-dev \
  libsecret-1-dev
```

Install additional packages used by Arduino Editor:

```bash
sudo apt install -y \
  clang-19 clang-format-19 \
  nlohmann-json3-dev \
  curl
```

### wxWidgets (custom build)

The system wxWidgets package is **not** used on Raspberry Pi, because it is typically not built with the options
required by Arduino Editor (notably `wxSecretStore`). Instead, build wxWidgets from source and install it under
`/opt/wx-3.3/`.

Example build (wxWidgets 3.3.1):

```bash
cd /tmp
wget -O wxWidgets-3.3.1.tar.bz2 https://github.com/wxWidgets/wxWidgets/releases/download/v3.3.1/wxWidgets-3.3.1.tar.bz2
tar -xjf wxWidgets-3.3.1.tar.bz2
cd wxWidgets-3.3.1

mkdir -p build-gtk-ninja
cd build-gtk-ninja

cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DwxBUILD_SHARED=ON \
  -DwxBUILD_TOOLKIT=gtk3 \
  -DwxUSE_WEBREQUEST=ON \
  -DwxUSE_SECURE_STORAGE=ON \
  -DwxUSE_STC=ON \
  -DwxBUILD_SAMPLES=OFF \
  -DwxBUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=/opt/wx-3.3

cmake --build . -j "$(nproc)"
sudo cmake --install .
```

Notes:

* `wxSecretStore` on Linux uses the Secret Service API. Installing `libsecret-1-dev` **before** configuring wxWidgets
  is important so wxWidgets can detect it and enable secret-store support.
* If you need to verify what wxWidgets you are using later, check `/opt/wx-3.3/bin/wx-config` and ensure your build
  picks it up (PATH / wx-config selection depends on your Makefile setup).

### Build

From the `build/` directory:

* Default target builds **both** DEB and AppImage:

```bash
make -f Makefile.rpi
```

* Build DEB package only:

```bash
make -f Makefile.rpi deb
```

* Build AppImage only:

```bash
make -f Makefile.rpi appimage
```
---

## Windows (x86_64) cross-build on macOS

This project supports cross-compiling the Windows x86_64 build on macOS using Homebrew + MinGW-w64.

### Requirements

* macOS with Xcode Command Line Tools installed (`xcode-select --install`)
* Homebrew
* MinGW-w64 toolchain:

```bash
brew install mingw-w64
```

Additional tools used by optional packaging targets:

```bash
brew install nsis osslsigncode
```

### wxWidgets (cross-compiled, static)

wxWidgets is cross-compiled for Windows and installed into `/opt/wxwidgets-win64`.

Example build (wxWidgets 3.3.1):

```bash
cd /tmp
wget -O wxWidgets-3.3.1.tar.bz2 https://github.com/wxWidgets/wxWidgets/releases/download/v3.3.1/wxWidgets-3.3.1.tar.bz2
tar -xjf wxWidgets-3.3.1.tar.bz2
cd wxWidgets-3.3.1

mkdir -p build-mingw
cd build-mingw

../configure \
  --host=x86_64-w64-mingw32 \
  --build=x86_64-apple-darwin24.6.0 \
  --disable-shared \
  --with-msw \
  --prefix=/opt/wxwidgets-win64

make -j$(sysctl -n hw.ncpu)
sudo make install
```

Notes:

* `--disable-shared` is used to build wxWidgets statically for the Windows target.
* The exact `--build=...` triplet depends on your macOS version; use the value from `config.guess` if needed.

### libcurl (cross-compiled, static)

libcurl is cross-compiled for Windows and installed into `/opt/curl-win64`.

The build below uses CMake + Ninja and produces a static library (no `curl.exe`). It is configured to use Windows
Schannel (native TLS) and HTTP-only features.

```bash
# In the curl source directory
cmake -S . -B build-win64 -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
  -DBUILD_CURL_EXE=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DHTTP_ONLY=ON \
  -DCURL_USE_SCHANNEL=ON \
  -DCURL_USE_OPENSSL=OFF \
  -DCURL_USE_LIBPSL=OFF \
  -DCURL_USE_LIBSSH2=OFF \
  -DCURL_BROTLI=OFF \
  -DCURL_ZSTD=OFF \
  -DUSE_WIN32_IDN=ON

cmake --build build-win64 -j
sudo cmake --install build-win64 --prefix /opt/curl-win64
```

### Clang/LLVM for Windows (prebuilt)

The Windows distribution bundles a prebuilt Clang/LLVM toolchain (MSVC ABI build).

* Download an archive like `clang+llvm-21.1.6-x86_64-pc-windows-msvc.tar.xz`
* Extract it into:

  * `/opt/clang+llvm-msvc-x86_64`

(See the Makefile / project scripts for the exact expected directory layout.)

### Build / packaging

The Windows distribution is produced from the `build/` directory.

Create a runnable `ArduinoEditor.exe` and stage all required dependencies into `dist_win64/`:

```bash
cd build
make -f Makefile.win64 package_win64
```

Notes:

* The build expects `arduino-cli.exe` to be present under `third-party/` so it can be bundled into the Windows
  distribution.

### Installer and ZIP

* Build the Windows installer:

```bash
cd build
make -f Makefile.win64 installer_win64
```

This target requires:

* NSIS (`brew install nsis`)

* osslsigncode (`brew install osslsigncode`)

* Windows signing materials placed in `winsign/`:

  * `winsign/cert.pem`
  * `winsign/key.pem`

* Build a ZIP package (from the staged `dist_win64` output):

```bash
cd build
make -f Makefile.win64 zip_win64
```

---

## Troubleshooting

- Ensure all required system dependencies are installed.
- Verify that the correct toolchain is selected for each platform.
- Check build logs for missing libraries or incompatible targets.
