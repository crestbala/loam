# Android gallery example

Same Zeus UI as the wasm, iOS, and macOS hosts. Zeus paints the window
(Canvas / JNI); Android widgets are not used. The gallery is pure UI — no
backend — so there is nothing to start besides the emulator.

From the **repo root**. macOS + Homebrew.

## 1. Install the SDK (once)

```
./examples/zeus/gallery/android/install.sh
```

This installs Temurin (JDK), Android command-line tools, Gradle, platform 34,
NDK, the emulator image, and an AVD named `loam`. It is several gigabytes and
asks you to accept Google's SDK licenses.

**`.sdk-env` is created by this script.** It is gitignored. Do not `source` it
until `install.sh` has printed `android: done`. If you see:

```
source: no such file or directory: ./examples/zeus/gallery/android/.sdk-env
```

`install.sh` has not finished. Run it again; it is idempotent. It writes
`.sdk-env` next to itself with `JAVA_HOME`, `ANDROID_HOME`, and `PATH`, which
`run.sh` sources.

If you already installed the SDK for another zeus example, skip this and
symlink or copy its `.sdk-env`:

```
ln -s ../../counter/android/.sdk-env examples/zeus/gallery/android/.sdk-env
```

## 2. Boot the emulator (optional — `run.sh` auto-boots it)

`run.sh` detects when no device is connected and boots the shared `loam` AVD
itself (SwiftShader, same flags as below). Only run this manually if you want
the emulator in its own terminal or to adjust flags:
## 2. Start the emulator (optional — `run.sh` auto-boots it)

`run.sh` detects when no device is connected and boots the shared `loam` AVD
itself. Only run this manually if you want the emulator in its own terminal
or need custom flags:

```
./examples/zeus/counter/android/emu.sh
```

`emu.sh` launches the `loam` AVD with SwiftShader (the counter example shares
the same AVD). You can also boot it from Android Studio. Keep it running if
you started it by hand.

## 3. Run the gallery

```
./examples/zeus/gallery/android/run.sh
```

Builds the APK with Gradle, installs it on the running emulator/device, and
launches the app. Zeus scroll panes are touch-only here (no scrollbar).
