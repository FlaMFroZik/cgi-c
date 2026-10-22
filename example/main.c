#define _XOPEN_SOURCE 700

#include "../fcgi.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int cgi_main(char *root, fcgi_env_t *env, char *data, size_t size,
               fcgi_stream_t *out);


/* ---------------- Access log (Apache/Nginx combined) ---------------- */

static const char *g_access_log_path = NULL;
static FILE       *g_access_log      = NULL;
static pthread_once_t g_log_once     = PTHREAD_ONCE_INIT;

static void log_close(void);

/* Открыть журнал один раз, даже если первые запросы пришли
 * одновременно в разные потоки. */
static void log_init_once(void)
{
    const char *path = g_access_log_path;

    if (path != NULL && path[0] != '\0') {
        g_access_log = fopen(path, "a");
        if (g_access_log == NULL)
            perror(path);
        else
            atexit(log_close); /* закрыть и при обычном выходе */
    }
}

static FILE *log_stream(void)
{
    return g_access_log != NULL ? g_access_log : stderr;
}

static void log_close(void)
{
    if (g_access_log != NULL) {
        fclose(g_access_log);
        g_access_log = NULL;
    }
}

static void log_escape(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789abcdef";
    size_t            k     = 0;

    if (dst_size == 0)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    for (; *src != '\0' && k + 1 < dst_size; src++) {
        unsigned char c = (unsigned char)*src;

        if (c == '"' || c == '\\') {
            if (k + 2 >= dst_size)
                break;
            dst[k++] = '\\';
            dst[k++] = (char)c;
        } else if (c < 32 || c == 127) {
            if (k + 4 >= dst_size)
                break;
            dst[k++] = '\\';
            dst[k++] = 'x';
            dst[k++] = hex[c >> 4];
            dst[k++] = hex[c & 15];
        } else {
            dst[k++] = (char)c;
        }
    }

    dst[k] = '\0';
}

typedef struct {
    const char *method;
    const char *uri;
    const char *version;
    char        path[2048];
    const char *content_type;
    const char *referer;
    const char *user_agent;
    const char *body;
    size_t      body_len;
} request_t;

static void log_access(const fcgi_env_t *env, const request_t *req,
                         int status, size_t bytes)
{
    char      ip[64];
    char      timestamp[64];
    char      request_line[4096];
    char      referer[2048];
    char      user_agent[1024];
    char      bytes_str[32];
    time_t    now;
    struct tm tm;

    /* За FastCGI-запросом стоит веб-сервер, поэтому настоящий
     * IP клиента — в переменной REMOTE_ADDR. */
    snprintf(ip, sizeof ip, "%s",
             fcgi_env_get(env, "REMOTE_ADDR") != NULL
                 ? fcgi_env_get(env, "REMOTE_ADDR")
                 : "-");

    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(timestamp, sizeof timestamp, "%d/%b/%Y:%H:%M:%S %z", &tm);

    snprintf(request_line, sizeof request_line, "%s %s %s",
             req->method[0] ? req->method : "-",
             req->uri[0] ? req->uri : "-",
             req->version[0] ? req->version : "-");

    log_escape(req->referer, referer, sizeof referer);
    log_escape(req->user_agent, user_agent, sizeof user_agent);

    if (bytes > 0)
        snprintf(bytes_str, sizeof bytes_str, "%zu", bytes);
    else
        strcpy(bytes_str, "-");

    fprintf(log_stream(), "%s - - [%s] \"%s\" %d %s \"%s\" \"%s\"\n",
            ip, timestamp, request_line, status, bytes_str,
            referer[0] ? referer : "-",
            user_agent[0] ? user_agent : "-");
    fflush(log_stream());
}


/* ---------------- FastCGI response helpers ---------------- */

static int send_head(fcgi_stream_t *out, int code, const char *reason,
                       const char *type, size_t length)
{
    /* В FastCGI статус ответа передаётся заголовком «Status:»,
     * а не первой строкой HTTP — веб-сервер соберёт настоящий
     * HTTP-ответ сам. */
    return fcgi_printf(out,
                       "Status: %d %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %zu\r\n"
                       "\r\n",
                       code, reason, type, length);
}

static int send_response(fcgi_stream_t *out, int code, const char *reason,
                           const char *type, const char *body,
                           size_t length, int head_only)
{
    if (send_head(out, code, reason, type, length) == 0 && !head_only)
        fcgi_write(out, body, length);
    return code;
}

