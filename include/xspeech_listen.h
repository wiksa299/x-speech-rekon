#ifndef XSPEECH_LISTEN_H
#define XSPEECH_LISTEN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XSL_PROVIDER_SIGNATURE "wiksa.x-speech-listen"
#define XSL_DATAREF_SERVICE_READY "xspeech_listen/service/ready"
#define XSL_DATAREF_MODEL_STATE "xspeech_listen/model/state"
#define XSL_DATAREF_PTT_BUSY "xspeech_listen/ptt/busy"
#define XSL_ABI_VERSION 1u
#define XSL_MESSAGE_REQUEST 0x58534C01
#define XSL_MESSAGE_EVENT 0x58534C02
#define XSL_MAX_TERMS 256u
#define XSL_MAX_TERM_BYTES 96u
#define XSL_MAX_VOCABULARY_BYTES 16384u
#define XSL_MAX_TEXT_BYTES 8192u
#define XSL_MAX_CANDIDATES 4u
#define XSL_DEFAULT_MONITOR_GAIN_PERCENT 10u
#define XSL_MAX_MONITOR_GAIN_PERCENT 100u

enum XslOperation {
    XSL_SUBSCRIBE = 1, XSL_UNSUBSCRIBE, XSL_GET_STATUS, XSL_SET_VOCABULARY,
    XSL_PTT_BEGIN, XSL_PTT_END, XSL_PTT_CANCEL
};

enum XslStatus {
    XSL_OK = 0, XSL_SERVICE_UNAVAILABLE, XSL_BAD_VERSION, XSL_BAD_REQUEST,
    XSL_NOT_SUBSCRIBED, XSL_BUSY, XSL_MODEL_UNAVAILABLE, XSL_MIC_UNAVAILABLE,
    XSL_ENGINE_LOST, XSL_ENGINE_RESTARTING, XSL_TIMEOUT, XSL_CANCELLED,
    XSL_NO_AUDIO, XSL_NO_SPEECH, XSL_STALE_SESSION, XSL_OUTPUT_TRUNCATED,
    XSL_ENGINE_ERROR, XSL_VOCABULARY_LIMIT, XSL_ENGINE_DISABLED, XSL_ENGINE_SUSPENDED
};

enum XslVocabulary { XSL_VOCAB_NONE = 0, XSL_VOCAB_APPLIED, XSL_VOCAB_UNSUPPORTED };
enum XslEventKind { XSL_CAPTURE_STARTED = 1, XSL_RESULT, XSL_SERVICE_STOPPED };
enum XslStage { XSL_STAGE_NONE = 0, XSL_STAGE_CAPTURE_START, XSL_STAGE_CAPTURE, XSL_STAGE_TRANSCRIPTION };
enum XslModelState { XSL_MODEL_MISSING = 0, XSL_MODEL_UNLOADED, XSL_MODEL_LOADING, XSL_MODEL_READY };

typedef struct XslString {
    const char *data;
    uint32_t bytes;
} XslString;

/* Send synchronously from an XPLM callback. Initialize to zero, then set
 * version, size, operation and status = XSL_SERVICE_UNAVAILABLE before sending.
 * For SET_VOCABULARY, pointers need only live until the call returns.
 * BEGIN fills session. END and CANCEL take that same session.
 * SUBSCRIBE and GET_STATUS return the selected model and service states. */
typedef struct XslRequest {
    uint32_t version;
    uint32_t struct_size;
    uint32_t operation;
    uint32_t status;
    uint64_t session;
    const XslString *terms;
    uint32_t term_count;
    uint32_t model_state;
    uint32_t supports_vocabulary;
    uint32_t service_status;
    char model_id[64];
    char quantization[16];
} XslRequest;

/* Optional PTT_BEGIN payload. Pass &request with request.struct_size set to
 * sizeof(XslPttBeginRequest). The provider's monitoring preference must be enabled.
 * The value overrides the provider's monitoring volume for this session only.
 * It affects playback only. Zero mutes monitoring. The base XslRequest uses
 * the provider's configured volume. */
typedef struct XslPttBeginRequest {
    XslRequest request;
    uint32_t monitor_gain_percent;
} XslPttBeginRequest;

/* Callback-scoped event. Copy the data needed after the callback returns.
 * Text is present only for a successful terminal result. When available,
 * candidates are ordered by the decoder and candidates[0] matches text.
 * Copy both the strings and their byte lengths before returning. Check
 * struct_size >= sizeof(XslEvent) before accessing the candidate fields. */
typedef struct XslEvent {
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
    uint32_t candidate_count;
    const XslString *candidates;
} XslEvent;

/* Size of the event prefix through language. */
#define XSL_EVENT_BASE_SIZE offsetof(XslEvent, candidate_count)

#ifdef __cplusplus
}
#endif
#endif
