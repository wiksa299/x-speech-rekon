# X-Speech Listen changelog

## Unreleased

- Rename the plugin and Windows package to X-Speech Listen. Use the Listen signatures, datarefs and ABI v1 header for integrations.

- Offer the compact Fluid Speech Pro Q8_0 conversion from CrispASR and run its residual encoder block in transcribe.cpp. Existing FluidVoice Pro files remain loadable.
- Align the settings window with X-Speech Speak. Wrap controls in narrow windows, show the active models path between the field and the buttons while editing, and support system clipboard shortcuts in the folder field.
- Show known engine failures directly in the Engine panel.
- Prefer Q6_K where available and show a description and RAM estimate for the selected model and quantization.

## 0.1.0-beta.2

- Add Fluid Speech Mini and Pro with verified model downloads and compact weights.
- Use Moonshine Small Streaming in the Moonshine slot.
- Add a lexical bias toggle, model-specific default weight and optional TDT beams of two to four paths.
- Expose ranked transcript candidates in the C event payload. The first candidate remains in the primary transcript field.

## 0.1.0-beta.1

First Windows x64 beta for X-Plane 12.4.4 or later.

- Local push-to-talk speech recognition with Parakeet v2 and v3, Moonshine Streaming, and Nemotron 3.5 Streaming. The model catalog supports downloads and local model discovery.
- CPU inference by default, with optional Vulkan and automatic CPU fallback if a compatible GPU cannot load the model.
- Native C ABI v1 for per-client vocabulary, targeted transcripts and model metadata. The optional Diagnostic plugin demonstrates integration and provides a mappable PTT command.
- Microphone setup and monitoring, audio file tests, an in-flight capture indicator, request history and copyable diagnostics.
- English, French and Spanish interfaces for both plugins. Follow the simulator language or save a manual choice. Additional translations can be supplied as JSON files.

The beta package targets Windows x64. See the [installation instructions](README.md#install-and-test-on-windows).
