# Repository Guidelines

## Project Structure

This is a C++20 Android ARM64 native project built with CMake, with Android installer and runner applications under `app/`. Application code lives in `src/`, and public project headers live in `include/`. Keep source/header pairs aligned by name.

`lib/arm64-v8a/` is a local staging directory for separately supplied runtime libraries and models. Its README is tracked; runtime binaries and model weights must not be committed. Generated output belongs in `cmake-build-*`, `build/`, or `out/`.

## Build and Validation

Use the environment setup and platform-specific commands in `README.md`. Set `ANDROID_NDK_HOME` or pass `CMAKE_ANDROID_NDK` explicitly; do not add personal absolute paths to tracked files.

- `cmake --build cmake-build-debug --target kaiagentd --parallel` builds the configured native executable.
- From `app/`, `./gradlew :installer:assembleDebug :runner:assembleDebug` builds both applications.
- `init.bat` deploys local runtime resources; `m.bat <path-to-libkaiagentd.so>` then deploys and starts the program on an attached device.
- After staging changes, run `python3 scripts/check_public_tree.py` and `git diff --cached --check`.

For native behavior changes, perform a clean Android NDK build and an on-device smoke test when the required environment is available. Report unavailable checks explicitly. If tests are added, place native tests under `tests/` and wire them into CTest where appropriate.

## Coding Style

Follow the existing C++ style: four-space indentation, braces on the same line for functions and control blocks, `kPascalCase` constants, `lowerCamelCase` functions, and trailing underscores for private members. Prefer `constexpr` for constants and explicit Android/platform includes.

## Commit and Pull Request Guidelines

Use short imperative commit subjects. Commit creation is controlled by agents; do not run `git commit` manually unless the coordinating agent or repository owner explicitly requests it. Pull requests should describe behavior changes, list build/device verification performed, and identify any changes to runtime dependency requirements.

## Public Source Requirements

Keep the public version independent of private deployment services. Do not restore card-key login, online license validation, private service endpoints, or embedded credentials. Keep Android/Shizuku/root permission checks intact.

Never commit signing keys, passwords, tokens, environment files, local IDE settings, device data, logs, screenshots, or generated artifacts. Release signing uses optional environment variables documented in the README. Preserve third-party copyright and license notices when modifying or adding dependencies.