static int send_response_loc(fcgi_stream_t *out, int code,
                               const char *reason, const char *location,
                               const char *type, const char *body,
                               size_t length, int head_only)
{
    int rc = fcgi_printf(out,
                         "Status: %d %s\r\n"
                         "Content-Type: %s\r\n"
                         "Content-Length: %zu\r\n"
                         "Location: %s\r\n"
                         "\r\n",
                         code, reason, type, length, location);

    if (rc != 0)
        return -1;
    if (!head_only && length > 0)
        fcgi_write(out, body, length);
    return code;
}

static int send_error(fcgi_stream_t *out, int code, const char *reason,
                        int head_only)
{
    char body[256];
    int  n = snprintf(body, sizeof body, "%d %s\n", code, reason);

    return send_response(out, code, reason, "text/plain; charset=utf-8",
                           body, (size_t)n, head_only);
}


/* ---------------- Request parsing ---------------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *src, size_t n, char *out, size_t out_size, int plus)
{
    size_t i, k = 0;

    for (i = 0; i < n; i++) {
        int c = (unsigned char)src[i];

        if (c == '%' && i + 2 < n && hex_value(src[i + 1]) >= 0 && hex_value(src[i + 2]) >= 0) {
            c = hex_value(src[i + 1]) * 16 + hex_value(src[i + 2]);
            i += 2;
        } else if (c == '+' && plus) {
            c = ' ';
        }
        if (c == 0 || k + 1 >= out_size)
            return -1;
        out[k++] = (char)c;
    }
    out[k] = '\0';
    return 0;
}

/*
 * Запрос уже разобран веб-сервером: метод, URI и заголовки
 * пришли в PARAMS, тело — в STDIN. Здесь только вытаскиваем
 * нужное из окружения и раскодируем путь.
 */
static int fill_request(const fcgi_env_t *env, const char *data,
                            size_t size, request_t *req)
{
    const char *uri;
    const char *query;

    memset(req, 0, sizeof *req);

    req->method      = fcgi_env_get(env, "REQUEST_METHOD");
    req->version     = fcgi_env_get(env, "SERVER_PROTOCOL");
    req->content_type = fcgi_env_get(env, "CONTENT_TYPE");
    req->referer     = fcgi_env_get(env, "HTTP_REFERER");
    req->user_agent  = fcgi_env_get(env, "HTTP_USER_AGENT");
    req->body        = data;
    req->body_len    = size;
    if (req->method == NULL) req->method = "";
    if (req->version == NULL) req->version = "";
    if (req->content_type == NULL) req->content_type = "";
    if (req->referer == NULL) req->referer = "";
    if (req->user_agent == NULL) req->user_agent = "";

    uri = fcgi_env_get(env, "REQUEST_URI");
    if (uri == NULL)
        uri = fcgi_env_get(env, "PATH_INFO");
    if (uri == NULL)
        uri = "/";
    req->uri = uri;

    query = strchr(uri, '?');
    if (url_decode(uri, query ? (size_t)(query - uri) : strlen(uri),
        req->path, sizeof req->path, 0) != 0)
        return 400;
    return 0;
}


/* ---------------- Static file serving ---------------- */

static const char *mime_type(const char *path)
{
    static const char *const types[][2] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".css",  "text/css; charset=utf-8" },
        { ".js",   "text/javascript; charset=utf-8" },
        { ".json", "application/json" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".md",   "text/plain; charset=utf-8" },
        { ".c",    "text/plain; charset=utf-8" },
        { ".h",    "text/plain; charset=utf-8" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".svg",  "image/svg+xml" },
        { ".ico",  "image/x-icon" },
        { ".pdf",  "application/pdf" },
    };
    const char *dot = strrchr(path, '.');
    size_t      i;

    if (dot == NULL || strchr(dot, '/') != NULL)
        return NULL;
    for (i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcasecmp(dot, types[i][0]) == 0)
            return types[i][1];
    return NULL;
}

