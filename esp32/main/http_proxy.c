/*
 * HTTP CONNECT to MUSE_HTTP_PROXY_HOST, then TLS to the real host.
 *
 * esp_tls_conn_new_sync only dials a socket in ESP_TLS_INIT, and that is also
 * the only place it sets is_tls. Skipping INIT (sockfd already connected,
 * state ESP_TLS_CONNECTING) leaves is_tls false, so esp_tls_conn_destroy
 * closes the fd once. mbedtls cleanup does not close it. Socket timeouts
 * are set here because the usual tcp_connect path is skipped.
 */
#include "http_proxy.h"

#if MUSE_HTTP_PROXY_ENABLE

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "link.proxy";

#define MAX_BODY (32 * 1024)
#define MAX_HEADER_BYTES 4096
#define MAX_REDIRECTS 4

static bool host_ok(const char *host)
{
    if (!host) return false;
    size_t n = strlen(host);
    if (n < 1 || n > 253) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)host[i];
        if (isalnum(c) || c == '.' || c == '-') continue;
        return false;
    }
    return true;
}

static int set_timeouts(int fd, int timeout_ms)
{
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
    return 0;
}

static int proxy_tcp_connect(int timeout_ms)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, MUSE_HTTP_PROXY_HOST, &addr) != 1) {
        ESP_LOGE(TAG, "proxy host is not an IPv4 address");
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "proxy socket failed: %d", errno);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(MUSE_HTTP_PROXY_PORT),
        .sin_addr = addr,
    };
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "proxy connect failed: %d", errno);
        close(fd);
        return -1;
    }
    if (rc < 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        rc = select(fd + 1, NULL, &wset, NULL, &tv);
        int err = 0;
        socklen_t len = sizeof(err);
        if (rc <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
            ESP_LOGW(TAG, "proxy connect timeout or error");
            close(fd);
            return -1;
        }
    }
    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0 || set_timeouts(fd, timeout_ms) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Read a proxy response until the header terminator. Does not read past it:
 * the next bytes are the server's TLS handshake. The buffer is on the heap
 * because this runs on the main task, whose stack just fits a TLS handshake. */
static int read_connect_status(int fd)
{
    char *buf = malloc(1024);
    if (!buf) return -1;
    size_t n = 0;
    int status = -1;
    while (n + 1 < 1024) {
        ssize_t r = recv(fd, buf + n, 1, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n++;
        if (n >= 4 && memcmp(buf + n - 4, "\r\n\r\n", 4) == 0) {
            if (n < 12 || strncmp(buf, "HTTP/1.", 7) != 0) break;
            char *sp = memchr(buf, ' ', n);
            if (!sp) break;
            status = atoi(sp + 1);
            break;
        }
    }
    free(buf);
    return status;
}

esp_err_t muse_tls_connect_proxy(const char *host, int port, int timeout_ms, esp_tls_t **out)
{
    if (out) *out = NULL;
    if (!out || !host_ok(host) || port < 1 || port > 65535) return ESP_ERR_INVALID_ARG;
    if (timeout_ms <= 0) timeout_ms = 15000;

    ESP_LOGI(TAG, "HTTPS via proxy %s:%d -> %s:%d",
             MUSE_HTTP_PROXY_HOST, MUSE_HTTP_PROXY_PORT, host, port);

    int fd = proxy_tcp_connect(timeout_ms);
    if (fd < 0) return ESP_FAIL;

    char *req = malloc(512);
    if (!req) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(req, 512,
                     "CONNECT %s:%d HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "Proxy-Connection: keep-alive\r\n"
                     "\r\n",
                     host, port, host, port);
    if (n < 0 || n >= 512 || send_all(fd, req, (size_t)n) != 0) {
        ESP_LOGW(TAG, "proxy CONNECT send failed");
        free(req);
        close(fd);
        return ESP_FAIL;
    }
    free(req);
    int proxy_status = read_connect_status(fd);
    ESP_LOGI(TAG, "proxy CONNECT status %d", proxy_status);
    if (proxy_status != 200) {
        close(fd);
        return ESP_FAIL;
    }

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    /* Own the fd from here. Destroy closes it because is_tls stays false. */
    if (esp_tls_set_conn_sockfd(tls, fd) != ESP_OK
        || esp_tls_set_conn_state(tls, ESP_TLS_CONNECTING) != ESP_OK) {
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
    };
    int r = esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg, tls);
    if (r != 1) {
        ESP_LOGW(TAG, "TLS handshake via proxy failed");
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }
    *out = tls;
    return ESP_OK;
}

