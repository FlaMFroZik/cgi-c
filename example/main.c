#define _XOPEN_SOURCE 700

#include "../server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int cgi_main(char *root, char *data, size_t size, int client);


/* ---------------- Access log (Apache/Nginx combined) ---------------- */

static const char *g_access_log_path = NULL;
static FILE       *g_access_log      = NULL;
static size_t      g_body_bytes      = 0;

static void log_init(void)
{
    const char *path = g_access_log_path;

    if (path != NULL && path[0] != '\0') {
        g_access_log = fopen(path, "a");
        if (g_access_log == NULL)
            perror(path);
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

static void get_client_ip(int fd, char *out, size_t size)
{
    struct sockaddr_storage ss;
    socklen_t               len = sizeof ss;

    if (size == 0)
        return;
    out[0] = '\0';

    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0)
        return;

    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &s->sin_addr, out, (socklen_t)size);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &s->sin6_addr, out, (socklen_t)size);
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
    char        method[16];
    char        target[2048];
    char        version[16];
    char        path[2048];
    char        content_type[256];
    char        referer[2048];
    char        user_agent[1024];
    size_t      head_len;
    const char *body;
    size_t      body_len;
} request_t;

static void log_access(int fd, const request_t *req, int status)
{
    char      ip[64];
    char      timestamp[64];
    char      request_line[4096];
    char      referer[2048];
    char      user_agent[1024];
    char      bytes[32];
    time_t    now;
    struct tm tm;

    get_client_ip(fd, ip, sizeof ip);
    if (ip[0] == '\0')
        strcpy(ip, "-");

    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(timestamp, sizeof timestamp, "%d/%b/%Y:%H:%M:%S %z", &tm);

    snprintf(request_line, sizeof request_line, "%s %s %s",
             req->method[0] ? req->method : "-",
             req->target[0] ? req->target : "-",
             req->version[0] ? req->version : "-");

    log_escape(req->referer, referer, sizeof referer);
    log_escape(req->user_agent, user_agent, sizeof user_agent);

    if (g_body_bytes > 0)
        snprintf(bytes, sizeof bytes, "%zu", g_body_bytes);
    else
        strcpy(bytes, "-");

    fprintf(log_stream(), "%s - - [%s] \"%s\" %d %s \"%s\" \"%s\"\n",
            ip, timestamp, request_line, status, bytes,
            referer[0] ? referer : "-",
            user_agent[0] ? user_agent : "-");
    fflush(log_stream());
}


/* ---------------- HTTP helpers ---------------- */

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_body(int fd, const void *buf, size_t len)
{
    int r = send_all(fd, buf, len);

    if (r == 0)
        g_body_bytes += len;
    return r;
}

static int send_head(int fd, int code, const char *reason, const char *type, size_t length)
{
    char head[512];
    int  n = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      code, reason, type, length);

    if (n < 0 || (size_t)n >= sizeof head)
        return -1;
    return send_all(fd, head, (size_t)n);
}

static int send_response(int fd, int code, const char *reason, const char *type,
                         const char *body, size_t length, int head_only)
{
    if (send_head(fd, code, reason, type, length) == 0 && !head_only)
        send_body(fd, body, length);
    return code;
}

static int send_response_loc(int fd, int code, const char *reason,
                             const char *location,
                             const char *type,
                             const char *body, size_t length, int head_only)
{
    char head[768];
    int  n = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Location: %s\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      code, reason, type, length, location);

    if (n < 0 || (size_t)n >= sizeof head)
        return -1;
    if (send_all(fd, head, (size_t)n) != 0)
        return -1;
    if (!head_only)
        send_body(fd, body, length);
    return code;
}

