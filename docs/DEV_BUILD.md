# akróasys Developer Build Guide

This is the authoritative cross-platform build guide for akróasys developers. The
top-level `README.md` carries only a minimal build snippet; everything below
covers the full set of system dependencies, Python packages, and platform
quirks you actually need to produce a working build.

The reference for build dependencies is `.github/workflows/build.yml`. If
anything in this document diverges from that workflow, treat the workflow as
correct and file a fix against this document.

---

## 1. Overview

A akróasys build produces two artefacts that must be assembled together:

1. **The C++ JUCE plugin.** A single CMake target produces four formats:
   - `Standalone` on every platform
   - `VST3` on every platform
   - `AU` on macOS only
   - `CLAP` on macOS only — via clap-juce-extensions, fetched at configure
     time; turn it off with `-DT5YNTH_BUILD_CLAP=OFF`
   These are emitted under `build_clean/T5ynth_artefacts/Release/<format>/`.

2. **The Python inference backend.** During development this is an ordinary
   Python script (`backend/pipe_inference.py`) running inside a venv. For
   release the same script is frozen into a one-folder PyInstaller bundle
   (`backend/dist/pipe_inference/`) and copied next to the plugin so the
   standalone application can launch it as a subprocess. The backend is
   responsible for running diffusion inference (Stable Audio Open / AudioLDM2)
   because the BrownianTree SDE sampler from `torchsde` has no C++ equivalent.

The two artefacts communicate over a binary stdin/stdout protocol documented
in `docs/IPC_PROTOCOL.md`.

---

## 2. Supported Platforms

| Platform | Acceleration | Status |
| --- | --- | --- |
| macOS 14+ on Apple Silicon | MPS (Metal) | Primary development target. |
| Linux x86_64 (Ubuntu 22.04 / 24.04) | CUDA 12.8 (NVIDIA) | CI target. |
| Linux x86_64 (Fedora 42) | CUDA 12.8 (NVIDIA) | Not in CI. Verified end-to-end on one machine 2026-11-08 (plugin build with the Csound bundle gate, backend generate + LRO authoring over the pipe). Fedora packages no Csound: it is built from source, §4.2. |
| Windows 11 x86_64 | CUDA 12.8 (NVIDIA) | CI target. MSVC 2022. |

CPU-only fallback works on all three platforms but is too slow for interactive
use during development. A discrete NVIDIA GPU is strongly recommended on
Linux/Windows; on macOS the integrated Apple Silicon GPU via MPS is roughly
3x faster than CPU and is the default device on that platform.

---

## 3. Common Prerequisites

The following are required on every platform regardless of host OS.

- **CMake** >= 3.22
- **C++20 toolchain** (Apple Clang on macOS, GCC >= 11 on Linux, MSVC 19.3x
  on Windows)