typedef struct {
    char host[254];
    int port;
    char *path;
} parsed_url_t;

static void free_url(parsed_url_t *u)
{
    if (!u) return;
    free(u->path);
    u->path = NULL;
}

static int parse_https(const char *url, parsed_url_t *out)
{
    memset(out, 0, sizeof(*out));
    if (strncmp(url, "https://", 8) != 0) return -1;
    const char *p = url + 8;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = NULL;

    size_t host_len = colon ? (size_t)(colon - p) : (slash ? (size_t)(slash - p) : strlen(p));
    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';
    if (!host_ok(out->host)) return -1;

    out->port = 443;
    if (colon) {
        char *end = NULL;
        long port = strtol(colon + 1, &end, 10);
        if (port < 1 || port > 65535 || !end) return -1;
        if (slash) {
            if (end != slash) return -1;
        } else if (*end) {
            return -1;
        }
        out->port = (int)port;
    }

    const char *path = slash ? slash : "/";
    out->path = strdup(path[0] ? path : "/");
    if (!out->path || out->path[0] != '/' || strpbrk(out->path, " \r\n")) {
        free_url(out);
        return -1;
    }
    return 0;
}

/* 0 replaced *next. -1 do not follow (caller keeps the 3xx). */
static int apply_location(const parsed_url_t *cur, const char *location, parsed_url_t *next)
{
    memset(next, 0, sizeof(*next));
    if (!location || !location[0]) return -1;
    if (strncmp(location, "https://", 8) == 0) return parse_https(location, next);
    if (strncmp(location, "http://", 7) == 0) return -1;
    if (location[0] != '/') return -1;
    memcpy(next->host, cur->host, sizeof(next->host));
    next->port = cur->port;
    next->path = strdup(location);
    if (!next->path) return -1;
    return 0;
}

typedef struct {
    esp_tls_t *tls;
    uint8_t buf[512];
    size_t len;
    size_t pos;
    int64_t deadline;
} rdr_t;

/* 1 data available, 0 clean EOF, -1 error or deadline. */
static int rdr_fill(rdr_t *r)
{
    if (r->pos < r->len) return 1;
    while (esp_timer_get_time() < r->deadline) {
        ssize_t n = esp_tls_conn_read(r->tls, r->buf, sizeof(r->buf));
        if (n > 0) {
            r->len = (size_t)n;
            r->pos = 0;
            return 1;
        }
        if (n == 0) return 0;
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() >= r->deadline) return -1;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        return -1;
    }
    return -1;
}

static int rdr_byte(rdr_t *r)
{
    int filled = rdr_fill(r);
    if (filled != 1) return filled == 0 ? -2 : -1;
    return r->buf[r->pos++];
}

