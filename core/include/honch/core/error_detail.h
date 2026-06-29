#ifndef HONCH_CORE_ERROR_DETAIL_H
#define HONCH_CORE_ERROR_DETAIL_H

#include "honch/core/status.h"
#include "honch/core/error_reason.h"
#include "honch/core/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Structured detail for the most recent failure. Caller-owned (copied out by
 * honch_core_get_last_error); contains no owned allocations. `message` and
 * `component` point to static string literals valid for the process lifetime,
 * so the struct may be copied and outlive the call freely. */
typedef struct honch_error_detail {
    honch_status_t           status;           /* coarse code (unchanged contract) */
    honch_error_reason_t     reason;           /* finer cause */
    honch_transport_result_t transport_result; /* transport classification; only
                                                * meaningful for a transport-phase
                                                * failure. A local failure (queue
                                                * full / encode / OOM) leaves it 0
                                                * (== HONCH_TRANSPORT_ACCEPTED), so
                                                * read it together with reason. */
    int                      http_status;      /* HTTP status code, 0 if not applicable */
    int                      os_error;         /* errno / esp_err_t etc., 0 if not applicable */
    const char              *message;          /* short static description, or NULL */
    const char              *component;        /* failure phase, e.g. "http"/"queue", or NULL */
} honch_error_detail_t;

#ifdef __cplusplus
}
#endif

#endif /* HONCH_CORE_ERROR_DETAIL_H */
