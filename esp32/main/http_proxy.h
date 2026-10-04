/*
 * Optional HTTP CONNECT proxy for the Passport's HTTPS calls.
 * Enabled when the build environment sets MUSE_HTTP_PROXY_HOST. The proxy
 * sees the hostname (CONNECT) and then a TLS handshake; it does not see
 * the HTTP body.
 */
#pragma once

#include "muse_http_proxy_env.h"

#if MUSE_HTTP_PROXY_ENABLE

#include "esp_err.h"
#include "esp_tls.h"

#ifdef __cplusplus
extern "C" {
#endif

/* On success *out is a connected TLS session. On failure *out is NULL and
 * any socket opened here is closed. */
esp_err_t muse_tls_connect_proxy(const char *host, int port, int timeout_ms,
                                 esp_tls_t **out);

/* HTTPS request through the proxy. Returns 0 when an HTTP response was
 * completed (including 401 and 500). Returns -1 on a network failure and
 * sets *status to 0. *resp is malloc'd and NUL-terminated on success;
 * the caller frees it. *resp is NULL on failure. */
int muse_https_exchange(const char *url, const char *method, const char *auth,
                        const char *body, int timeout_ms, int *status,
                        char **resp, size_t *resp_len);

#ifdef __cplusplus
}
#endif

#endif /* MUSE_HTTP_PROXY_ENABLE */
