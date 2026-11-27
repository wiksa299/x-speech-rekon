# X‑Speech Rekon

Add local push-to-talk speech recognition to your X-Plane plugin through a native C API. X‑Speech Rekon is a shared speech recognition service for X‑Plane 12.4.4 or later. It runs Moonshine Streaming, Parakeet v2 or v3, Nemotron 3.5 Streaming, and Whisper Small.en on the user's machine. The Windows x64 beta runs on the CPU by default and offers Vulkan on compatible GPUs.

[Install on Windows](#install-and-test-on-windows) · [Integrate with your plugin](#integrate-with-your-x-plane-plugin) · [Build from source](#build-and-test-without-the-simulator) · [Release notes](CHANGELOG.md)

## Install and test on Windows

Download `X-Speech-Rekon-Windows-x86-64.zip` and its SHA‑256 file from **Artifacts** on the corresponding [Windows build](https://github.com/wiksa299/x-speech-rekon/actions/workflows/windows.yml). Choose a model to download after installation, or point Rekon to the models you already have.

1. Close X‑Plane. When upgrading from an earlier preview, move its plugin folders out of `Resources/plugins` and keep your GGUF files. Extract `X-Speech-Rekon` into `Resources/plugins`. Extract `X-Speech-Rekon-Diagnostic` alongside it to try the reference client.
2. Start X‑Plane and open **Plugins → X-Speech Rekon → Settings**. Use **Models folder** to select where you kept your models, or select a model such as Parakeet v3 or Whisper Small.en, choose **Q8_0**, and click **Download**. Select **Use this model** when it appears as available.
3. Open **Test**. Try **Test the built-in recording** or **Choose a WAV file…** with a recording of up to 30 seconds. Use **Hold to talk** to test your microphone. Results appear under **Transcript**.
4. To test inter-plugin PTT with the diagnostic client, bind `xspeech_rekon/diagnostic/ptt` in X‑Plane. Open its diagnostic window, hold the binding while speaking, and release it to see the transcript and model identity. Reassign the binding after upgrading from an earlier preview.

For English-only recognition, select **Parakeet v2**. Q4_K_M, Q6_K and Q8_0 are available. The built-in recording is in English even when the interface uses French or Spanish.

## Integrate with your X-Plane plugin

Include [`xspeech_rekon.h`](include/xspeech_rekon.h), which defines C ABI v1 and ships in the Windows ZIP. Install the provider alongside your plugin. Your plugin owns its push-to-talk command and chooses how to interpret the transcript. The provider opens the microphone for one accepted request at a time and sends the result only to the client that started that session.

### Minimal C client

Call `EnableVoicePTT()` from `XPluginEnable` and `DisableVoicePTT()` from `XPluginDisable`. If your plugin already exports `XPluginReceiveMessage`, merge the handler below into that callback. Replace `example/voice/ptt` with your own command name and bind it in X‑Plane. Send requests from XPLM callbacks on the simulator's main thread.

```c
#include <XPLMPlugin.h>
#include <XPLMUtilities.h>
#include <stdbool.h>
#include <stdio.h>
#include "xspeech_rekon.h"

static XPLMPluginID voice_provider = XPLM_NO_PLUGIN_ID;
static XPLMCommandRef voice_command;
static uint64_t voice_session;
static bool voice_held;
static uint32_t voice_status = XSR_SERVICE_UNAVAILABLE;
static char voice_text[XSR_MAX_TEXT_BYTES + 1];

static XsrRequest SendVoiceRequest(uint32_t operation, uint64_t session,
                                   const XsrString *terms, uint32_t count) {
    XsrRequest req = {0};
    req.version = XSR_ABI_VERSION;
    req.struct_size = sizeof req;
    req.operation = operation;
    req.status = XSR_SERVICE_UNAVAILABLE;
    req.session = session;
    req.terms = terms;
    req.term_count = count;
    if (voice_provider != XPLM_NO_PLUGIN_ID && XPLMIsPluginEnabled(voice_provider))
        XPLMSendMessageToPlugin(voice_provider, XSR_MESSAGE_REQUEST, &req);
    return req;
}

static int OnVoiceCommand(XPLMCommandRef command, XPLMCommandPhase phase, void *context) {
    (void)command;
    (void)context;
    if (phase == xplm_CommandBegin && !voice_session) {
        voice_text[0] = '\0';
        voice_provider = XPLMFindPluginBySignature(XSR_PROVIDER_SIGNATURE);
        XsrRequest reply = SendVoiceRequest(XSR_SUBSCRIBE, 0, NULL, 0);
        if (reply.status == XSR_OK) {
            reply = SendVoiceRequest(XSR_PTT_BEGIN, 0, NULL, 0);
            if (reply.status == XSR_OK) {
                voice_session = reply.session;
                voice_held = true;
            }
        }
        voice_status = reply.status;
    } else if (phase == xplm_CommandEnd && voice_held) {
        voice_held = false;
        voice_status = SendVoiceRequest(XSR_PTT_END, voice_session, NULL, 0).status;
        if (voice_status != XSR_OK) voice_session = 0;
    }
    return 1;
}

void EnableVoicePTT(void) {
    voice_command = XPLMCreateCommand("example/voice/ptt", "Hold to talk");
    if (voice_command) XPLMRegisterCommandHandler(voice_command, OnVoiceCommand, 1, NULL);
}

void DisableVoicePTT(void) {
    if (voice_command) XPLMUnregisterCommandHandler(voice_command, OnVoiceCommand, 1, NULL);
    SendVoiceRequest(XSR_UNSUBSCRIBE, 0, NULL, 0);
    voice_command = NULL;
    voice_provider = XPLM_NO_PLUGIN_ID;
    voice_session = 0;
    voice_held = false;
    voice_status = XSR_SERVICE_UNAVAILABLE;
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID sender, int message, void *parameter) {
    if (sender != voice_provider || message != XSR_MESSAGE_EVENT || !parameter) return;
    const XsrEvent *event = (const XsrEvent *)parameter;
    if (event->version != XSR_ABI_VERSION || event->struct_size < sizeof *event) return;
    if (event->kind == XSR_SERVICE_STOPPED) {
        voice_provider = XPLM_NO_PLUGIN_ID;
        voice_session = 0;
        voice_held = false;
        voice_status = XSR_SERVICE_UNAVAILABLE;
    } else if (event->kind == XSR_RESULT && voice_session && event->session == voice_session) {
        voice_session = 0;
        voice_held = false;
        voice_status = event->status;
        if (event->status == XSR_OK && event->text)
            snprintf(voice_text, sizeof voice_text, "%s", event->text);
    }
}
```

The example stores the last status and copies the transcript into `voice_text`. Request replies are synchronous. `XSR_CAPTURE_STARTED` and `XSR_RESULT` arrive asynchronously as `XSR_MESSAGE_EVENT`. Match the sender and session before using an event, and copy any strings you need after the callback. Use `XSR_PTT_CANCEL` with the session ID to abandon a request.

### Availability and reconnecting

| Status or event | What the client should do |
| --- | --- |
| `XSR_SERVICE_UNAVAILABLE`, `XSR_NOT_SUBSCRIBED` or `XSR_SERVICE_STOPPED` event | Clear the session and discover the provider again. Resubscribe and resend vocabulary after a provider reload. The example rediscovers on the next PTT press. |
| `XSR_ENGINE_RESTARTING` | Show that recognition is temporarily unavailable and let the user retry after the engine recovers. A restart from **Settings** preserves subscriptions. |
| `XSR_BUSY` | Another request already owns the microphone. Wait for it to finish. |
| `XSR_MODEL_UNAVAILABLE` or `XSR_MIC_UNAVAILABLE` | Ask the user to check the selected model or microphone. |
| `XSR_BAD_VERSION` | Check that the client and the installed provider use a compatible API version. |

`XSR_GET_STATUS` returns the currently selected model, its quantization, `service_status`, `model_state` and `supports_vocabulary`. The success of `XSR_SUBSCRIBE` confirms the subscription. Its `service_status` field reports whether the engine is ready. The [diagnostic client source](src/diagnostic.cpp) shows periodic discovery, timeouts and a UI that reports request status.

### Model metadata and subscriber vocabulary

A successful `XSR_RESULT` includes the raw UTF-8 transcript, `model_id`, `quantization`, `revision` and `language` if known. Use the result metadata to choose any text normalization specific to the model that handled the request. `capture_ms` is the audio duration. `compute_ms` measures time spent in decoding calls, excluding model loading and waiting for audio.

After subscribing, send optional words or phrases with `XSR_SET_VOCABULARY`. For example, insert the following after `XSR_SUBSCRIBE` succeeds in the handler above.

```c
static const XsrString terms[] = {
    {"flight level", sizeof "flight level" - 1},
    {"hold short", sizeof "hold short" - 1}
};
XsrRequest vocabulary = SendVoiceRequest(XSR_SET_VOCABULARY, 0, terms, 2);
```

Check `vocabulary.status` for errors such as `XSR_VOCABULARY_LIMIT`. The provider copies the terms during the call and uses the requesting subscriber's list at `XSR_PTT_BEGIN`. Each new list replaces the previous one. An empty list clears it. Parakeet v2 (English only), multilingual Parakeet v3, Nemotron with early decoding disabled, and the optional official Moonshine Voice engine can use these phrases as recognition hints. The `vocabulary` field in the result indicates whether the hints were passed to the model or the active mode does not support them. Your plugin validates and interprets the result.

### Read-only X-Plane datarefs

| Dataref | Integer value |
| --- | --- |
| `xspeech_rekon/service/ready` | `1` when the engine has confirmed its current configuration and is out of recovery. |
| `xspeech_rekon/model/state` | `0` unavailable, `1` unloaded, `2` loading, `3` loaded. |
| `xspeech_rekon/ptt/busy` | `1` during an accepted PTT or built-in test, through its terminal result. |

The datarefs return zero while the provider is disabled. Their names are also defined in `xspeech_rekon.h`.

## Settings and troubleshooting

Open **Settings → Microphone → Configure microphone…**, or **Test → Check microphone…**. Pick an input or follow the system default. The guided ten-second test uses the same capture path as push-to-talk and works without a speech model. It shows the peak and average voice level in dBFS and flags a weak signal or clipping. Adjust input gain in your operating system and run the test again. **Play recording** uses the default audio output. Rekon discards the recording when you close the panel or start another test.

Under **Settings → Interface language**, choose **Follow X-Plane** (default) or select a language by name. Rekon ships English, French and Spanish. The provider and Diagnostic menus and panels switch immediately, including during a PTT session. The choice is saved in X-Plane’s preferences. Recognition language and the built-in test recording keep their own selection rules.

Use **Activity** to see which plugins are subscribed and inspect the 12 most recent requests. For a microphone request, wait for **Listening · Audio received** before speaking. It appears only when the provider receives confirmation of the first audio block. Activity shows the delay from PTT press to that confirmation, then transcription when the PTT is released. A release before confirmation is reported separately. WAV tests start at transcription. The cards also show the raw transcript, model, quantization, vocabulary status and time to result after release or WAV start. The history is kept in memory for the lifetime of the provider.

Open **Diagnostics → Copy diagnostics** for versions, engine and model state, microphone, requested and effective compute devices, settings storage, and up to eight recent incidents. The report contains technical information only. X‑Speech Rekon entries in `Log.txt` also record status codes and timings.

**Compute and memory** changes apply automatically. Use **Restart engine** between requests to restart the speech process. The active profile, subscriptions and activity history are kept during a manual engine restart. If a newly selected model fails to load, the provider can restore the last model that loaded successfully, including after restarting X‑Plane.

Enable **Settings → Microphone indicator → Show in flight** for a movable indicator inside X‑Plane. It shows **Idle**, **Waiting for audio** after a PTT request starts, or **Capture** once audio arrives. The window is compact when the requester name is hidden. Set **Background opacity** and **Border opacity** independently. At 0% for both, the icon and status text remain visible. Rekon saves the indicator position and settings.

Enable **Microphone monitoring** under **Settings → Microphone** to hear your own microphone through the system's default audio output while holding PTT. Monitoring is off by default. **Monitoring mode** defaults to **Standard**. **Low latency** uses the default output's reported mix rate and requests shorter buffers while the recognition stream stays at 16 kHz. Rekon falls back to **Standard**, then capture only if the output still cannot start.

To compare the modes, select your simulation headset as the system's default output and use the same PTT command and monitoring volume. Try the same phrase in both modes. **Diagnostics → Last microphone monitoring** reports the mode used, the stream and output rates, and the output buffer period. That period describes the driver configuration. Record the microphone and headset output together to measure round-trip latency.

For headsets with built-in sidetone, turn off Rekon's software monitoring to hear only the headset's local microphone feedback. Using both can make your voice sound distant or doubled.

Set **Monitoring volume** under **Settings → Microphone**. It ranges from 0 to 100 percent, starts at 10 percent, and applies to the built-in PTT test and to clients using the base request. A client can override the volume for one PTT session by sending an `XsrPttBeginRequest` with the same ABI version and the larger structure size. The provider's monitoring preference must still be enabled. Zero mutes monitoring. The gain affects playback only.

Select a Vulkan adapter under **Compute device** to try GPU inference. The status line shows the device that loaded the model, including CPU fallback if the adapter disappeared or the model failed to load on it. A compatible graphics driver provides the Vulkan runtime.

Nemotron 3.5 Streaming is available in Q4_K_M, Q6_K and Q8_0. It detects the language by default and can be set to English, French or Spanish. **Early decoding** starts transcription while PTT is held.

**Whisper Small.en** decodes English after PTT release. Select **Whisper Small.en · whisper.cpp** or **Whisper Small.en · transcribe.cpp** in **Settings** and download the corresponding Q8_0 file. Both use the selected CPU or Vulkan adapter. The whisper.cpp profile turns the requesting plugin's vocabulary into an initial prompt and a soft grammar. The transcribe.cpp profile uses the vocabulary as context. The two profile IDs are `whisper-small.en-whisper-cpp` and `whisper-small.en-transcribe`.

If Whisper repeats the same word or phrase at the end of a result, Rekon makes one extra decoding pass. It accepts the retry only when it confirms the same words without the duplicate. If the entire transcript is an exact loop, Rekon can retain one copy from the first pass. Repeated digits stay intact. The measured decoding time includes the retry.

## Add an interface language

The provider and Diagnostic share the UTF-8 JSON files in `X-Speech-Rekon/locales/`. To add Italian, for example, copy `en.json` to `it.json`, translate the values and add `"it": "Italiano"` to `catalog.json`. Keep the keys unchanged. Preserve each formatted string’s `%u` arguments in order, and keep ImGui identifiers out of translations. The bundled fonts cover Latin, Greek and Cyrillic characters. Other scripts may require additional font coverage.

Reload the plugins to discover new catalog entries. A language registered under its X-Plane language code can also be selected automatically. Missing or invalid translations fall back to English entry by entry. The binaries contain a copy of the English catalog for installations missing the language files. X-Plane obtains plugin descriptions at startup and the Diagnostic PTT command description when it registers the command.

## Build and test without the simulator

On Linux, you need CMake 3.24 or later, Ninja, a C++17 compiler, and the development packages for libcurl and OpenSSL. On Windows, use the Visual Studio 2022 generator targeting x64. CMake fetches pinned dependencies into the build directory. Use `FETCHCONTENT_SOURCE_DIR_<NAME>` to point to a local copy of a dependency.

```sh
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

Use the helper to download or verify the catalog model variants and test file transcription and WAV-driven PTT.

```sh
build/dev/xsr-check --engine build/dev/xsr-engine --catalog assets/catalog.json \
  --models /path/to/models --fetch --wav assets/test-en.wav
```

Add `-DXSR_TEST_MODELS=/path/to/models` when configuring CMake to include that test in CTest. Use `--mic --profile parakeet-tdt-0.6b-v3/Q8_0` to test a real microphone. The other tests run without a microphone or X‑Plane. The Windows build uses native file dialogs and the system clipboard. On Linux, the WAV picker uses Zenity or KDialog and copying diagnostics uses `wl-copy` on Wayland or `xclip` on X11, when available.

On an AVX2-capable Linux development machine, the tested configuration uses `-DXSR_LINUX_AVX2=ON`. Build with `-DXSR_VULKAN=ON` and provide Vulkan headers, the loader library, `glslc` and the SPIRV-Headers CMake package. Pass `--compute vulkan` to `xsr-check` to test the first detected adapter. The Windows ZIP automatically selects a compatible ggml CPU backend. The generic backend can exceed the transcription time limits on slower processors.

The build places the whisper.cpp worker and its libraries in the `whisper` subdirectory beside `xsr-engine`. Keep that directory intact when relocating the engine. Each engine uses its own ggml build.

### Experimental official Moonshine Voice engine

Extract the [Moonshine Voice v0.1.6 native SDK](https://github.com/moonshine-ai/moonshine/releases/tag/v0.1.6)
for your platform and set `-DXSR_MOONSHINE_SDK=/path/to/extracted/sdk` when configuring CMake.
The directory must contain `include` and `lib`. Start with a Release build. On Windows, use MSVC x64
and the Microsoft Visual C++ x64 runtime. The Apple Silicon configuration targets the standalone
engine (`-DXSR_BUILD_PLUGINS=OFF`). It still needs native validation.
The standard Windows preview ZIP builds without this optional SDK. The workflow’s **Test Moonshine Voice**
input runs a separate MSVC build and tests both models from its staging directory with a WAV file and simulated PTT.
Pushes to `dev/moonshine-voice` run the same job. Download the CPU-only experimental ZIP
`X-Speech-Rekon-Moonshine-Windows-x86-64` from **Artifacts** after the job passes. Use the normal
[installation steps](#install-and-test-on-windows). The experimental build requires the
[Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

With the SDK enabled, **Moonshine Voice Small** and **Moonshine Voice Medium** appear in the model list.
The English models use their official quantized ONNX Runtime assets (142.3 MB and 269.1 MB),
run on the CPU and decode after PTT release. **Download** fetches and verifies the individual files.
You can also copy an intact model directory into **Models folder**. Rekon checks the contents before use.

Use the official-model catalog with the same standalone test tool.

```sh
build/dev/xsr-check --engine build/dev/xsr-engine --catalog assets/moonshine-catalog.json \
  --models /path/to/models --fetch --wav assets/test-en.wav
```

Set `MOONSHINE_ORT_SINGLE_THREAD=1` in the Linux test environment to reproduce the single-thread
CPU measurements. In X-Plane, use the subscriber’s PTT to test its vocabulary. The Test tab uses the
local test context.

The result identifies the selected model as `moonshine-voice-small` or `moonshine-voice-medium`,
with `ORT` in the format/quantization field. Official Moonshine Voice uses the requester's vocabulary
as contextual bias. A term containing a comma makes the list unsupported for that request because the SDK
accepts a comma-separated list. A caller without vocabulary starts with bias disabled.

X‑Speech Rekon's source code is available under the MIT license. Dependencies and model weights retain their own licenses. [MIT license](LICENSE)

Codex prompted by wiksa