static int serve_file(fcgi_stream_t *out, int fd, const char *path,
                        const struct stat *st, int head_only)
{
    char        chunk[64 * 1024];
    const char *type = mime_type(path);
    ssize_t     n    = read(fd, chunk, sizeof chunk);

    if (n < 0)
        return send_error(out, 500, "Internal Server Error", head_only);

    if (type == NULL)
        type = memchr(chunk, 0, (size_t)n) ? "application/octet-stream"
        : "text/plain; charset=utf-8";

    if (send_head(out, 200, "OK", type, (size_t)st->st_size) == 0 &&
        !head_only) {
        while (n > 0 && fcgi_write(out, chunk, (size_t)n) == 0)
            n = read(fd, chunk, sizeof chunk);
    }
    return 200;
}

static int path_is_safe(const char *path)
{
    return path[0] == '/' && strstr(path, "/.") == NULL;
}


/* ---------------- Dynamic buffer ---------------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} buf_t;

static int buf_reserve(buf_t *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    size_t cap;
    char  *p;

    if (need <= b->cap)
        return 0;

    cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;

    p = realloc(b->data, cap);
    if (p == NULL)
        return -1;

    b->data = p;
    b->cap  = cap;
    return 0;
}

static int buf_append(buf_t *b, const char *s, size_t n)
{
    if (buf_reserve(b, n) != 0)
        return -1;

    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int buf_puts(buf_t *b, const char *s)
{
    return buf_append(b, s, strlen(s));
}

static int buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int     n;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        va_end(ap2);
        return -1;
    }

    if (buf_reserve(b, (size_t)n) != 0) {
        va_end(ap2);
        return -1;
    }

    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return 0;
}


/* ---------------- HTML helpers ---------------- */

static void html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t k = 0;

    for (; *src != '\0' && k + 1 < dst_size; src++) {
        const char *rep = NULL;

        switch (*src) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default:   break;
        }

        if (rep != NULL) {
            size_t n = strlen(rep);
            if (k + n >= dst_size)
                break;
            memcpy(dst + k, rep, n);
            k += n;
        } else {
            dst[k++] = *src;
        }
    }

    dst[k] = '\0';
}

static int url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t            k     = 0;

    for (; *src != '\0'; src++) {
        unsigned char c = (unsigned char)*src;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (k + 1 >= dst_size)
                return -1;
            dst[k++] = (char)c;
        } else {
            if (k + 3 >= dst_size)
                return -1;
            dst[k++] = '%';
            dst[k++] = hex[c >> 4];
            dst[k++] = hex[c & 15];
        }
    }

    if (k >= dst_size)
        return -1;

    dst[k] = '\0';
    return 0;
}


/* ---------------- Directory listing ---------------- */

static int dir_listing(fcgi_stream_t *out, const char *real,
                         const char *path, int head_only)
{
    DIR           *dir;
    struct dirent *ent;
    buf_t          body = {0};
    char           title[4096];
    int            ret;

    dir = opendir(real);
    if (dir == NULL) {
        int saved = errno;

        return send_error(out, saved == ENOENT ? 404 :
                               saved == EACCES ? 403 : 500,
                          saved == ENOENT ? "Not Found" :
                          saved == EACCES ? "Forbidden"
                                          : "Internal Server Error",
                          head_only);
    }

    html_escape(path, title, sizeof title);

    if (buf_printf(&body,
        "<!doctype html>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>Index of %s</title>\n"
        "<h1>Index of %s</h1>\n"
        "<p><a href=\"/upload\">Upload a file</a></p>\n"
        "<ul>\n",
        title, title) != 0)
        goto fail;

    if (strcmp(path, "/") != 0 &&
        buf_puts(&body, "<li><a href=\"../\">../</a></li>\n") != 0)
        goto fail;

    while ((ent = readdir(dir)) != NULL) {
        char        full[4096];
        char        encoded[4096];
        char        escaped[4096];
        struct stat st;
        int         is_dir = 0;

        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0)
            continue;

        if (snprintf(full, sizeof full, "%s/%s", real, ent->d_name) >=
            (int)sizeof full)
            continue;

        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            is_dir = 1;

        if (url_encode(ent->d_name, encoded, sizeof encoded) != 0)
            continue;

        html_escape(ent->d_name, escaped, sizeof escaped);

        if (buf_printf(&body,
            "<li><a href=\"%s%s\">%s%s</a></li>\n",
            encoded, is_dir ? "/" : "",
            escaped, is_dir ? "/" : "") != 0)
            goto fail;
    }

    if (buf_puts(&body, "</ul>\n") != 0)
        goto fail;

    closedir(dir);
    /* body.data не NULL: первый buf_printf уже выделил буфер. */
    ret = send_response(out, 200, "OK", "text/html; charset=utf-8",
                        body.data, body.len, head_only);
    free(body.data);
    return ret;

