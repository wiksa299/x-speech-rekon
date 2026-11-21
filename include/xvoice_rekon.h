#ifndef XVOICE_REKON_H
#define XVOICE_REKON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XVR_PROVIDER_SIGNATURE "wiksa.xvoice-rekon"
#define XVR_ABI_VERSION 1u
#define XVR_MESSAGE_REQUEST 0x58565201
#define XVR_MESSAGE_EVENT 0x58565202
#define XVR_MAX_TERMS 256u
#define XVR_MAX_TERM_BYTES 96u
#define XVR_MAX_VOCABULARY_BYTES 16384u
#define XVR_MAX_TEXT_BYTES 8192u

enum XvrOperation {
    XVR_SUBSCRIBE = 1, XVR_UNSUBSCRIBE, XVR_GET_STATUS, XVR_SET_VOCABULARY,
    XVR_PTT_BEGIN, XVR_PTT_END, XVR_PTT_CANCEL
};

enum XvrStatus {
    XVR_OK = 0, XVR_SERVICE_UNAVAILABLE, XVR_BAD_VERSION, XVR_BAD_REQUEST,
    XVR_NOT_SUBSCRIBED, XVR_BUSY, XVR_MODEL_UNAVAILABLE, XVR_MIC_UNAVAILABLE,
    XVR_ENGINE_LOST, XVR_ENGINE_RESTARTING, XVR_TIMEOUT, XVR_CANCELLED,
    XVR_NO_AUDIO, XVR_NO_SPEECH, XVR_STALE_SESSION, XVR_OUTPUT_TRUNCATED,
    XVR_ENGINE_ERROR, XVR_VOCABULARY_LIMIT
};

enum XvrVocabulary { XVR_VOCAB_NONE = 0, XVR_VOCAB_APPLIED, XVR_VOCAB_UNSUPPORTED };
enum XvrEventKind { XVR_CAPTURE_STARTED = 1, XVR_RESULT, XVR_SERVICE_STOPPED };
enum XvrStage { XVR_STAGE_NONE = 0, XVR_STAGE_CAPTURE_START, XVR_STAGE_CAPTURE, XVR_STAGE_TRANSCRIPTION };
enum XvrModelState { XVR_MODEL_MISSING = 0, XVR_MODEL_UNLOADED, XVR_MODEL_LOADING, XVR_MODEL_READY };

typedef struct XvrString {
    const char *data;
    uint32_t bytes;
} XvrString;

/* Send synchronously from an XPLM callback. Initialize to zero, then set
 * version, size, operation and status = XVR_SERVICE_UNAVAILABLE before sending.
 * For SET_VOCABULARY, pointers need only live until the call returns.
 * BEGIN fills session. END and CANCEL take that same session.
 * SUBSCRIBE and GET_STATUS return the selected model and service states. */
typedef struct XvrRequest {
    uint32_t version;
    uint32_t struct_size;
    uint32_t operation;
    uint32_t status;
    uint64_t session;
    const XvrString *terms;
    uint32_t term_count;
    uint32_t model_state;
    uint32_t supports_vocabulary;
    uint32_t service_status;
    char model_id[64];
    char quantization[16];
} XvrRequest;

/* Callback-scoped event. Copy the data needed after the callback returns.
 * Text is present only for a successful terminal result. */
typedef struct XvrEvent {
    uint32_t version;
    uint32_t struct_size;
    uint32_t kind;
    uint32_t status;
    uint64_t session;
    uint32_t vocabulary;
    uint32_t stage;
    uint32_t capture_ms;
    uint32_t compute_ms;
    const char *text;
    const char *model_id;
    const char *revision;
    const char *quantization;
    const char *language;
} XvrEvent;

#ifdef __cplusplus
}
#endif
#endif
