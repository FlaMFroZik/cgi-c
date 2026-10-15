#define _XOPEN_SOURCE 700

#include "../server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int cgi_main(char *root, char *data, size_t size, int client);


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
        send_all(fd, body, length);
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
        send_all(fd, body, length);
    return code;
}

static int send_error(int fd, int code, const char *reason, int head_only)
{
    char body[256];
    int  n = snprintf(body, sizeof body, "%d %s\n", code, reason);

    return send_response(fd, code, reason, "text/plain; charset=utf-8",
                         body, (size_t)n, head_only);
}

typedef struct {
    char        method[16];
    char        target[2048];
    char        version[16];
    char        path[2048];
    size_t      head_len;
    const char *body;
    size_t      body_len;
} request_t;

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
    return 0;
}

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
        while (n > 0 && send_all(client, chunk, (size_t)n) == 0)
            n = read(fd, chunk, sizeof chunk);
    }
    return 200;
}

static int path_is_safe(const char *path)
{
    return path[0] == '/' && strstr(path, "/.") == NULL;
}

static int dir_response(int client, const char *path, int head_only)
{
    char        loc[4096];
    char        body[512];
    const char *reason = "Multiple Choices";
    int         n;

    if (path[0] != '\0' && path[strlen(path) - 1] == '/')
        snprintf(loc, sizeof loc, "%s", path);
    else
        snprintf(loc, sizeof loc, "%s/", path);

    n = snprintf(body, sizeof body,
                 "<!doctype html>\n<meta charset=\"utf-8\">\n"
                 "<title>300 Multiple Choices</title>\n"
                 "<h1>300 Multiple Choices</h1>\n"
                 "<p>List of this directory: "
                 "<a href=\"%s\">%s</a>.</p>\n",
                 loc, loc);

    return send_response_loc(client, 300, reason, loc,
                             "text/html; charset=utf-8",
                             body, (size_t)n, head_only);
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
    else if (S_ISDIR(st.st_mode))
        code = dir_response(client, path, head_only);
        else if (S_ISREG(st.st_mode))
            code = serve_file(client, fd, real, &st, head_only);
    else
        code = send_error(client, 403, "Forbidden", head_only);

    if (fd >= 0)
        close(fd);
    free(real);
    return code;
}

/* ======================================================================== */
/* CGI-функция                                                              */
/* ======================================================================== */

int cgi_main(char *root, char *data, size_t size, int client)
{
    request_t req;
    int       head_only, error;

    error = parse_request(data, size, &req);
    if (error == 414)
        return send_error(client, 414, "URI Too Long", 0);
    if (error != 0)
        return send_error(client, 400, "Bad Request", 0);
    head_only = strcmp(req.method, "HEAD") == 0;

    if (strcmp(req.path, "/echo") == 0)
        return send_response(client, 200, "OK", "text/plain; charset=utf-8",
                             data, size, head_only);

        if (strcmp(req.method, "GET") != 0 && !head_only)
            return send_error(client, 405, "Method Not Allowed", 0);
    return serve_path(client, root, req.path, head_only);
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

    fprintf(stderr, "\n[cgi_main returned %d]\n", status);
    return status < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-") == 0) {
        if (argc > 3) {
            fprintf(stderr,
                    "Use: %s - [ROOT] < request\n"
                    "Example: printf 'GET / HTTP/1.1\\r\\n\\r\\n' | %s - .\n",
                    argv[0], argv[0]);
            return 2;
        }
        return run_once(argc > 2 ? argv[2] : NULL);
    }

    return server_run(cgi_main, argc, argv);
}