fail:
    closedir(dir);
    free(body.data);
    return send_error(out, 500, "Internal Server Error", head_only);
}

static int serve_index(fcgi_stream_t *out, const char *dir, const char *path,
                         int head_only)
{
    char        index[4096];
    struct stat st;
    int         fd, code;

    if (strcmp(dir, "/") == 0) {
        if (snprintf(index, sizeof index, "/index.html") >=
            (int)sizeof index)
            return dir_listing(out, dir, path, head_only);
    } else if (snprintf(index, sizeof index, "%s/index.html", dir) >=
               (int)sizeof index) {
        return dir_listing(out, dir, path, head_only);
    }

    fd = open(index, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == EISDIR)
            return dir_listing(out, dir, path, head_only);
        return send_error(out, errno == EACCES ? 403 : 500,
                          errno == EACCES ? "Forbidden"
                                          : "Internal Server Error",
                          head_only);
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return dir_listing(out, dir, path, head_only);
    }

    code = serve_file(out, fd, index, &st, head_only);
    close(fd);
    return code;
}

static int serve_path(fcgi_stream_t *out, const char *root,
                        const char *path, int head_only)
{
    char        file[4096];
    char       *real;
    struct stat st;
    size_t      root_len = strlen(root);
    int         fd, code, inside;

    if (!path_is_safe(path))
        return send_error(out, 403, "Forbidden", head_only);

    if (snprintf(file, sizeof file, "%s%s", root, path) >= (int)sizeof file)
        return send_error(out, 414, "URI Too Long", head_only);

    real = realpath(file, NULL);
    if (real == NULL)
        return send_error(out, 404, "Not Found", head_only);
    inside = strcmp(root, "/") == 0 ||
    (strncmp(real, root, root_len) == 0 &&
    (real[root_len] == '/' || real[root_len] == '\0'));
    if (!inside) {
        free(real);
        return send_error(out, 403, "Forbidden", head_only);
    }

    fd = open(real, O_RDONLY);
    if (fd < 0) {
        int saved = errno;

        /* Файл исчез между realpath() и open() — это 404, а не
         * 403, как было раньше; нет прав — 403; остальное — 500. */
        code = send_error(out,
                          saved == ENOENT || saved == ENOTDIR ? 404 :
                          saved == EACCES ? 403 : 500,
                          saved == ENOENT || saved == ENOTDIR ? "Not Found" :
                          saved == EACCES ? "Forbidden"
                                          : "Internal Server Error",
                          head_only);
    } else if (fstat(fd, &st) != 0) {
        code = send_error(out, 500, "Internal Server Error", head_only);
    } else if (S_ISDIR(st.st_mode)) {
        if (path[strlen(path) - 1] != '/') {
            char        loc[4096];
            const char *body = "Moved\n";

            if (snprintf(loc, sizeof loc, "%s/", path) >= (int)sizeof loc) {
                code = send_error(out, 414, "URI Too Long", head_only);
            } else {
                code = send_response_loc(out, 301, "Moved Permanently",
                                         loc,
                                         "text/plain; charset=utf-8",
                                         body, strlen(body), head_only);
            }
        } else {
            code = serve_index(out, real, path, head_only);
        }
    }
    else if (S_ISREG(st.st_mode))
        code = serve_file(out, fd, real, &st, head_only);
    else
        code = send_error(out, 403, "Forbidden", head_only);

    if (fd >= 0)
        close(fd);
    free(real);
    return code;
}


/* ---------------- Upload ---------------- */

