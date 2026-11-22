#ifndef XSPEECH_REKON_H
#define XSPEECH_REKON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XSR_PROVIDER_SIGNATURE "wiksa.x-speech-rekon"
#define XSR_DATAREF_SERVICE_READY "xspeech_rekon/service/ready"
#define XSR_DATAREF_MODEL_STATE "xspeech_rekon/model/state"
#define XSR_DATAREF_PTT_BUSY "xspeech_rekon/ptt/busy"
#define XSR_ABI_VERSION 1u
#define XSR_MESSAGE_REQUEST 0x58565201
#define XSR_MESSAGE_EVENT 0x58565202
#define XSR_MAX_TERMS 256u
#define XSR_MAX_TERM_BYTES 96u
#define XSR_MAX_VOCABULARY_BYTES 16384u
#define XSR_MAX_CONTEXT_BYTES 2048u
#define XSR_MODEL_QWEN3_ASR_06B "qwen3-asr-0.6b"
#define XSR_MAX_TEXT_BYTES 8192u
#define XSR_DEFAULT_MONITOR_GAIN_PERCENT 10u
#define XSR_MAX_MONITOR_GAIN_PERCENT 100u

enum XsrOperation {
    XSR_SUBSCRIBE = 1, XSR_UNSUBSCRIBE, XSR_GET_STATUS, XSR_SET_VOCABULARY,
    XSR_PTT_BEGIN, XSR_PTT_END, XSR_PTT_CANCEL
};

enum XsrStatus {
    XSR_OK = 0, XSR_SERVICE_UNAVAILABLE, XSR_BAD_VERSION, XSR_BAD_REQUEST,
    XSR_NOT_SUBSCRIBED, XSR_BUSY, XSR_MODEL_UNAVAILABLE, XSR_MIC_UNAVAILABLE,
    XSR_ENGINE_LOST, XSR_ENGINE_RESTARTING, XSR_TIMEOUT, XSR_CANCELLED,
    XSR_NO_AUDIO, XSR_NO_SPEECH, XSR_STALE_SESSION, XSR_OUTPUT_TRUNCATED,
    XSR_ENGINE_ERROR, XSR_VOCABULARY_LIMIT
};

enum XsrVocabulary { XSR_VOCAB_NONE = 0, XSR_VOCAB_APPLIED, XSR_VOCAB_UNSUPPORTED };
enum XsrEventKind { XSR_CAPTURE_STARTED = 1, XSR_RESULT, XSR_SERVICE_STOPPED };
enum XsrStage { XSR_STAGE_NONE = 0, XSR_STAGE_CAPTURE_START, XSR_STAGE_CAPTURE, XSR_STAGE_TRANSCRIPTION };
enum XsrModelState { XSR_MODEL_MISSING = 0, XSR_MODEL_UNLOADED, XSR_MODEL_LOADING, XSR_MODEL_READY };

typedef struct XsrString {
    const char *data;
    uint32_t bytes;
} XsrString;

/* Send synchronously from an XPLM callback. Initialize to zero, then set
 * version, size, operation and status = XSR_SERVICE_UNAVAILABLE before sending.
 * For SET_VOCABULARY, pointers need only live until the call returns.
 * BEGIN fills session. END and CANCEL take that same session.
 * SUBSCRIBE and GET_STATUS return the selected model and service states. */
typedef struct XsrRequest {
    uint32_t version;
    uint32_t struct_size;
    uint32_t operation;
    uint32_t status;
    uint64_t session;
    const XsrString *terms;
    uint32_t term_count;
    uint32_t model_state;
    uint32_t supports_vocabulary;
    uint32_t service_status;
    char model_id[64];
    char quantization[16];
} XsrRequest;

/* Optional PTT_BEGIN payload. Pass &request with request.struct_size set to
 * sizeof(XsrPttBeginRequest). A v1 provider without this extension ignores
 * the gain field. The provider's monitoring preference must be enabled.
 * The value affects playback only, for this session. Zero mutes monitoring. */
typedef struct XsrPttBeginRequest {
    XsrRequest request;
    uint32_t monitor_gain_percent;
} XsrPttBeginRequest;

/* Optional PTT_BEGIN payload for Qwen3-ASR. Set begin.request.struct_size to
 * sizeof(XsrPttContextRequest). The request terms replace the subscriber's
 * vocabulary for this session, including when the list is empty. Context
 * is optional UTF-8 text. Pointers need only live until the call returns.
 * Other profiles and older providers use the registered vocabulary. */
typedef struct XsrPttContextRequest {
    XsrPttBeginRequest begin;
    XsrString context;
} XsrPttContextRequest;

/* Callback-scoped event. Copy the data needed after the callback returns.
 * Text is present only for a successful terminal result. */
typedef struct XsrEvent {
    uint32_t version;
    uint32_t struct_size;
    uint32_t kind;
    uint32_t status;
    uint64_t session;
    uint32_t vocabulary;
    uint32_t stage;
    uint32_t capture_ms;
    uint32_t compute_ms; /* Wall time inside decoding calls, excluding model load and audio wait. */
    const char *text;
    const char *model_id;
    const char *revision;
    const char *quantization;
    const char *language;
} XsrEvent;

#ifdef __cplusplus
}
#endif
#endif