- **Python 3.10** for the inference backend. git-main `stable-audio-tools`
  pins `python_requires '>=3.10,<3.11'`, so the PyInstaller backend builds
  **only** on 3.10 — both CI and the dev `.venv` use 3.10. On 3.11+, `pip
  install` of the native stack fails outright. (The JUCE/C++ build itself does
  not depend on the Python version; this constraint is purely the backend's.)
- **Git.** The `JUCE/` directory in this repository is **vendored**, not a
  submodule. Do **not** run `git submodule update --init` against it; it has
  no effect on JUCE and may obscure local edits in other future submodules.
  A plain `git clone` is sufficient to obtain everything the build needs.

---

## 4. Per-Platform System Dependencies

### 4.1 macOS (Apple Silicon)

```bash
# Xcode command-line tools (provides Apple Clang, headers, codesign)
xcode-select --install

# CMake via Homebrew
brew install cmake
```

Notes:

- No codesigning identity is required for development. The CMake build uses
  ad-hoc signing automatically. The post-build step in `CMakeLists.txt`
  re-signs the VST3 bundle ad-hoc after JUCE generates `moduleinfo.json`
  (see section 11 for the rationale).
- Release installers are different: `installer/macos/build_pkg.sh` now
  supports `Developer ID Application` signing for the app bundle,
  `Developer ID Installer` signing for the `.pkg`, and optional
  notarization via `xcrun notarytool`. See `docs/RELEASE_PROCESS.md` for
  the required credentials.
- `WebKit.framework` is part of the base macOS install, so
  `JUCE_WEB_BROWSER=1` works out of the box with no extra packages.
- Homebrew's CMake is fine. If you prefer the official CMake.app installer,
  make sure `cmake` is on `PATH`.

### 4.2 Linux (Ubuntu 24.04 reference)

The full apt package set used by CI:

```bash
sudo apt-get update
sudo apt-get install -y \
  cmake build-essential \
  libasound2-dev libcurl4-openssl-dev libfreetype-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
  libxcomposite-dev libxext-dev libxrender-dev libfontconfig1-dev \
  libwebkit2gtk-4.1-dev libgtk-3-dev
```

Why `libgtk-3-dev` is needed even though only `libwebkit2gtk-4.1-dev` looks
like the obvious WebView dependency:

JUCE 8's `juce_gui_extra` module header **does not** declare a `linuxPackages:`
line for its WebBrowserComponent. JUCE's CMake helpers therefore create a
pkg-config target called `JUCE_BROWSER_LINUX_DEPS` but do not link it
automatically. `T5ynth`'s `CMakeLists.txt` adds an explicit link via:

```cmake
$<$<BOOL:${UNIX}>:$<$<NOT:$<BOOL:${APPLE}>>:juce::pkgconfig_JUCE_BROWSER_LINUX_DEPS>>
```

That target resolves through `pkg-config` to `webkit2gtk-4.1` **and**
`gtk+-x11-3.0`. Without `libgtk-3-dev` installed, `pkg-config` cannot find
`gtk+-x11-3.0.pc` and the build fails inside `juce_gui_extra.cpp` with:

```
fatal error: gtk/gtk.h: No such file or directory
```

So both packages and the explicit CMake link are required; removing either
one breaks the Linux build.

22.04 differences:
- `libwebkit2gtk-4.1-dev` is also packaged on 22.04 since the 22.04.3 point
  release. Older 22.04 systems may only ship `libwebkit2gtk-4.0-dev`; in that
  case upgrade or pin the 4.1 PPA, since JUCE 8 expects the 4.1 ABI.
- All other packages match.

Fedora 42 reference (verified 2026-11-08 on one machine, GCC 15.2, glibc 2.41;
not in CI):

```bash
sudo dnf install -y \
  cmake gcc-c++ make git pkgconf-pkg-config patchelf \
  python3.10 python3.10-devel python3-pip \
  alsa-lib-devel libcurl-devel freetype-devel \
  libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel \
  libXcomposite-devel libXext-devel libXrender-devel fontconfig-devel \
  webkit2gtk4.1-devel gtk3-devel
```

`patchelf` is what `tools/bundle_csound_linux.sh` rewrites rpaths with; the
plugin build's POST_BUILD step fails without it. If your Fedora release does
not ship `webkit2gtk4.1-devel` under that exact name, search for the closest
4.1 ABI package (`dnf search webkit2gtk`).

**Csound on Fedora is built from source.** Fedora 42 packages no Csound at all
(`dnf list csound` finds nothing), and the plugin links `libcsound64` through
`find_library` (`CMakeLists.txt`, `T5YNTH_CSOUND_LINUX_LIB`). The build that is
verified here, with the extra development packages it needs:

```bash
sudo dnf install -y libsndfile-devel libsamplerate-devel portaudio-devel \
  pulseaudio-libs-devel eigen3-devel gmm-devel stk-devel

git clone --depth 1 --branch 6.18.1 https://github.com/csound/csound.git ~/ai/csound
cd ~/ai/csound
# GCC 15 rejects include/plugin.h:1005 — `this->opaddr` names a member that is
# spelled `opadr` everywhere else; a one-word fix that is not in the 6.18.1 tag:
sed -i '1005s/this->opaddr/this->opadr/' include/plugin.h
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DUSE_DOUBLE=ON -DUSE_LIB64=OFF -DUSE_CURL=OFF \
  -DBUILD_TESTS=OFF -DBUILD_JAVA_INTERFACE=OFF -DBUILD_LUA_INTERFACE=OFF
cmake --build build -j"$(nproc)"
sudo cmake --install build
echo /usr/local/lib | sudo tee /etc/ld.so.conf.d/csound-local.conf
sudo ldconfig
```

Why each option matters:

- `USE_DOUBLE=ON` — the plugin, the backend and the bundler all name
  `libcsound64`. A float build is `libcsound`, and nothing finds it.
- `USE_LIB64=OFF` — installs to `/usr/local/lib`, where the drop-in above
  and the bundler's plugin-directory search (`/usr/local/lib/csound/plugins64-*`)
  look. ON puts the library under `/usr/local/lib64` instead.
- `USE_CURL=OFF` — Csound uses curl for two things only, URL `#include`s and
  the `strfromurl` opcode, neither of which the LRO reaches. With curl ON the
  library's transitive closure drags libcurl, OpenSSL, krb5, libselinux, libz
  and pcre2 into the bundle (28 libraries instead of 10), and the bundle gate
  fails (§11.6): JUCE links the distribution's libcurl directly, so the binary
  resolves libz and pcre2 from `/lib64` while copies of both sit in `libs/`.
  Ubuntu's packaged Csound has the same curl-free closure, so OFF matches
  what CI ships.

The plugin opcodes that live outside the main repository — STK, Chua, linear
algebra — come from the `plugins` repository's Csound 6 branch. The STK module
reads its rawwave files from the path compiled in here; Fedora's `stk` package
installs them under `/usr/share/stk/rawwaves`:

```bash
git clone --depth 1 https://github.com/csound/plugins.git ~/ai/csound-plugins
cd ~/ai/csound-plugins      # the default branch is csound6
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DUSE_DOUBLE=ON -DUSE_LIB64=OFF \
  -DBUILD_STK_OPCODES=ON -DDEFAULT_STK_RAWWAVE_PATH=/usr/share/stk/rawwaves \
  -DBUILD_CHUA_OPCODES=ON -DBUILD_LINEAR_ALGEBRA_OPCODES=ON \
  -DBUILD_FAUST_OPCODES=OFF -DBUILD_FLUID_OPCODES=OFF -DBUILD_HDF5_OPCODES=OFF \
  -DBUILD_IMAGE_OPCODES=OFF -DBUILD_WEBSOCKET_OPCODE=OFF -DUSE_FLTK=OFF \
  -DBUILD_WIIMOTE_OPCODES=OFF -DBUILD_MP3OUT_OPCODE=OFF -DBUILD_PYTHON_OPCODES=OFF \
  -DBUILD_P5GLOVE_OPCODES=OFF -DBUILD_JACK_OPCODES=OFF
cmake --build build -j"$(nproc)"
sudo cmake --install build   # → /usr/local/lib/csound/plugins64-6.0/lib{stkops,chua,linear_algebra}.so
```

Check the result the way the backend will see it:

```bash
timeout 30 csound -z1 2>&1 </dev/null | awk '{print $1}' | sort -u | wc -l   # 1426 unique opcode names
timeout 30 csound -z1 2>&1 </dev/null | grep -cE '\bSTK[A-Z]'               # 27 STK opcodes
.venv/bin/python tools/lco_measure.py --selftest                            # 88/88
```

Faust is deliberately not built: `faustgen` segfaults the host process on an
`import("stdfaust.lib")` (project `CLAUDE.md`, Instrument Authoring rule 6).

### 4.3 Windows 11

- **Visual Studio 2022 Community** (or Build Tools) with the
  *"Desktop development with C++"* workload. This installs MSVC, the
  Windows 10/11 SDK, and ATL/MFC headers.
- **CMake** — either the Windows installer from cmake.org or the version
  shipped inside the VS workload. Make sure `cmake` is on `PATH` from a
  fresh Developer PowerShell.
- **WebView2 runtime** — pre-installed on Windows 11. JUCE uses it for
  `JUCE_WEB_BROWSER=1`. No action required on Win11; on Win10 install the
  Evergreen runtime from Microsoft.
- **Python 3.10 from python.org.** Do **not** use the Microsoft Store
  Python build: PyInstaller has long-standing issues with the sandboxed
  Store install (path resolution, module discovery). Tick *Add Python to
  PATH* in the installer.

---

## 5. Python Backend Setup

The same procedure applies on every platform. Activate the venv before
running `pip` and `pyinstaller` commands.

### 5.1 Create the venv

Linux / macOS:

```bash
python3 -m venv .venv --clear
source .venv/bin/activate
```

Windows (PowerShell):

```powershell
python -m venv .venv
.venv\Scripts\Activate.ps1
```

### 5.2 Install PyInstaller

```bash
pip install pyinstaller
```

### 5.3 Install PyTorch (platform-specific)

This step is **before** `backend/requirements.txt`, because the native
extension wheels (`torch`, `torchaudio`, `torchvision`) must be ABI-aligned and
the platform-correct torch wheel must win.

**Linux / Windows (CUDA 12.8):**

```bash
pip install torch==2.7.1 torchaudio==2.7.1 torchvision==0.22.1 --index-url https://download.pytorch.org/whl/cu128
```

**macOS (Apple Silicon, MPS):**

```bash
pip install torch==2.7.1 torchaudio==2.7.1 torchvision==0.22.1
```

The default PyPI wheel for `arm64` macOS already enables MPS. There is no
special index URL for Apple Silicon and you do not want one.

### 5.4 Install backend requirements

```bash
pip install -r backend/requirements.txt
```

The requirements file currently lists:

```
torch>=2.0.0
diffusers>=0.24.0
transformers>=4.30.0
safetensors
accelerate
stable-audio-tools
soundfile>=0.12.0
numpy
scipy
torchsde
```

`torchsde` is the load-bearing dependency that forces the inference path
through Python — its BrownianTree SDE sampler is what makes Stable Audio
sound right and has no C++ port.

**llama-cpp-python on a Linux CUDA machine must be built from source.** The
PyPI wheel is CPU-only, and the LRO author (a 12B GGUF, `n_gpu_layers=-1`) is
not usable on CPU. With the CUDA toolkit installed (CMake finds `nvcc` on
`PATH` or at `/usr/local/cuda`; verified with CUDA 13.0 and an RTX PRO 6000,
compute capability 12.0):

```bash
CMAKE_ARGS="-DGGML_CUDA=on -DCMAKE_CUDA_ARCHITECTURES=120" \
  pip install --force-reinstall --no-cache-dir --no-binary llama-cpp-python llama-cpp-python
python -c "import llama_cpp; print(llama_cpp.llama_supports_gpu_offload())"   # True
```

`CMAKE_CUDA_ARCHITECTURES` is the card's compute capability without the dot
(`nvidia-smi --query-gpu=compute_cap --format=csv,noheader`). The build takes
several minutes; the wheel it produces is ~270 MB.

### 5.5 Quick sanity test (no PyInstaller)

```bash
cd backend
python pipe_inference.py
```

The process should print initialisation banners to stderr and then block
waiting for a JSON request on stdin. Press `Ctrl+D` (Unix) or `Ctrl+Z` then
Enter (Windows) to close it. If it crashes during import, your venv is
broken — fix that before proceeding to PyInstaller.

---

## 6. Bundling the Backend with PyInstaller

The PyInstaller spec lives at `backend/pipe_inference.spec`. Run it from
inside the `backend/` directory so the relative paths in the spec resolve.
t5-base (the text encoder for native Stable Audio Open Small) is no longer
bundled — the user installs it via akróasys's Settings page after first launch.

```bash
cd backend
pyinstaller pipe_inference.spec --noconfirm
```

Output: `backend/dist/pipe_inference/`. This is one-folder mode (a directory
of binaries and data files, with the launcher at
`backend/dist/pipe_inference/pipe_inference`). One-file mode is **not** used
because torch loads native libraries by file name and breaks when frozen
into a single archive.

The spec strips a number of CUDA libraries (cufft, cusparse, cusolver, nccl,
nvrtc, nvJitLink, cupti, triton) that are present in the wheel but not used
by inference. This is intentional and saves roughly 800 MB.

### 6.1 Smoke-test the bundle

Linux / macOS:

```bash
timeout 10 ./dist/pipe_inference/pipe_inference < /dev/null 2>&1 || true
```

The expected outcome is initialisation messages on stderr followed by the
process exiting (because stdin closed). This is the same check the CI runs.
A bundle that fails to start here will fail when launched by JUCE.

Windows (PowerShell):

```powershell
$proc = Start-Process .\dist\pipe_inference\pipe_inference.exe -PassThru
Start-Sleep -Seconds 5
if (!$proc.HasExited) { Stop-Process -Id $proc.Id; "ok" } else { "failed: exit $($proc.ExitCode)" }
```

---

## 7. Configuring and Building the Plugin

The project-local convention remains `build_clean/`, matching
[`CLAUDE.md`](../CLAUDE.md). CI uses `build/` on ephemeral runners, but local
instructions below intentionally stay on `build_clean/`.

```bash
cmake -S . -B build_clean -DCMAKE_BUILD_TYPE=Release
```

Build with all available cores:

Linux:
```bash
cmake --build build_clean --config Release -j$(nproc)
```

macOS:
```bash
cmake --build build_clean --config Release -j$(sysctl -n hw.ncpu)
```

Windows (Developer PowerShell for VS 2022):
```powershell
cmake --build build_clean --config Release -j $env:NUMBER_OF_PROCESSORS
```

### 7.1 Debug builds

```bash
cmake -S . -B build_clean -DCMAKE_BUILD_TYPE=Debug
cmake --build build_clean --config Debug -j$(nproc)
```

Debug builds work, but expect the first generation after launch to be slow:
the Python subprocess only initialises the diffusion pipeline lazily on the
first inference request, and that work is unaffected by the C++ build type.

### 7.2 Build outputs

After a Release build the artefacts are at:

```
build_clean/T5ynth_artefacts/Release/Standalone/akroasys(.app|.exe|)
build_clean/T5ynth_artefacts/Release/VST3/akroasys.vst3
build_clean/T5ynth_artefacts/Release/AU/akroasys.component        # macOS only
build_clean/T5ynth_artefacts/Release/CLAP/akroasys.clap           # macOS only
```

---

## 8. Assembling the Standalone App Bundle (macOS)

The Standalone `.app` produced by JUCE is a fully formed bundle but does
**not** yet contain the Python backend. The app expects to find the bundled
backend at `akroasys.app/Contents/Resources/backend/pipe_inference`. Copy it
in after the build:

```bash
APP=build_clean/T5ynth_artefacts/Release/Standalone/akroasys.app
mkdir -p "$APP/Contents/Resources/backend"
cp -R backend/dist/pipe_inference/* "$APP/Contents/Resources/backend/"
```

This is the same step CI runs (see `Assemble app bundle` in
`.github/workflows/build.yml`), but with the local build directory name changed
from `build/` to `build_clean/`.

For Linux and Windows builds the equivalent is to place the
`backend/dist/pipe_inference/` directory next to the `T5ynth` /
`akroasys.exe` binary. CI does this under `dist/akroasys/backend/` —
mirror that layout when shipping.

---

## 9. Running the Standalone

macOS:
```bash
./build_clean/T5ynth_artefacts/Release/Standalone/akroasys.app/Contents/MacOS/akróasys
```

Linux:
```bash
./build_clean/T5ynth_artefacts/Release/Standalone/akroasys
```

Windows (PowerShell):
```powershell
.\build_clean\T5ynth_artefacts\Release\Standalone\akroasys.exe
```

On first launch with no model installed, akróasys opens in its clean Init
state; generation will fail until a model is installed. Click the *Settings*
button in the status bar to walk through the per-model install flow described in
`docs/handover_distribution_session.md` and the in-app *About* dialog.

---

## 10. Installing the Built Plugin into a DAW

Copy or symlink the built bundle into the standard plugin directory for
your platform.

| Platform | Format | Location |
| --- | --- | --- |
| macOS | AU | `~/Library/Audio/Plug-Ins/Components/akroasys.component` |
| macOS | VST3 | `~/Library/Audio/Plug-Ins/VST3/akroasys.vst3` |
| macOS | CLAP | `~/Library/Audio/Plug-Ins/CLAP/akroasys.clap` |
| Linux | VST3 | `~/.vst3/akroasys.vst3` |
| Windows | VST3 | `%CommonProgramFiles%\VST3\akroasys.vst3` |

A symlink from the build output avoids re-copying after every build:

```bash
# macOS AU example
ln -sf "$PWD/build_clean/T5ynth_artefacts/Release/AU/akroasys.component" \
  "$HOME/Library/Audio/Plug-Ins/Components/akroasys.component"
```

The plugin formats and the Standalone share a single CMake target, so
re-building updates the symlinked bundle in place.

---

## 11. Common Build Errors and Their Causes

### 11.1 `gtk/gtk.h: No such file or directory` (Linux)

Cause: missing `libgtk-3-dev` **or** missing the explicit
`juce::pkgconfig_JUCE_BROWSER_LINUX_DEPS` link in `CMakeLists.txt`. Both are
required. JUCE 8's `juce_gui_extra` module header has no `linuxPackages:`
declaration, so JUCE neither pulls the package via pkg-config nor links it
automatically. akróasys's CMake configuration links the helper target by
hand; you provide the system package.

Fix: install `libgtk-3-dev` (see section 4.2). If the link is missing in
your local fork, restore the generator expression:

```cmake
$<$<BOOL:${UNIX}>:$<$<NOT:$<BOOL:${APPLE}>>:juce::pkgconfig_JUCE_BROWSER_LINUX_DEPS>>
```

### 11.2 `JuceHeader.h: No such file or directory` in clangd / LSP

Cause: clangd cannot find the JUCE-generated header. The actual CMake build
works fine — this is purely an editor integration issue.

Fix: configure clangd to read `compile_commands.json` from `build_clean/`.
Add a `.clangd` at the repo root or symlink
`build_clean/compile_commands.json` to the repo root.

### 11.3 PyInstaller failure on macOS involving `multiprocessing` or `resource_tracker`

Cause: a known class of issues where PyInstaller's runtime hooks plus
`multiprocessing.resource_tracker` produce a fork bomb when started under
the wrong launch conditions.

Fix: see `backend/runtime_hook.py` and keep `multiprocessing`-using packages
out of runtime hooks on macOS unless you have tested the bundled binary
end-to-end through the JUCE app.

### 11.4 `error: no matching constructor for initialization of 'juce::WebBrowserComponent'`

Cause: `JUCE_WEB_BROWSER` is set to `0` in the compile definitions of the
target. akróasys requires it to be `1` — the in-app Manual overlay renders
the shipped HTML guide through a `juce::WebBrowserComponent`.

Fix: verify `CMakeLists.txt` still contains:

```cmake
target_compile_definitions(T5ynth PUBLIC JUCE_WEB_BROWSER=1 ...)
```

### 11.5 macOS VST3: `code has no resources but signature indicates they must be present`

Cause: JUCE 8.0.6 codesigns the VST3 bundle **before** `juce_vst3_helper`
generates `Contents/Resources/moduleinfo.json`. Adding that file after
the seal invalidates the signature, and macOS DAWs refuse to load the
plugin as an instrument (it appears in scan logs but not in the
instrument browser).

Fix: akróasys's `CMakeLists.txt` adds a `POST_BUILD` `add_custom_command`
on the `T5ynth_VST3` target that re-signs the bundle ad-hoc after JUCE's
own POST_BUILD steps complete. POST_BUILD commands run in target order,
so this lands after the manifest step. If you see this error, verify the
custom command is still present:

```cmake
if(APPLE)
    add_custom_command(TARGET T5ynth_VST3 POST_BUILD
        COMMAND codesign --force --sign - --deep
            "$<TARGET_BUNDLE_DIR:T5ynth_VST3>"
        VERBATIM)
endif()
```

### 11.6 `Csound bundling incomplete: … still resolves /lib64/libz.so.1` (Linux)

The POST_BUILD bundler (`tools/bundle_csound_linux.sh`) copies `libcsound64`
and its whole non-system dependency closure into `libs/` and then requires
that any library of which a copy sits in `libs/` is also what the binary
resolves. A Csound built with `USE_CURL=ON` pulls libcurl → OpenSSL → krb5 →
libselinux → libz / pcre2 into that closure, while JUCE links the
distribution's libcurl directly and freetype resolves `/lib64/libz.so.1`
before the rpath copy. The gate flags both and `make` deletes the binary.
Rebuild Csound with `-DUSE_CURL=OFF` (§4.2); the bundle then holds 10
libraries and the gate passes. The Ubuntu package already has that closure,
which is why CI never sees this.

### 11.7 `'struct csnd::InPlug<N>' has no member named 'opaddr'` (Csound 6.18.1 with GCC 15)

Not this project's code: `include/plugin.h:1005` in the Csound 6.18.1 tag
spells the member `opaddr` where the struct defines `opadr`. Older GCCs did not
instantiate the template there; GCC 15 does. One-word fix before configuring
Csound (§4.2).

---

## 12. Running the Python Backend Standalone for Debugging

You do not need to launch the JUCE app to test inference logic. Activate
the venv and run the script directly:

```bash
cd backend
source ../.venv/bin/activate    # or .venv\Scripts\activate on Windows
python pipe_inference.py
```

The process reads newline-terminated JSON requests on stdin and writes
binary responses on stdout, with all logging on stderr. The exact request
schema, response framing, handshake, and supported operations are
specified in `docs/IPC_PROTOCOL.md` — build any hand-crafted debug request
against that document. Running the backend this way is the recommended
way to isolate inference bugs from GUI bugs: if a generation works here
but fails inside the JUCE app, the bug is in
`src/inference/PipeInference.cpp` or in how the bundled binary is
launched, not in the model code.

---

## 13. References

- `.github/workflows/build.yml` — authoritative dependency list and CI
  procedure for all three platforms.
- `CMakeLists.txt` — target definitions, compile definitions, the
  Linux GTK link, the macOS VST3 re-sign step.
- `backend/requirements.txt` — Python runtime dependencies.
- `backend/pipe_inference.spec` — PyInstaller bundling configuration.
- `backend/runtime_hook.py` — PyInstaller runtime hook (multiprocessing
  workaround).
- `docs/IPC_PROTOCOL.md` — JUCE ↔ Python pipe protocol.