static const char *mem_find(const char *hay, size_t hay_len, const char *needle, size_t needle_len)
{
    size_t i;

    if (needle_len == 0 || hay_len < needle_len)
        return NULL;
    for (i = 0; i + needle_len <= hay_len; i++)
        if (memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    return NULL;
}

static int parse_boundary(const char *content_type, char *boundary, size_t size)
{
    const char *p = strstr(content_type, "boundary=");
    size_t      n = 0;

    if (p == NULL)
        return -1;
    p += 9;

    if (*p == '"') {
        const char *end;
        p++;
        end = strchr(p, '"');
        if (end == NULL)
            return -1;
        n = (size_t)(end - p);
    } else {
        while (p[n] != '\0' && p[n] != ';' && p[n] != ' ' &&
            p[n] != '\r' && p[n] != '\n')
            n++;
    }

    if (n == 0 || n + 1 >= size)
        return -1;
    memcpy(boundary, p, n);
    boundary[n] = '\0';
    return 0;
}

static int upload_page(fcgi_stream_t *out, int head_only)
{
    const char *body =
    "<!doctype html>\n"
    "<meta charset=\"utf-8\">\n"
    "<title>Upload</title>\n"
    "<h1>Upload file</h1>\n"
    "<form method=\"post\" action=\"/upload\" enctype=\"multipart/form-data\">\n"
    "<p><input type=\"file\" name=\"file\"></p>\n"
    "<p><button type=\"submit\">Upload</button></p>\n"
    "</form>\n"
    "<p><a href=\"/\">Back</a></p>\n";

    return send_response(out, 200, "OK", "text/html; charset=utf-8",
                         body, strlen(body), head_only);
}

static int write_uploaded(const char *root, const char *filename,
                          const char *data, size_t len,
                          char *saved_name, size_t saved_name_size)
{
    char dir[4096];
    char full[4096];
    int  fd;

    if (snprintf(dir, sizeof dir, "%s/uploads", root) >= (int)sizeof dir)
        return -1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;

    if (snprintf(full, sizeof full, "%s/%s", dir, filename) >= (int)sizeof full)
        return -1;

    fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    /* write() может записать меньше, чем попросили, — доисываем. */
    while (len > 0) {
        ssize_t n = write(fd, data, len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(full);
            return -1;
        }
        if (n == 0) {
            close(fd);
            unlink(full);
            return -1;
        }
        data += (size_t)n;
        len  -= (size_t)n;
    }
    close(fd);

    if (snprintf(saved_name, saved_name_size, "uploads/%s", filename) >= (int)saved_name_size)
        return -1;
    return 0;
}

static int save_upload_multipart(fcgi_stream_t *out, const char *root,
                                   request_t *req, int head_only)
{
    char        boundary[256];
    char        delim[300];
    const char *body     = req->body;
    size_t      body_len = req->body_len;
    const char *p, *headers_end, *data_start, *data_end, *q;
    char        filename[512] = "upload.bin";
    char        saved[512];
    char        resp[2048];
    size_t      delim_len;
    size_t      data_len;
    int         n;

    if (parse_boundary(req->content_type, boundary, sizeof boundary) != 0)
        return send_error(out, 400, "Bad Request: missing boundary", head_only);

    delim_len = (size_t)snprintf(delim, sizeof delim, "--%s", boundary);

    p = mem_find(body, body_len, delim, delim_len);
    if (p == NULL)
        return send_error(out, 400, "Bad Request: no boundary in body", head_only);
    p += delim_len;

    if (p + 1 < body + body_len && p[0] == '\r' && p[1] == '\n')
        p += 2;
    else if (p < body + body_len && p[0] == '\n')
        p += 1;

    headers_end = NULL;
    data_start  = NULL;
    for (q = p; q + 1 < body + body_len; q++) {
        if (q[0] == '\r' && q[1] == '\n' && q + 3 < body + body_len &&
            q[2] == '\r' && q[3] == '\n') {
            headers_end = q;
        data_start  = q + 4;
        break;
            }
            if (q[0] == '\n' && q[1] == '\n') {
                headers_end = q;
                data_start  = q + 2;
                break;
            }
    }
    if (headers_end == NULL)
        return send_error(out, 400, "Bad Request: bad multipart headers", head_only);

    {
        const char *fn = mem_find(p, (size_t)(headers_end - p), "filename=\"", 10);
        if (fn != NULL) {
            const char *fe;
            fn += 10;
            fe = memchr(fn, '"', (size_t)(headers_end - fn));
            if (fe != NULL) {
                size_t n2 = (size_t)(fe - fn);
                size_t j  = 0;
                size_t i;

                for (i = 0; i < n2 && j + 1 < sizeof filename; i++) {
                    char c = fn[i];
                    if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 32)
                        continue;
                    filename[j++] = c;
                }
                filename[j] = '\0';
                if (j == 0 || strcmp(filename, ".") == 0 ||
                    strcmp(filename, "..") == 0)
                    strcpy(filename, "upload.bin");
            }
        }
    }

    data_end = NULL;
    for (q = data_start; q + delim_len <= body + body_len; q++) {
        if ((q == data_start || q[-1] == '\n') &&
            memcmp(q, delim, delim_len) == 0) {
            data_end = q;
            if (data_end > data_start && data_end[-1] == '\n') data_end--;
            if (data_end > data_start && data_end[-1] == '\r') data_end--;
            break;
        }
    }
    if (data_end == NULL)
        return send_error(out, 400, "Bad Request: no data end", head_only);

    data_len = (size_t)(data_end - data_start);

    if (write_uploaded(root, filename, data_start, data_len,
        saved, sizeof saved) != 0)
        return send_error(out, 500, "Internal Server Error: cannot save file", head_only);

    n = snprintf(resp, sizeof resp,
                 "<!doctype html>\n"
                 "<meta charset=\"utf-8\">\n"
                 "<title>Uploaded</title>\n"
                 "<h1>Uploaded</h1>\n"
                 "<p>Saved as <code>%s</code> (%zu bytes).</p>\n"
                 "<p><a href=\"/%s\">Open</a> | "
                 "<a href=\"/upload\">Upload another</a> | "
                 "<a href=\"/\">Back</a></p>\n",
                 saved, data_len, saved);

    return send_response(out, 200, "OK", "text/html; charset=utf-8",
                         resp, (size_t)n, head_only);
}