static int rdr_line(rdr_t *r, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int c = rdr_byte(r);
        if (c < 0) return -1;
        if (c == '\n') {
            if (n && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return 0;
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
}

static int rdr_exact(rdr_t *r, uint8_t *dst, size_t n)
{
    while (n) {
        if (rdr_fill(r) != 1) return -1;
        size_t have = r->len - r->pos;
        size_t take = have < n ? have : n;
        if (dst) memcpy(dst, r->buf + r->pos, take);
        r->pos += take;
        if (dst) dst += take;
        n -= take;
    }
    return 0;
}

static int body_add(char **buf, size_t *len, size_t *cap, const void *data, size_t n, bool *truncated)
{
    if (*truncated || n == 0) return 0;
    if (*len >= MAX_BODY) {
        *truncated = true;
        return 0;
    }
    if (*len + n > MAX_BODY) {
        n = MAX_BODY - *len;
        *truncated = true;
    }
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 1024;
        while (nc < *len + n + 1) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int tls_write_all(esp_tls_t *tls, const void *data, size_t len, int64_t deadline)
{
    const char *p = data;
    size_t off = 0;
    while (off < len) {
        if (esp_timer_get_time() >= deadline) return -1;
        ssize_t n = esp_tls_conn_write(tls, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        return -1;
    }
    return 0;
}

static char *trim_dup(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0) return true;
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0) return true;
    }
    return false;
}

static int header_is(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return 0;
    *value = line + n + 1;
    return 1;
}

static int write_request(esp_tls_t *tls, const char *method, const parsed_url_t *u,
                         const char *auth, const char *body, int64_t deadline)
{
    char host_extra[8] = "";
    if (u->port != 443) snprintf(host_extra, sizeof(host_extra), ":%d", u->port);

    char clen[64] = "";
    size_t body_len = 0;
    if (body) {
        body_len = strlen(body);
        snprintf(clen, sizeof(clen),
                 "Content-Type: application/json\r\nContent-Length: %u\r\n",
                 (unsigned)body_len);
    }
    const char *auth_line_prefix = auth ? "Authorization: " : "";
    const char *auth_line = auth ? auth : "";
    const char *auth_end = auth ? "\r\n" : "";

    size_t need = strlen(method) + strlen(u->path) + strlen(u->host) + strlen(host_extra)
                  + strlen(clen) + strlen(auth_line) + body_len + 160;
    char *req = malloc(need);
    if (!req) return -1;

    int n = snprintf(req, need,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s%s\r\n"
                     "Connection: close\r\n"
                     "X-API-Version: 1.0.0\r\n"
                     "%s%s%s"
                     "%s"
                     "\r\n",
                     method, u->path, u->host, host_extra,
                     auth_line_prefix, auth_line, auth_end, clen);
    if (n < 0 || (size_t)n >= need) {
        free(req);
        return -1;
    }
    int rc = tls_write_all(tls, req, (size_t)n, deadline);
    free(req);
    if (rc != 0) return -1;
    if (body_len && tls_write_all(tls, body, body_len, deadline) != 0) return -1;
    return 0;
}

static int read_body(rdr_t *r, bool chunked, long content_length, bool have_length,
                     char **body_out, size_t *body_len)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    bool truncated = false;

    if (chunked) {
        for (;;) {
            char line[64];
            if (rdr_line(r, line, sizeof(line)) != 0) {
                free(buf);
                return -1;
            }
            char *semi = strchr(line, ';');
            if (semi) *semi = '\0';
            char *end = NULL;
            unsigned long sz = strtoul(line, &end, 16);
            if (end == line || (end && *end)) {
                free(buf);
                return -1;
            }
            if (sz == 0) {
                char trailer[256];
                for (;;) {
                    if (rdr_line(r, trailer, sizeof(trailer)) != 0) {
                        free(buf);
                        return -1;
                    }
                    if (trailer[0] == '\0') break;
                }
                break;
            }
            if (sz > MAX_BODY) {
                free(buf);
                return -1;
            }
            while (sz) {
                uint8_t tmp[256];
                size_t take = sz > sizeof(tmp) ? sizeof(tmp) : (size_t)sz;
                if (rdr_exact(r, tmp, take) != 0 || body_add(&buf, &len, &cap, tmp, take, &truncated) != 0) {
                    free(buf);
                    return -1;
                }
                sz -= take;
            }
            char crlf[8];
            if (rdr_line(r, crlf, sizeof(crlf)) != 0 || crlf[0] != '\0') {
                free(buf);
                return -1;
            }
        }
    } else if (have_length) {
        if (content_length < 0) {
            free(buf);
            return -1;
        }
        long left = content_length;
        while (left > 0) {
            uint8_t tmp[256];
            size_t take = (size_t)left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
            if (rdr_exact(r, tmp, take) != 0 || body_add(&buf, &len, &cap, tmp, take, &truncated) != 0) {
                free(buf);
                return -1;
            }
            left -= (long)take;
            if (truncated && left > MAX_BODY) break;
        }
    } else {
        for (;;) {
            int filled = rdr_fill(r);
            if (filled == 0) break;
            if (filled != 1) {
                free(buf);
                return -1;
            }
            size_t have = r->len - r->pos;
            if (body_add(&buf, &len, &cap, r->buf + r->pos, have, &truncated) != 0) {
                free(buf);
                return -1;
            }
            r->pos = r->len;
            if (truncated) break;
        }
    }

    if (truncated) ESP_LOGW(TAG, "response too large, dropping tail");
    if (!buf) {
        buf = calloc(1, 1);
        if (!buf) return -1;
        len = 0;
    }
    *body_out = buf;
    *body_len = len;
    return 0;
}

