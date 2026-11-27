# X-Speech Rekon changelog

## 0.1.0-beta.1

First Windows x64 beta for X-Plane 12.4.4 or later.

- Local push-to-talk speech recognition with Parakeet v2 and v3, Moonshine Streaming, Nemotron 3.5 Streaming, and Whisper Small.en through whisper.cpp or transcribe.cpp. The model catalog supports downloads and local model discovery.
- CPU inference by default, with optional Vulkan and automatic CPU fallback if a compatible GPU cannot load the model.
- Native C ABI v1 for per-client vocabulary, targeted transcripts and model metadata. The optional Diagnostic plugin demonstrates integration and provides a mappable PTT command.
- Microphone setup and monitoring, audio file tests, an in-flight capture indicator, request history and copyable diagnostics.
- English, French and Spanish interfaces for both plugins. Follow the simulator language or save a manual choice. Additional translations can be supplied as JSON files.

The beta package targets Windows x64. See the [installation and upgrade instructions](README.md#install-and-test-on-windows).