static int save_upload_raw(fcgi_stream_t *out, const char *root,
                             request_t *req, int head_only)
{
    char        saved[512];
    char        resp[1024];
    const char *filename = "upload.bin";
    int         n;

    if (write_uploaded(root, filename, req->body, req->body_len,
        saved, sizeof saved) != 0)
        return send_error(out, 500, "Internal Server Error: cannot save file", head_only);

    n = snprintf(resp, sizeof resp,
                 "<!doctype html>\n"
                 "<meta charset=\"utf-8\">\n"
                 "<title>Uploaded</title>\n"
                 "<h1>Uploaded</h1>\n"
                 "<p>Saved as <code>%s</code> (%zu bytes).</p>\n"
                 "<p><a href=\"/%s\">Open</a> | "
                 "<a href=\"/upload\">Upload another</a> | "
                 "<a href=\"/\">Back</a></p>\n",
                 saved, req->body_len, saved);

    return send_response(out, 200, "OK", "text/html; charset=utf-8",
                         resp, (size_t)n, head_only);
}

static int save_upload(fcgi_stream_t *out, const char *root,
                         request_t *req, int head_only)
{
    if (strncasecmp(req->content_type, "multipart/form-data", 19) == 0)
        return save_upload_multipart(out, root, req, head_only);
    return save_upload_raw(out, root, req, head_only);
}


/* ---------------- FastCGI entry ---------------- */

int cgi_main(char *root, fcgi_env_t *env, char *data, size_t size,
               fcgi_stream_t *out)
{
    request_t  req;
    int        head_only, error, status = 0;

    pthread_once(&g_log_once, log_init_once);

    error = fill_request(env, data, size, &req);

    if (error == 400) {
        status = send_error(out, 400, "Bad Request", 0);
    } else {
        head_only = strcmp(req.method, "HEAD") == 0;

        if (strcmp(req.path, "/echo") == 0) {
            if (size == 0) {
                /* Пустой ответ ничего не говорит клиенту: браузер
                 * покажет пустую страницу. Отвечаем пояснением. */
                static const char empty_body[] =
                    "(request body is empty)\n";

                status = send_response(out, 200, "OK",
                                       "text/plain; charset=utf-8",
                                       empty_body, sizeof empty_body - 1,
                                       head_only);
            } else {
                status = send_response(out, 200, "OK",
                                       "text/plain; charset=utf-8",
                                       data, size, head_only);
            }
        } else if (strcmp(req.path, "/upload") == 0) {
            if (strcmp(req.method, "POST") == 0)
                status = save_upload(out, root, &req, 0);
            else if (strcmp(req.method, "GET") == 0 || head_only)
                status = upload_page(out, head_only);
            else
                status = send_error(out, 405, "Method Not Allowed", 0);
        } else if (strcmp(req.method, "GET") != 0 && !head_only) {
            status = send_error(out, 405, "Method Not Allowed", 0);
        } else {
            status = serve_path(out, root, req.path, head_only);
        }
    }

    log_access(env, &req, status, fcgi_stream_bytes(out));
    return status;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Use: %s [-l LOGFILE] [ROOT]\n"
            "     %s [-l LOGFILE] - [ROOT] < request\n"
            "\n"
            "Options:\n"
            "  -l LOGFILE   write access log in Apache/Nginx combined format\n"
            "  -            single-shot mode: read environment as\n"
            "               NAME=VALUE lines (empty line ends them),\n"
            "               then the request body from stdin;\n"
            "               write the answer to stdout\n"
            "  ROOT         document root (default: current directory)\n"
            "\n"
            "Examples:\n"
            "  %s -l access.log /var/www\n"
            "  printf 'REQUEST_METHOD=GET\\nREQUEST_URI=/\\n\\n' | %s - .\n",
            prog, prog, prog, prog);
}

