# X‑Speech Listen

Add local push-to-talk speech recognition to your X-Plane plugin through a native C API. X‑Speech Listen is a shared speech recognition service for X‑Plane 12.4.4 or later. It runs Moonshine Small Streaming, Parakeet v2 or v3, Fluid Speech Mini and Pro, and Nemotron 3.5 Streaming on the user's machine. The Windows x64 beta runs on the CPU by default and offers Vulkan on compatible GPUs.

[Install on Windows](#install-and-test-on-windows) · [Integrate with your plugin](#integrate-with-your-x-plane-plugin) · [Build from source](#build-and-test-without-the-simulator) · [Release notes](CHANGELOG.md)

## Install and test on Windows

Download the Windows x64 ZIP and its SHA‑256 file from **Artifacts** on a successful **Windows x86-64** run on the main branch. Choose a model to download after installation, or point Listen to the models you already have.

1. Close X‑Plane and extract `X-Speech-Listen` into `Resources/plugins`. To reuse existing models, select their folder in Settings. Extract `X-Speech-Listen-Diagnostic` alongside Listen to try the reference client.
2. Start X‑Plane and open **Plugins → X-Speech Listen → Settings**. In **Models folder**, click **Browse…** or enter the full path to an existing folder and click **Apply**. Select a model such as Parakeet v3. Listen preselects **Q6_K** when available. **Download** appears if that variant is missing from the models folder. Select **Use this model** when the variant is available.
3. Open **Test**. Try **Test the built-in recording** or **Choose a WAV file…** with a recording of up to 30 seconds. Use **Hold to talk** to test your microphone. Results appear under **Transcript**.
4. To test inter-plugin PTT with the diagnostic client, bind `xspeech_listen/diagnostic/ptt` in X‑Plane. Open its diagnostic window, hold the binding while speaking, and release it to see the transcript and model identity.

For English-only recognition, select **Parakeet v2**. Q4_K_M, Q6_K and Q8_0 are available. The built-in recording is in English even when the interface uses French or Spanish.

## Integrate with your X-Plane plugin

Include [`xspeech_listen.h`](include/xspeech_listen.h), which defines C ABI v1 and ships in the Windows ZIP. Install the provider alongside your plugin and discover it by its `wiksa.x-speech-listen` signature. Your plugin owns its push-to-talk command and chooses how to interpret the transcript. The provider opens the microphone for one accepted request at a time and sends the result only to the client that started that session.

### Minimal C client

Call `EnableVoicePTT()` from `XPluginEnable` and `DisableVoicePTT()` from `XPluginDisable`. If your plugin already exports `XPluginReceiveMessage`, merge the handler below into that callback. Replace `example/voice/ptt` with your own command name and bind it in X‑Plane. Send requests from XPLM callbacks on the simulator's main thread.

```c
#include <XPLMPlugin.h>
#include <XPLMUtilities.h>
#include <stdbool.h>
#include <stdio.h>
#include "xspeech_listen.h"

static XPLMPluginID voice_provider = XPLM_NO_PLUGIN_ID;
static XPLMCommandRef voice_command;
static uint64_t voice_session;
static bool voice_held;
static uint32_t voice_status = XSL_SERVICE_UNAVAILABLE;
static char voice_text[XSL_MAX_TEXT_BYTES + 1];

static XslRequest SendVoiceRequest(uint32_t operation, uint64_t session,
                                   const XslString *terms, uint32_t count) {
    XslRequest req = {0};
    req.version = XSL_ABI_VERSION;
    req.struct_size = sizeof req;
    req.operation = operation;
    req.status = XSL_SERVICE_UNAVAILABLE;
    req.session = session;
    req.terms = terms;
    req.term_count = count;
    if (voice_provider != XPLM_NO_PLUGIN_ID && XPLMIsPluginEnabled(voice_provider))
        XPLMSendMessageToPlugin(voice_provider, XSL_MESSAGE_REQUEST, &req);
    return req;
}

static int OnVoiceCommand(XPLMCommandRef command, XPLMCommandPhase phase, void *context) {
    (void)command;
    (void)context;
    if (phase == xplm_CommandBegin && !voice_session) {
        voice_text[0] = '\0';
        voice_provider = XPLMFindPluginBySignature(XSL_PROVIDER_SIGNATURE);
        XslRequest reply = SendVoiceRequest(XSL_SUBSCRIBE, 0, NULL, 0);
        if (reply.status == XSL_OK) {
            reply = SendVoiceRequest(XSL_PTT_BEGIN, 0, NULL, 0);
            if (reply.status == XSL_OK) {
                voice_session = reply.session;
                voice_held = true;
            }
        }
        voice_status = reply.status;
    } else if (phase == xplm_CommandEnd && voice_held) {
        voice_held = false;
        voice_status = SendVoiceRequest(XSL_PTT_END, voice_session, NULL, 0).status;
        if (voice_status != XSL_OK) voice_session = 0;
    }
    return 1;
}

void EnableVoicePTT(void) {
    voice_command = XPLMCreateCommand("example/voice/ptt", "Hold to talk");
    if (voice_command) XPLMRegisterCommandHandler(voice_command, OnVoiceCommand, 1, NULL);
}

void DisableVoicePTT(void) {
    if (voice_command) XPLMUnregisterCommandHandler(voice_command, OnVoiceCommand, 1, NULL);
    SendVoiceRequest(XSL_UNSUBSCRIBE, 0, NULL, 0);
    voice_command = NULL;
    voice_provider = XPLM_NO_PLUGIN_ID;
    voice_session = 0;
    voice_held = false;
    voice_status = XSL_SERVICE_UNAVAILABLE;
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID sender, int message, void *parameter) {
    if (sender != voice_provider || message != XSL_MESSAGE_EVENT || !parameter) return;
    const XslEvent *event = (const XslEvent *)parameter;
    if (event->version != XSL_ABI_VERSION || event->struct_size < sizeof *event) return;
    if (event->kind == XSL_SERVICE_STOPPED) {
        voice_provider = XPLM_NO_PLUGIN_ID;
        voice_session = 0;
        voice_held = false;
        voice_status = XSL_SERVICE_UNAVAILABLE;
    } else if (event->kind == XSL_RESULT && voice_session && event->session == voice_session) {
        voice_session = 0;
        voice_held = false;
        voice_status = event->status;
        if (event->status == XSL_OK && event->text)
            snprintf(voice_text, sizeof voice_text, "%s", event->text);
    }
}
```

The example stores the last status and copies the transcript into `voice_text`. Request replies are synchronous. `XSL_CAPTURE_STARTED` and `XSL_RESULT` arrive asynchronously as `XSL_MESSAGE_EVENT`. Match the sender and session before using an event, and copy any strings you need after the callback. Use `XSL_PTT_CANCEL` with the session ID to abandon a request.

### Availability and reconnecting

| Status or event | What the client should do |
| --- | --- |
| `XSL_SERVICE_UNAVAILABLE`, `XSL_NOT_SUBSCRIBED` or `XSL_SERVICE_STOPPED` event | Clear the session and discover the provider again. Resubscribe and resend vocabulary after a provider reload. The example rediscovers on the next PTT press. |
| `XSL_ENGINE_RESTARTING` | Show that recognition is temporarily unavailable and let the user retry after the engine recovers. A restart from **Settings** preserves subscriptions. |
| `XSL_ENGINE_DISABLED`, `XSL_ENGINE_SUSPENDED` | Recognition is paused from **Settings**. The subscription stays active. The user can enable or resume the engine. |
| `XSL_BUSY` | Another request already owns the microphone. Wait for it to finish. |
| `XSL_MODEL_UNAVAILABLE` or `XSL_MIC_UNAVAILABLE` | Ask the user to check the selected model or microphone. |
| `XSL_BAD_VERSION` | Check that the client and the installed provider use a compatible API version. |

`XSL_GET_STATUS` returns the currently selected model, its quantization, `service_status`, `model_state` and `supports_vocabulary`. The success of `XSL_SUBSCRIBE` confirms the subscription. Its `service_status` field reports whether the engine is ready. The [diagnostic client source](src/diagnostic.cpp) shows periodic discovery, timeouts and a UI that reports request status.

### Model metadata and subscriber vocabulary

A successful `XSL_RESULT` includes the raw UTF-8 transcript, `model_id`, `quantization`, `revision` and `language` if known. Use the result metadata to choose any text normalization specific to the model that handled the request. `capture_ms` is the audio duration. `compute_ms` measures time spent in decoding calls, excluding model loading and waiting for audio.

After subscribing, send optional words or phrases with `XSL_SET_VOCABULARY`. For example, insert the following after `XSL_SUBSCRIBE` succeeds in the handler above.

```c
static const XslString terms[] = {
    {"flight level", sizeof "flight level" - 1},
    {"hold short", sizeof "hold short" - 1}
};
XslRequest vocabulary = SendVoiceRequest(XSL_SET_VOCABULARY, 0, terms, 2);
```

Check `vocabulary.status` for errors such as `XSL_VOCABULARY_LIMIT`. The provider copies the terms during the call and snapshots the requesting subscriber's list at `XSL_PTT_BEGIN`. Each new list replaces the previous one. An empty list clears it. The user's lexical bias switch controls whether the provider sends registered terms to the engine. The `vocabulary` field reports whether the model applied them, had no terms to apply, or the active decoding mode did not support them.

A successful event can carry up to `XSL_MAX_CANDIDATES` ranked hypotheses. Check `event->struct_size >= sizeof(XslEvent)` before reading `candidate_count` and `candidates`. Each `XslString` supplies a UTF-8 pointer and byte length. Element zero matches `text`. Copy any data you need before returning from the XPLM callback. Your plugin is responsible for validating the text and any action it derives from it.

### Read-only X-Plane datarefs

| Dataref | Integer value |
| --- | --- |
| `xspeech_listen/service/ready` | `1` when the engine has confirmed its current configuration and is out of recovery. |
| `xspeech_listen/model/state` | `0` unavailable, `1` unloaded, `2` loading, `3` loaded. |
| `xspeech_listen/ptt/busy` | `1` during an accepted PTT or built-in test, through its terminal result. |

The datarefs return zero while the provider is disabled. Their names are also defined in `xspeech_listen.h`.

## Settings and troubleshooting

Open **Settings → Microphone → Configure microphone…**. Pick an input or follow the system default. The guided ten-second test uses the same capture path as push-to-talk and works without a speech model. It shows the peak and average voice level in dBFS and flags a weak signal or clipping. Adjust input gain in your operating system and run the test again. **Play recording** uses the default audio output. Listen discards the recording when you close the panel or start another test.

Under **Settings → Interface language**, choose **Follow X-Plane** (default) or select a language by name. Listen ships English, French and Spanish. The provider and Diagnostic menus and panels switch immediately, including during a PTT session. The choice is saved in X-Plane’s preferences. Recognition language and the built-in test recording keep their own selection rules.

Use **Activity** to see which plugins are subscribed and inspect the 12 most recent requests. For a microphone request, wait for **Listening · Audio received** before speaking. It appears only when the provider receives confirmation of the first audio block. Activity shows the delay from PTT press to that confirmation, then transcription when the PTT is released. A release before confirmation is reported separately. WAV tests start at transcription. The cards also show the raw transcript, model, quantization, vocabulary status and time to result after release or WAV start. The history is kept in memory for the lifetime of the provider.

Open **Diagnostics → Copy diagnostics** for versions, engine and model state, microphone, requested and effective compute devices, settings storage, and up to eight recent incidents. The report contains technical information only. X‑Speech Listen entries in `Log.txt` also record status codes and timings.

The **Engine** panel shows the helper's status, the inference engine name and any known reason for a failure. **Suspend** stops it for the current plugin session. **Enable engine** persists across simulator sessions. **Restart engine** restarts the helper while retaining the active profile, subscriptions and activity history. The CPU and Vulkan selector is in the same panel. For CPU inference with `transcribe.cpp`, the thread setting takes effect the next time the engine starts.

The **Models** panel shows the model's load state and identifies the model in memory. The description below the selector lists the selected model's languages and features and shows a RAM estimate for its quantization. These estimates use peak RAM measurements of the catalog files in a Linux CPU benchmark. **Use this model** appears when you select a different model, quantization or available model file. Load and unload policy changes apply automatically. The **Inactivity (minutes)** slider appears with **After inactivity** and applies the selected delay when you release the slider. If a newly selected model fails to load, the provider can restore the last model that loaded successfully, including after restarting X‑Plane.

Under **Recognition**, enable **Lexical bias** to use the requesting plugin's vocabulary. **Weight** controls how strongly compatible models favor it. **Auto** uses the model default, and a weight of zero disables bias. Moonshine Small defaults to bias off. Parakeet v2/v3 and Fluid support greedy decoding or a beam of two to four paths. A wider beam can return additional ranked hypotheses and take longer. Moonshine Small and Nemotron support **Early decoding**, which starts while you speak. Turn it off to use a caller's lexicon.

You can paste a path from the system clipboard into **Models folder**. While the draft differs from the active folder, Listen shows the active path between the field and the buttons. The new path becomes active after **Apply** and a successful scan.

Fluid Speech Mini downloads from FluidVoice. **Fluid Speech Pro Q8_0** downloads the [CrispASR conversion](https://huggingface.co/cstr/fluid-speech-pro-GGUF) with the residual encoder block and normal transcription. Existing FluidVoice Pro files remain usable. Download the new file from **Models**, then select **Use this model** to switch. Listen verifies the downloaded GGUF and prepares a compatible temporary copy during model loading. This requires temporary disk space roughly equal to the download size. The temporary copy is reused until the model file changes or the engine exits.

Enable **Settings → Microphone indicator → Show in flight** for a movable indicator inside X‑Plane. It shows **Idle**, **Waiting for audio** after a PTT request starts, or **Capture** once audio arrives. The window is compact when the requester name is hidden. Set **Background opacity** and **Border opacity** independently. At 0% for both, the icon and status text remain visible. Listen saves the indicator position and settings.

Enable **Microphone monitoring** under **Settings → Microphone** to hear your own microphone through the system's default audio output while holding PTT. Monitoring is off by default. **Monitoring mode** defaults to **Standard**. **Low latency** uses the default output's reported mix rate and requests shorter buffers while the recognition stream stays at 16 kHz. Listen falls back to **Standard**, then capture only if the output still cannot start.

To compare the modes, select your simulation headset as the system's default output and use the same PTT command and monitoring volume. Try the same phrase in both modes. **Diagnostics → Last microphone monitoring** reports the mode used, the stream and output rates, and the output buffer period. That period describes the driver configuration. Record the microphone and headset output together to measure round-trip latency.

For headsets with built-in sidetone, turn off Listen's software monitoring to hear only the headset's local microphone feedback. Using both can make your voice sound distant or doubled.

Set **Monitoring volume** under **Settings → Microphone**. It ranges from 0 to 100 percent, starts at 10 percent, and applies to the built-in PTT test and to clients using the base request. A client can override the volume for one PTT session by sending an `XslPttBeginRequest` with the same ABI version and the larger structure size. The provider's monitoring preference must still be enabled. Zero mutes monitoring. The gain affects playback only.

Select a Vulkan adapter under **Settings → Engine → Compute device** to try GPU inference. The setting applies automatically. The **Engine** panel shows the effective device when it differs from the selection, including CPU fallback. A compatible graphics driver provides the Vulkan runtime.

Nemotron 3.5 Streaming is available in Q4_K_M, Q6_K and Q8_0. It detects the language by default and can be set to English, French or Spanish. **Early decoding** starts transcription while PTT is held.

## Add an interface language

The provider and Diagnostic share the UTF-8 JSON files in `X-Speech-Listen/locales/`. To add Italian, for example, copy `en.json` to `it.json`, translate the values and add `"it": "Italiano"` to `catalog.json`. Keep the keys unchanged. Preserve each formatted string’s `%u` arguments in order, and keep ImGui identifiers out of translations. The bundled fonts cover Latin, Greek and Cyrillic characters. Other scripts may require additional font coverage.

Reload the plugins to discover new catalog entries. A language registered under its X-Plane language code can also be selected automatically. Missing or invalid translations fall back to English entry by entry. The binaries contain a copy of the English catalog for installations missing the language files. X-Plane obtains plugin descriptions at startup and the Diagnostic PTT command description when it registers the command.

## Build and test without the simulator

On Linux, you need CMake 3.24 or later, Ninja, a C++17 compiler, and the development packages for libcurl and OpenSSL. On Windows, use the Visual Studio 2022 generator targeting x64. CMake fetches pinned dependencies into the build directory. Use `FETCHCONTENT_SOURCE_DIR_<NAME>` to point to a local copy of a dependency.

```sh
cmake -S . -B build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

The pinned public transcribe.cpp source is built with the bundled `cmake/transcribe-listen.patch`. CMake uses Git to apply it. This keeps the build reproducible on GitHub Actions.

Use the helper to download or verify the catalog model variants and test file transcription and WAV-driven PTT.

```sh
build/dev/xsl-check --engine build/dev/xsl-engine --catalog assets/catalog.json \
  --models /path/to/models --fetch --wav assets/test-en.wav
```

Add `-DXSL_TEST_MODELS=/path/to/models` when configuring CMake to include that test in CTest. Use `--mic --profile parakeet-tdt-0.6b-v3/Q8_0` to test a real microphone. The other tests run without a microphone or X‑Plane. The Windows build uses native file dialogs and the system clipboard. On Linux, the WAV picker uses Zenity or KDialog and copying diagnostics uses `wl-copy` on Wayland or `xclip` on X11, when available.

On an AVX2-capable Linux development machine, the tested configuration uses `-DXSL_LINUX_AVX2=ON`. Build with `-DXSL_VULKAN=ON` and provide Vulkan headers, the loader library, `glslc` and the SPIRV-Headers CMake package. Pass `--compute vulkan` to `xsl-check` to test the first detected adapter. The Windows ZIP automatically selects a compatible ggml CPU backend. The generic backend can exceed the transcription time limits on slower processors.

## License

X‑Speech Listen's source code is available under the MIT license. Dependencies and model weights retain their own licenses. [MIT license](LICENSE)

## Author

Created and maintained by **wiksa**.

Codex prompted by wiksa