static int read_response(esp_tls_t *tls, int64_t deadline, int *status,
                         char **body_out, size_t *body_len, char **location)
{
    rdr_t r = { .tls = tls, .deadline = deadline };
    char line[768];
    size_t header_bytes = 0;
    if (rdr_line(&r, line, sizeof(line)) != 0) return -1;
    header_bytes += strlen(line);
    if (strncmp(line, "HTTP/1.", 7) != 0) return -1;
    char *sp = strchr(line, ' ');
    if (!sp) return -1;
    *status = atoi(sp + 1);
    if (*status < 100 || *status > 599) return -1;

    bool chunked = false;
    bool have_length = false;
    long content_length = -1;
    *location = NULL;

    for (;;) {
        if (rdr_line(&r, line, sizeof(line)) != 0) {
            free(*location);
            *location = NULL;
            return -1;
        }
        header_bytes += strlen(line) + 2;
        if (header_bytes > MAX_HEADER_BYTES) {
            free(*location);
            *location = NULL;
            return -1;
        }
        if (line[0] == '\0') break;
        const char *value = NULL;
        if (header_is(line, "Content-Length", &value)) {
            while (*value == ' ' || *value == '\t') value++;
            char *end = NULL;
            long n = strtol(value, &end, 10);
            if (end == value || (end && *end) || n < 0) {
                free(*location);
                *location = NULL;
                return -1;
            }
            if (have_length && content_length != n) {
                free(*location);
                *location = NULL;
                return -1;
            }
            have_length = true;
            content_length = n;
        } else if (header_is(line, "Transfer-Encoding", &value)) {
            if (contains_ci(value, "chunked")) chunked = true;
        } else if (header_is(line, "Location", &value)) {
            free(*location);
            *location = trim_dup(value);
            if (!*location) return -1;
        }
    }

    if (chunked) have_length = false;
    return read_body(&r, chunked, content_length, have_length, body_out, body_len);
}

static bool method_ok(const char *method)
{
    return strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0;
}

int muse_https_exchange(const char *url, const char *method, const char *auth,
                        const char *body, int timeout_ms, int *status,
                        char **resp, size_t *resp_len)
{
    if (status) *status = 0;
    if (resp) *resp = NULL;
    if (resp_len) *resp_len = 0;
    if (!url || !method || !status || !resp || !resp_len || !method_ok(method)) return -1;
    if (auth && strpbrk(auth, "\r\n")) return -1;
    if (timeout_ms <= 0) timeout_ms = 15000;

    parsed_url_t cur;
    if (parse_https(url, &cur) != 0) return -1;

    char method_buf[8];
    snprintf(method_buf, sizeof(method_buf), "%s", method);
    const char *use_auth = (auth && auth[0]) ? auth : NULL;
    const char *use_body = body;

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        esp_tls_t *tls = NULL;
        if (muse_tls_connect_proxy(cur.host, cur.port, timeout_ms, &tls) != ESP_OK) {
            free_url(&cur);
            return -1;
        }
        if (write_request(tls, method_buf, &cur, use_auth, use_body, deadline) != 0) {
            esp_tls_conn_destroy(tls);
            free_url(&cur);
            return -1;
        }

        int st = 0;
        char *location = NULL;
        char *body_out = NULL;
        size_t body_len = 0;
        int rr = read_response(tls, deadline, &st, &body_out, &body_len, &location);
        esp_tls_conn_destroy(tls);
        if (rr != 0) {
            free(body_out);
            free(location);
            free_url(&cur);
            return -1;
        }

        bool redirect = st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
        if (!redirect || hop == MAX_REDIRECTS) {
            *status = st;
            *resp = body_out ? body_out : calloc(1, 1);
            *resp_len = body_out ? body_len : 0;
            free(location);
            free_url(&cur);
            return *resp ? 0 : -1;
        }

        free(body_out);
        parsed_url_t next;
        int followed = location ? apply_location(&cur, location, &next) : -1;
        free(location);
        if (followed != 0) {
            *status = st;
            *resp = calloc(1, 1);
            *resp_len = 0;
            free_url(&cur);
            return *resp ? 0 : -1;
        }
        if (strcasecmp(cur.host, next.host) != 0 || cur.port != next.port) use_auth = NULL;
        if (st == 301 || st == 302 || st == 303) {
            snprintf(method_buf, sizeof(method_buf), "GET");
            use_body = NULL;
        }
        free_url(&cur);
        cur = next;
    }

    free_url(&cur);
    return -1;
}

#else

typedef int muse_http_proxy_disabled;

#endif /* MUSE_HTTP_PROXY_ENABLE */