static int run_once(const char *root_arg)
{
    char           root[FCGI_ROOT_SIZE];
    fcgi_env_t    *env;
    fcgi_stream_t  out;
    char          *all = NULL, *body, *p, *line_end;
    size_t         cap = 0, len = 0, body_len;
    int            status;

    {
        char *full = realpath(root_arg != NULL ? root_arg : ".", NULL);
        size_t plen;

        if (full == NULL) {
            perror(root_arg != NULL ? root_arg : ".");
            return 1;
        }
        plen = strlen(full);
        if (plen >= FCGI_ROOT_SIZE) {
            fprintf(stderr, "path to directory exceeds maximum length of %d characters", FCGI_ROOT_SIZE - 1);
            free(full);
            return 1;
        }
        memset(root, 0, sizeof root);
        memcpy(root, full, plen);
        free(full);
    }

    for (;;) {
        ssize_t n;

        if (len == cap) {
            char *bigger;

            cap    = cap ? cap * 2 : 4096;
            bigger = realloc(all, cap + 1);
            if (bigger == NULL) {
                perror("realloc");
                free(all);
                return 1;
            }
            all = bigger;
        }
        n = read(STDIN_FILENO, all + len, cap - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            perror("stdin");
            free(all);
            return 1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    all[len] = '\0';

    /* Окружение: строки NAME=VALUE, конец — пустая строка.
     * Разбираем «на месте»: терминаторы превращаем в '\0'. */
    env = fcgi_env_create();
    if (env == NULL) {
        free(all);
        return 1;
    }
    p = all;
    while (p < all + len) {
        char *eq;
        size_t line_len;

        line_end = memchr(p, '\n', (size_t)(all + len - p));
        line_len = line_end != NULL ? (size_t)(line_end - p)
                                    : (size_t)(all + len - p);
        if (line_len == 0 || (line_len == 1 && p[0] == '\r')) {
            p = line_end != NULL ? line_end + 1 : all + len;
            break; /* пустая строка: дальше — тело */
        }
        eq = memchr(p, '=', line_len);
        if (eq != NULL) {
            *eq = '\0';
            if (line_end != NULL) {
                if (line_end > p && line_end[-1] == '\r')
                    line_end[-1] = '\0';
                *line_end = '\0';
            }
            fcgi_env_add(env, p, eq + 1);
        }
        p = line_end != NULL ? line_end + 1 : all + len;
    }
    body = p;
    body_len = (size_t)(all + len - p);

    /* Режим отладки: ответ — «голые» байты на stdout, без
     * записей FastCGI (так его удобно читать глазами). */
    out.fd         = STDOUT_FILENO;
    out.request_id = 0;
    out.framed     = 0;
    out.written    = 0;

    status = cgi_main(root, env, body, body_len, &out);

    fcgi_env_free(env);
    free(all);
    log_close();
    return status < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    char *filtered[64];
    int   fargc = 0;
    int   i;

    filtered[fargc++] = argv[0];
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            g_access_log_path = argv[i + 1];
            i++;
            continue;
        }
        if (fargc < (int)(sizeof filtered / sizeof filtered[0]) - 1)
            filtered[fargc++] = argv[i];
    }
    filtered[fargc] = NULL;

    if (fargc > 1 && strcmp(filtered[1], "-") == 0) {
        if (fargc > 3) {
            usage(argv[0]);
            return 2;
        }
        return run_once(fargc > 2 ? filtered[2] : NULL);
    }

    return fcgi_run(cgi_main, fargc, filtered);
}
