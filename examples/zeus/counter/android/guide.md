# Android counter example

Same Zeus UI and gRPC-Web API as the wasm, iOS, and macOS hosts. Zeus paints
the window (Canvas / JNI). Android widgets are not used.

From the **repo root**. macOS + Homebrew.

## 1. Install the SDK (once)

```
./examples/zeus/counter/android/install.sh
```

This installs Temurin (JDK), Android command-line tools, Gradle, platform 34,
NDK, the emulator image, and an AVD named `yuga`. It is several gigabytes and
asks you to accept Google’s SDK licenses.

**`.sdk-env` is created by this script.** It is gitignored. Do not `source` it
until `install.sh` has printed `android: done`. If you see:

```
source: no such file or directory: ./examples/zeus/counter/android/.sdk-env
```

`install.sh` has not finished. Run it again; it is idempotent. It writes
`.sdk-env` as soon as it knows `ANDROID_HOME`, then continues with packages
and the AVD.

`install.sh` also puts `JAVA_HOME`, `ANDROID_HOME`, and `PATH` in `.sdk-env`.
`run.sh` sources that file. You only need `source` in the terminal where you
start the emulator.

Optional: copy the `export` lines from `.sdk-env` into `~/.zshrc`.

## 2. Start the emulator (optional — `run.sh` auto-boots it)

`run.sh` detects when no device is connected and boots the shared `yuga` AVD
itself (SwiftShader, same flags as `emu.sh`). Only run this manually if you
want the emulator in its own terminal or need custom flags.

Quit any emulator window that is already open (the host-GPU path logs
`Failed to make display surface context current: 12301` and stays blank).

```
./examples/zeus/counter/android/emu.sh
```

That boots AVD `yuga` with **SwiftShader** (`-gpu swiftshader_indirect`) so
macOS does not have to bind a host OpenGL/Metal context. Leave this terminal
open. Wait until the home screen appears.

Equivalent:

```
source ./examples/zeus/counter/android/.sdk-env
emulator -avd yuga -gpu swiftshader_indirect -feature -Vulkan -no-snapshot-load
```

Do not use `emulator -avd yuga` alone on this Mac — that is what produces
errors 12299 / 12301 and a black window.

If SwiftShader is slow, try host GPU instead:

```
./examples/zeus/counter/android/emu.sh -gpu host
```

## 3. Run the app

Second terminal, repo root:

```
./examples/zeus/counter/android/run.sh
```

That script:

1. Sources `.sdk-env` (or finds Homebrew’s SDK / `~/Library/Android/sdk`).
2. Starts `../backend/run.sh` on `:8080` if nothing is listening.
3. Runs `yugac --target=android --run app.loam` (Gradle `installDebug` + `adb`).

The emulator reaches the Mac backend at **`10.0.2.2:8080`**, not `127.0.0.1`
(`http.client()` fills this per `--target`). The Simulator and Cocoa apps
use `127.0.0.1` because they share the Mac loopback. A physical device
cannot use `10.0.2.2` — set `YUGA_RPC_ADDR` to the Mac's LAN IP:port.

Layout is the phone view in density-independent pixels, like iOS points.
`zeus.App(title, build)` uses `window_size()`; trailing width/height only
pin a macOS/wasm window. Rebuild the APK after host changes (`./run.sh`).

Output Gradle tree: `examples/zeus/counter/android/build/app`.
Launch activity: `com.loam.app/com.loam.zeus.ZeusActivity`.

Without the helper scripts:

```
./examples/zeus/counter/backend/run.sh
./bin/yugac --target=android --run examples/zeus/counter/android/app.loam
```

`--target=android` always writes the Gradle project. `--run` needs the SDK,
NDK, Gradle 8.2+, and a booted emulator or device.

## Physical device

`10.0.2.2` is the emulator alias for the host loopback. A USB/Wi-Fi device
cannot use it. Change `api.android_addr()` to the Mac’s LAN IP. The backend
currently binds `127.0.0.1` only, so a physical device still cannot connect
until that bind is opened.

## If something fails

| Symptom | Fix |
|---|---|
| `source: no such file or directory: .sdk-env` | Run `./install.sh` to completion. |
| `run.sh: no Android SDK` | Same: `./install.sh`. Or set `ANDROID_HOME`. |
| `gradle installDebug` / no devices | Emulator not booted. `emulator -avd yuga` and wait. |
| `Failed to make display surface context current` / `12299` / `12301` / blank emulator | Host GPU bind failed. Quit the emulator and start `./emu.sh` (SwiftShader). |
| Emulator home screen is black | Same GPU issue. `./emu.sh`, not `emulator -avd yuga` alone. |
| App activity is blank / frozen | RPC used to run on the UI thread. Rebuild (`./run.sh`). Keep `../backend/run.sh` on `:8080`. |
| `emulator: command not found` | Use `./emu.sh`, or `source .sdk-env` then `$ANDROID_HOME/emulator/emulator`. |
| App opens but RPC fails | Backend not on `:8080`. `run.sh` starts it; or `../backend/run.sh`. Confirm you used `10.0.2.2`, not `127.0.0.1`. |
| `yugac` missing | `make` at the repo root. `run.sh` does this if `bin/yugac` is absent. |

Android Studio is an alternative to Homebrew: install Platform 34 + NDK +
platform-tools, then `export ANDROID_HOME="$HOME/Library/Android/sdk"`.