static int send_error(int fd, int code, const char *reason, int head_only)
{
    char body[256];
    int  n = snprintf(body, sizeof body, "%d %s\n", code, reason);

    return send_response(fd, code, reason, "text/plain; charset=utf-8",
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

static size_t take_word(const char *p, const char *end, char *out, size_t out_size)
{
    size_t n = 0;

    while (p + n < end && p[n] != ' ' && p[n] != '\r' && p[n] != '\n')
        n++;
    if (n < out_size) {
        memcpy(out, p, n);
        out[n] = '\0';
    } else {
        out[0] = '\0';
    }
    return n;
}

static void copy_header(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (src_len >= dst_size)
        src_len = dst_size - 1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

static void parse_headers(const char *data, size_t head_len, request_t *req)
{
    const char *p   = data;
    const char *end = data + head_len;

    while (p < end && *p != '\n')
        p++;
    if (p < end)
        p++;

    while (p < end) {
        const char *line_end = p;
        const char *colon;

        while (line_end < end && *line_end != '\n' && *line_end != '\r')
            line_end++;
        if (line_end == p)
            break;

        colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon != NULL) {
            const char *name  = p;
            size_t      nlen  = (size_t)(colon - p);
            const char *value = colon + 1;
            size_t      vlen;

            while (value < line_end && (*value == ' ' || *value == '\t'))
                value++;
            vlen = (size_t)(line_end - value);

            if (nlen == 12 && strncasecmp(name, "Content-Type", 12) == 0)
                copy_header(req->content_type, sizeof req->content_type, value, vlen);
            else if (nlen == 7 && strncasecmp(name, "Referer", 7) == 0)
                copy_header(req->referer, sizeof req->referer, value, vlen);
            else if (nlen == 10 && strncasecmp(name, "User-Agent", 10) == 0)
                copy_header(req->user_agent, sizeof req->user_agent, value, vlen);
        }

        p = line_end;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
}

static int parse_request(const char *data, size_t size, request_t *req)
{
    const char *end = data + size;
    const char *p   = data;
    const char *query;
    size_t      n, i;

    memset(req, 0, sizeof *req);

    n = take_word(p, end, req->method, sizeof req->method);
    if (n == 0 || n >= sizeof req->method || p + n >= end || p[n] != ' ')
        return 400;
    p += n + 1;

    n = take_word(p, end, req->target, sizeof req->target);
    if (n >= sizeof req->target)
        return 414;
    if (n == 0 || req->target[0] != '/')
        return 400;
    p += n;

    if (p < end && *p == ' ')
        take_word(p + 1, end, req->version, sizeof req->version);

    query = strchr(req->target, '?');
    if (url_decode(req->target, query ? (size_t)(query - req->target) : strlen(req->target),
        req->path, sizeof req->path, 0) != 0)
        return 400;

    req->head_len = size;
    for (i = 0; i < size; i++) {
        if (data[i] != '\n')
            continue;
        if (i + 1 < size && data[i + 1] == '\n') {
            req->head_len = i + 2;
            break;
        }
        if (i + 2 < size && data[i + 1] == '\r' && data[i + 2] == '\n') {
            req->head_len = i + 3;
            break;
        }
    }
    req->body     = data + req->head_len;
    req->body_len = size - req->head_len;

    parse_headers(data, req->head_len, req);
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

static int serve_file(int client, int fd, const char *path, const struct stat *st, int head_only)
{
    char        chunk[64 * 1024];
    const char *type = mime_type(path);
    ssize_t     n    = read(fd, chunk, sizeof chunk);

    if (n < 0)
        return send_error(client, 500, "Internal Server Error", head_only);

    if (type == NULL)
        type = memchr(chunk, 0, (size_t)n) ? "application/octet-stream"
        : "text/plain; charset=utf-8";

    if (send_head(client, 200, "OK", type, (size_t)st->st_size) == 0 && !head_only) {
        while (n > 0 && send_body(client, chunk, (size_t)n) == 0)
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

static int dir_listing(int client, const char *real, const char *path, int head_only)
{
    DIR           *dir;
    struct dirent *ent;
    buf_t          body = {0};
    char           title[4096];
    int            ret;

    dir = opendir(real);
    if (dir == NULL)
        return send_error(client, 403, "Forbidden", head_only);

    html_escape(path, title, sizeof title);

    if (buf_printf(&body,
        "<!doctype html>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>Index of %s</title>\n"
        "<h1>Index of %s</h1>\n"
        "<p><a href=\"/upload\">Upload a file</a></p>\n"
        "<ul>\n",
        title, title) != 0) {
        closedir(dir);
    free(body.data);
    return send_error(client, 500, "Internal Server Error", head_only);
        }

        if (strcmp(path, "/") != 0)
            buf_puts(&body, "<li><a href=\"../\">../</a></li>\n");

    while ((ent = readdir(dir)) != NULL) {
        char        full[4096];
        char        encoded[4096];
        char        escaped[4096];
        struct stat st;
        int         is_dir = 0;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (snprintf(full, sizeof full, "%s/%s", real, ent->d_name) >= (int)sizeof full)
            continue;

        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            is_dir = 1;

        if (url_encode(ent->d_name, encoded, sizeof encoded) != 0)
            continue;

        html_escape(ent->d_name, escaped, sizeof escaped);

        if (buf_printf(&body,
            "<li><a href=\"%s%s\">%s%s</a></li>\n",
            encoded, is_dir ? "/" : "",
            escaped, is_dir ? "/" : "") != 0) {
            closedir(dir);
        free(body.data);
        return send_error(client, 500, "Internal Server Error", head_only);
            }
    }

    closedir(dir);
    buf_puts(&body, "</ul>\n");

    ret = send_response(client, 200, "OK", "text/html; charset=utf-8",
                        body.data != NULL ? body.data : "",
                        body.len, head_only);

    free(body.data);
    return ret;
}

static int serve_path(int client, const char *root, const char *path, int head_only)
{
    char        file[4096];
    char       *real;
    struct stat st;
    size_t      root_len = strlen(root);
    int         fd, code, inside;

    if (!path_is_safe(path))
        return send_error(client, 403, "Forbidden", head_only);

    if (snprintf(file, sizeof file, "%s%s", root, path) >= (int)sizeof file)
        return send_error(client, 414, "URI Too Long", head_only);

    real = realpath(file, NULL);
    if (real == NULL)
        return send_error(client, 404, "Not Found", head_only);
    inside = strcmp(root, "/") == 0 ||
    (strncmp(real, root, root_len) == 0 &&
    (real[root_len] == '/' || real[root_len] == '\0'));
    if (!inside) {
        free(real);
        return send_error(client, 403, "Forbidden", head_only);
    }

    fd = open(real, O_RDONLY | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &st) != 0)
        code = send_error(client, 403, "Forbidden", head_only);
    else if (S_ISDIR(st.st_mode)) {
        if (strcmp(path, "/") == 0) {
            const char *body = "Moved to /index.html\n";

            code = send_response_loc(client, 301, "Moved Permanently",
                                     "/index.html",
                                     "text/plain; charset=utf-8",
                                     body, strlen(body), head_only);
        } else if (path[strlen(path) - 1] != '/') {
            char        loc[4096];
            const char *body = "Moved\n";

            if (snprintf(loc, sizeof loc, "%s/", path) >= (int)sizeof loc) {
                code = send_error(client, 414, "URI Too Long", head_only);
            } else {
                code = send_response_loc(client, 301, "Moved Permanently",
                                         loc,
                                         "text/plain; charset=utf-8",
                                         body, strlen(body), head_only);
            }
        } else {
            code = dir_listing(client, real, path, head_only);
        }
    }
    else if (S_ISREG(st.st_mode))
        code = serve_file(client, fd, real, &st, head_only);
    else
        code = send_error(client, 403, "Forbidden", head_only);

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

static int upload_page(int client, int head_only)
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

    return send_response(client, 200, "OK", "text/html; charset=utf-8",
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

    if (len > 0 && write(fd, data, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    close(fd);

    if (snprintf(saved_name, saved_name_size, "uploads/%s", filename) >= (int)saved_name_size)
        return -1;
    return 0;
}

static int save_upload_multipart(int client, const char *root, request_t *req, int head_only)
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
        return send_error(client, 400, "Bad Request: missing boundary", head_only);

    delim_len = (size_t)snprintf(delim, sizeof delim, "--%s", boundary);

    p = mem_find(body, body_len, delim, delim_len);
    if (p == NULL)
        return send_error(client, 400, "Bad Request: no boundary in body", head_only);
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
        return send_error(client, 400, "Bad Request: bad multipart headers", head_only);

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
                if (j == 0)
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
        return send_error(client, 400, "Bad Request: no data end", head_only);

    data_len = (size_t)(data_end - data_start);

    if (write_uploaded(root, filename, data_start, data_len,
        saved, sizeof saved) != 0)
        return send_error(client, 500, "Internal Server Error: cannot save file", head_only);

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

    return send_response(client, 200, "OK", "text/html; charset=utf-8",
                         resp, (size_t)n, head_only);
}

static int save_upload_raw(int client, const char *root, request_t *req, int head_only)
{
    char        saved[512];
    char        resp[1024];
    const char *filename = "upload.bin";
    int         n;

    if (write_uploaded(root, filename, req->body, req->body_len,
        saved, sizeof saved) != 0)
        return send_error(client, 500, "Internal Server Error: cannot save file", head_only);

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

    return send_response(client, 200, "OK", "text/html; charset=utf-8",
                         resp, (size_t)n, head_only);
}

static int save_upload(int client, const char *root, request_t *req, int head_only)
{
    if (strncasecmp(req->content_type, "multipart/form-data", 19) == 0)
        return save_upload_multipart(client, root, req, head_only);
    return save_upload_raw(client, root, req, head_only);
}


/* ---------------- CGI entry ---------------- */

int cgi_main(char *root, char *data, size_t size, int client)
{
    static int log_ready = 0;
    request_t  req;
    int        head_only, error, status = 0;

    if (!log_ready) {
        log_init();
        log_ready = 1;
    }

    g_body_bytes = 0;

    error = parse_request(data, size, &req);

    if (error == 414) {
        status = send_error(client, 414, "URI Too Long", 0);
    } else if (error != 0) {
        status = send_error(client, 400, "Bad Request", 0);
    } else {
        head_only = strcmp(req.method, "HEAD") == 0;

        if (strcmp(req.path, "/echo") == 0) {
            status = send_response(client, 200, "OK", "text/plain; charset=utf-8",
                                   data, size, head_only);
        } else if (strcmp(req.path, "/upload") == 0) {
            if (strcmp(req.method, "POST") == 0)
                status = save_upload(client, root, &req, 0);
            else if (strcmp(req.method, "GET") == 0 || head_only)
                status = upload_page(client, head_only);
            else
                status = send_error(client, 405, "Method Not Allowed", 0);
        } else if (strcmp(req.method, "GET") != 0 && !head_only) {
            status = send_error(client, 405, "Method Not Allowed", 0);
        } else {
            status = serve_path(client, root, req.path, head_only);
        }
    }

    log_access(client, &req, status);
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
            "  -            single-shot mode: read request from stdin, write to stdout\n"
            "  ROOT         document root (default: current directory)\n"
            "\n"
            "Examples:\n"
            "  %s -l access.log /var/www\n"
            "  printf 'GET / HTTP/1.1\\r\\n\\r\\n' | %s - .\n",
            prog, prog, prog, prog);
}

static int run_once(const char *root_arg)
{
    char           root[CGI_ROOT_SIZE];
    cgi_function_t cgi = cgi_main;
    char          *full, *data = NULL;
    size_t         size = 0, cap = 0, len;
    int            status;

    full = realpath(root_arg != NULL ? root_arg : ".", NULL);
    if (full == NULL) {
        perror(root_arg != NULL ? root_arg : ".");
        return 1;
    }
    len = strlen(full);
    if (len >= CGI_ROOT_SIZE) {
        fprintf(stderr, "path to directory exceeds maximum length of %d characters", CGI_ROOT_SIZE - 1);
        free(full);
        return 1;
    }
    memset(root, 0, sizeof root);
    memcpy(root, full, len);
    free(full);

    for (;;) {
        ssize_t n;

        if (size == cap) {
            char *bigger;

            cap    = cap ? cap * 2 : 4096;
            bigger = realloc(data, cap + 1);
            if (bigger == NULL) {
                perror("realloc");
                free(data);
                return 1;
            }
            data = bigger;
        }
        n = read(STDIN_FILENO, data + size, cap - size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            perror("stdin");
            free(data);
            return 1;
        }
        if (n == 0)
            break;
        size += (size_t)n;
    }
    data[size] = '\0';

    status = cgi(root, data, size, STDOUT_FILENO);
    free(data);

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

    return server_run(cgi_main, fargc, filtered);
}
