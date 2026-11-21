# X‑Voice‑Rekon

X‑Voice‑Rekon is a native plugin for X‑Plane 12.4.4 or later. It runs AI speech recognition locally with Moonshine Streaming or Parakeet v3 and exposes a push-to-talk (PTT) API to other plugins. The first preview targets 64-bit Windows and CPU inference.

## Install and test on Windows

1. Close X‑Plane. Extract the `X-Voice-Rekon` and `X-Voice-Rekon-Diagnostic` folders from the ZIP directly into `Resources/plugins`.
2. Start X‑Plane. Open **Plugins → X-Voice-Rekon → Settings**. Browse to your models folder or download Parakeet v3 Q8_0. Apply the model when it appears as available.
3. Select **Test model**. Then hold the microphone test button, speak and release it. The transcript appears in the settings window.
4. In X‑Plane's command settings, bind a key or button to `xvoice_rekon/diagnostic/ptt`. Open the diagnostic window, use the command and check the transcript and model identity. Verify that an existing Space binding still works while the settings window is open.
5. Disable and re-enable X‑Voice‑Rekon in the Plugin Admin. Check that the diagnostic plugin reconnects. Quit X‑Plane and verify that `xvr-engine.exe` exits.

When reporting a problem, include the Windows and X‑Plane versions, the model and quantization, the displayed status, and the X‑Voice‑Rekon lines from `Log.txt`. Technical logs record status codes and timings.

## Build and test without the simulator

On Linux, you need CMake 3.24 or later, Ninja, a C++17 compiler, and the development packages for libcurl and OpenSSL. On Windows, use the Visual Studio 2022 generator targeting x64. CMake fetches pinned dependencies into the build directory. Use `FETCHCONTENT_SOURCE_DIR_<NAME>` to point to a local copy of a dependency.

```sh
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

Use the helper to download or verify all six model variants and test file transcription and WAV-driven PTT.

```sh
build/dev/xvr-check --engine build/dev/xvr-engine --catalog assets/catalog.json \
  --models /path/to/models --fetch --wav assets/test-en.wav
```

Add `-DXVR_TEST_MODELS=/path/to/models` when configuring CMake to include that test in CTest. Use `--mic --profile parakeet-tdt-0.6b-v3/Q8_0` to test a real microphone. The other tests run without a microphone or X‑Plane. The Windows settings window uses the native folder picker.

On an AVX2-capable Linux development machine, the tested configuration uses `-DXVR_LINUX_AVX2=ON`. The Windows ZIP automatically selects a compatible ggml CPU backend. The generic backend can exceed the transcription time limits on slower processors.

## Plugin API and downloads

The `xvoice_rekon.h` header defines the C ABI v1. A client looks up `wiksa.xvoice-rekon`, subscribes, optionally supplies vocabulary, and sends PTT requests. Replies go to the requesting client. Event strings remain valid for the duration of the callback. Built-in test results appear in the settings window.

GitHub Actions builds the Windows preview with MSVC. Download the ZIP and its SHA‑256 file from the **Artifacts** section of the corresponding workflow run.

X‑Voice‑Rekon's source code is available under the MIT license. Dependencies and model weights retain their own licenses. [MIT license](LICENSE)

Codex prompted by wiksa
