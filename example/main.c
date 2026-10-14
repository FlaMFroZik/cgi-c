/*
 * example/main.c — пример CGI для cgi-server.
 *
 * В этом файле вместе находятся CGI-функция и функция main:
 *
 *   cgi_main()  CGI-функция. Makefile собирает файл в библиотеку
 *               example/cgi.so; сервер импортирует из неё cgi_main
 *               (dlopen + dlsym -> указатель на функцию) и вызывает её
 *               для каждого запроса в одном из рабочих потоков.
 *
 *   main()      обычная точка входа. Makefile собирает этот же файл
 *               и как программу example/cgi: она читает «голые» данные
 *               запроса из stdin, вызывает cgi_main() и печатает ответ
 *               в stdout. Так CGI можно проверить вообще без сервера:
 *
 *                 printf 'GET /echo HTTP/1.1\r\nHost: x\r\n\r\n' | ./example/cgi .
 *
 * Что отвечает пример:
 *
 *   GET  /        HTML-страница: аргументы CGI-функции (root, data, size,
 *                 client), первая строка запроса, сырые данные, форма для
 *                 POST и список файлов в root;
 *   POST /        та же страница, плюс поля отправленной формы;
 *   *    /echo    сырые данные запроса обратно, как text/plain;
 *   GET  /путь    файл из папки root (или список файлов, если это папка).
 */
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 + XSI: realpath(), scandir() */

#include "../import.h" /* CGI_ROOT_SIZE и тип cgi_function_t */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* CGI-функция. Её тип в точности совпадает с cgi_function_t из import.h. */
int cgi_main(char *root, char *data, size_t size, int client);

#define SHOW_MAX (64 * 1024) /* сколько сырых данных показывать на странице */

/* ======================================================================== */
/* Отправка ответа                                                          */
/* ======================================================================== */

/* Записать все len байт: write() может записать только часть. */
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

/* Строка статуса и заголовки. CGI сама формирует ВЕСЬ HTTP-ответ. */
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

/* Заголовки + тело. Возвращает code — сервер запишет его в журнал. */
static int send_response(int fd, int code, const char *reason, const char *type,
                         const char *body, size_t length, int head_only)
{
    if (send_head(fd, code, reason, type, length) == 0 && !head_only)
        send_all(fd, body, length);
    return code;
}

static int send_error(int fd, int code, const char *reason, int head_only)
{
    char body[512];
    int  n = snprintf(body, sizeof body,
                      "<!doctype html>\n<meta charset=\"utf-8\">\n<title>%d %s</title>\n"
                      "<h1>%d %s</h1>\n<p><a href=\"/\">На главную</a></p>\n",
                      code, reason, code, reason);

    return send_response(fd, code, reason, "text/html; charset=utf-8", body, (size_t)n, head_only);
}

/* ======================================================================== */
/* Растущая строка — для сборки HTML-страницы в памяти                      */
/* ======================================================================== */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    failed; /* 1 — не хватило памяти */
} text_t;

static void text_add(text_t *t, const char *s, size_t n)
{
    size_t need;

    if (t->failed)
        return;
    if (n >= SIZE_MAX / 4 - t->len) { /* защита от переполнения size_t */
        t->failed = 1;
        return;
    }
    need = t->len + n + 1;
    if (t->data == NULL || need > t->cap) {
        size_t cap = t->cap ? t->cap : 4096;
        char  *bigger;

        while (cap < need)
            cap *= 2;
        bigger = realloc(t->data, cap);
        if (bigger == NULL) {
            t->failed = 1;
            return;
        }
        t->data = bigger;
        t->cap  = cap;
    }
    memcpy(t->data + t->len, s, n);
    t->len += n;
    t->data[t->len] = '\0';
}

static void text_str(text_t *t, const char *s)
{
    text_add(t, s, strlen(s));
}

static void text_printf(text_t *t, const char *format, ...)
{
    char    buf[1024];
    va_list ap;
    int     n;

    va_start(ap, format);
    n = vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf)
        t->failed = 1; /* в примере форматируем только короткие строки */
    else
        text_add(t, buf, (size_t)n);
}

/* Текст для HTML: & < > " заменяются на сущности. */
static void text_html(text_t *t, const char *s, size_t n)
{
    size_t i, from = 0;

    for (i = 0; i < n; i++) {
        const char *entity = NULL;

        switch (s[i]) {
        case '&': entity = "&amp;";  break;
        case '<': entity = "&lt;";   break;
        case '>': entity = "&gt;";   break;
        case '"': entity = "&quot;"; break;
        default:  continue;
        }
        text_add(t, s + from, i - from);
        text_str(t, entity);
        from = i + 1;
    }
    text_add(t, s + from, n - from);
}

/* Сырые байты: \r, \n и \t видны как метки, прочие управляющие — как \xHH. */
static void text_raw(text_t *t, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\r')
            text_str(t, "<b>\\r</b>");
        else if (c == '\n')
            text_str(t, "<b>\\n</b>\n");
        else if (c == '\t')
            text_str(t, "<b>\\t</b>");
        else if (c < 0x20 || c == 0x7f)
            text_printf(t, "<b>\\x%02X</b>", c);
        else
            text_html(t, s + i, 1);
    }
}

/* Путь для ссылки (href): всё, кроме букв, цифр и -._~/, — в виде %XX. */
static void text_url(text_t *t, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";

    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr("-._~/", c) != NULL) {
            text_add(t, s, 1);
        } else {
            char esc[3];

            esc[0] = '%';
            esc[1] = hex[c >> 4];
            esc[2] = hex[c & 15];
            text_add(t, esc, 3);
        }
    }
}

/* ======================================================================== */
/* Разбор «голых» данных                                                    */
/* ======================================================================== */

typedef struct {
    char        method[16];   /* GET, POST, HEAD...                    */
    char        target[2048]; /* как в запросе: /путь?запрос           */
    char        version[16];  /* HTTP/1.1 (может отсутствовать)        */
    char        path[2048];   /* путь без ?запроса, %XX раскодированы  */
    size_t      head_len;     /* длина строки запроса и заголовков     */
    const char *body;         /* тело запроса — указатель внутрь data  */
    size_t      body_len;
} request_t;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Раскодировать %XX (и '+' в пробел, если plus) из src[0..n) в out.
 * -1, если результат не помещается или в нём нулевой байт. */
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

/* Слово до пробела или конца строки. Копируется в out, если помещается
 * (иначе out = ""). Возвращает длину слова во входных данных. */
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

/* Разобрать первую строку ("GET /index.html HTTP/1.1") и найти тело.
 * 0 — успех, 400 или 414 — HTTP-код ошибки. */
static int parse_request(const char *data, size_t size, request_t *req)
{
    const char *end = data + size;
    const char *p   = data;
    const char *query;
    size_t      n, i;

    memset(req, 0, sizeof *req);

    /* "GET" */
    n = take_word(p, end, req->method, sizeof req->method);
    if (n == 0 || n >= sizeof req->method || p + n >= end || p[n] != ' ')
        return 400;
    p += n + 1;

    /* "/index.html?x=1" */
    n = take_word(p, end, req->target, sizeof req->target);
    if (n >= sizeof req->target)
        return 414;
    if (n == 0 || req->target[0] != '/')
        return 400;
    p += n;

    /* "HTTP/1.1" (в HTTP/0.9 версии нет) */
    if (p < end && *p == ' ')
        take_word(p + 1, end, req->version, sizeof req->version);

    /* Путь — часть до '?', с раскодированными %XX: "/a%20b?x=1" -> "/a b". */
    query = strchr(req->target, '?');
    if (url_decode(req->target, query ? (size_t)(query - req->target) : strlen(req->target),
                   req->path, sizeof req->path, 0) != 0)
        return 400;

    /* Заголовки заканчиваются пустой строкой, дальше — тело. */
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

/* Значение заголовка name (без учёта регистра) -> out. 0 — найден. */
static int header_value(const char *data, size_t head_len, const char *name,
                        char *out, size_t out_size)
{
    const char *p        = data;
    const char *end      = data + head_len;
    size_t      name_len = strlen(name);

    while (p < end) {
        const char *eol      = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;

        if ((size_t)(line_end - p) > name_len && p[name_len] == ':' &&
            strncasecmp(p, name, name_len) == 0) {
            const char *v = p + name_len + 1;
            size_t      n;

            while (v < line_end && (*v == ' ' || *v == '\t'))
                v++;
            n = (size_t)(line_end - v);
            if (n > 0 && v[n - 1] == '\r')
                n--;
            if (n >= out_size)
                n = out_size - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        p = line_end + 1;
    }
    return -1;
}

/* ======================================================================== */
/* HTML-страницы                                                            */
/* ======================================================================== */

static const char PAGE_HEAD[] =
    "<!doctype html>\n"
    "<html lang=\"ru\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>cgi-c — пример CGI</title>\n"
    "<style>\n"
    "body{font-family:system-ui,sans-serif;max-width:60rem;margin:2rem auto;padding:0 1rem;"
    "line-height:1.5;color:#222}\n"
    "h2{margin-top:2rem;border-bottom:1px solid #ddd}\n"
    "code,pre,th{font-family:ui-monospace,Consolas,monospace}\n"
    "pre{background:#f5f5f5;padding:1rem;overflow:auto;white-space:pre-wrap;word-break:break-all}\n"
    "pre b{color:#c0392b;font-weight:normal}\n"
    "table{border-collapse:collapse}\n"
    "th,td{border:1px solid #ddd;padding:.3rem .6rem;text-align:left;vertical-align:top}\n"
    "th{background:#fafafa;white-space:nowrap}\n"
    "input,textarea{font:inherit;width:100%;box-sizing:border-box}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>cgi-c: пример CGI</h1>\n";

static const char PAGE_FOOT[] = "</body>\n</html>\n";

static const char PAGE_FORM[] =
    "<h2>Отправить POST-запрос</h2>\n"
    "<form method=\"post\" action=\"/\">\n"
    "<p><label>Имя<br><input name=\"name\" value=\"Мир\"></label></p>\n"
    "<p><label>Сообщение<br><textarea name=\"message\" rows=\"3\">Привет, CGI!</textarea>"
    "</label></p>\n"
    "<p><button type=\"submit\">Отправить</button></p>\n"
    "</form>\n";

static const char PAGE_LINKS[] =
    "<h2>Другие адреса</h2>\n"
    "<ul>\n"
    "<li><a href=\"/echo\">/echo</a> — вернуть сырые данные запроса обычным текстом</li>\n"
    "<li><code>/путь/к/файлу</code> — файл из папки root (скрытые файлы не отдаются)</li>\n"
    "</ul>\n";

/* Скрытые файлы и папки (".git", ".env"...) не показываем. */
static int visible(const struct dirent *entry)
{
    return entry->d_name[0] != '.';
}

/* Список файлов папки dir в виде ссылок; url — адрес этой папки. */
static void list_dir(text_t *t, const char *dir, const char *url)
{
    struct dirent **names;
    int             count, i;
    size_t          url_len = strlen(url);

    count = scandir(dir, &names, visible, alphasort);
    if (count < 0) {
        text_str(t, "<p>Не удалось прочитать папку.</p>\n");
        return;
    }
    if (count == 0)
        text_str(t, "<p>Папка пуста.</p>\n");

    text_str(t, "<ul>\n");
    for (i = 0; i < count; i++) {
        const char *name = names[i]->d_name;
        char        full[4096];
        struct stat st;
        int         is_dir = 0;

        if (snprintf(full, sizeof full, "%s/%s", dir, name) < (int)sizeof full &&
            stat(full, &st) == 0)
            is_dir = S_ISDIR(st.st_mode);

        text_str(t, "<li><a href=\"");
        text_url(t, url);
        if (url_len == 0 || url[url_len - 1] != '/')
            text_str(t, "/");
        text_url(t, name);
        text_str(t, is_dir ? "/\">" : "\">");
        text_html(t, name, strlen(name));
        text_str(t, is_dir ? "/</a></li>\n" : "</a></li>\n");
        free(names[i]);
    }
    text_str(t, "</ul>\n");
    free(names);
}

/* Поля HTML-формы: "name=%D0%9C%D0%B8%D1%80&message=hello+world". */
static void form_table(text_t *t, const char *body, size_t len)
{
    const char *p   = body;
    const char *end = body + len;

    text_str(t, "<table>\n");
    while (p < end) {
        const char *amp      = memchr(p, '&', (size_t)(end - p));
        const char *pair_end = amp ? amp : end;
        const char *eq       = memchr(p, '=', (size_t)(pair_end - p));
        const char *name_end = eq ? eq : pair_end;
        char        name[256];
        char        value[4096];

        if (p == pair_end) { /* пустая пара ("a=1&&b=2") — пропускаем */
            p = pair_end + 1;
            continue;
        }
        if (url_decode(p, (size_t)(name_end - p), name, sizeof name, 1) != 0)
            snprintf(name, sizeof name, "?");
        if (eq == NULL)
            value[0] = '\0';
        else if (url_decode(eq + 1, (size_t)(pair_end - eq - 1), value, sizeof value, 1) != 0)
            snprintf(value, sizeof value, "(не удалось раскодировать)");

        text_str(t, "<tr><th>");
        text_html(t, name, strlen(name));
        text_str(t, "</th><td>");
        text_html(t, value, strlen(value));
        text_str(t, "</td></tr>\n");
        p = pair_end + 1;
    }
    text_str(t, "</table>\n");
}

/* Кто на другом конце client. Без сервера client — это stdout, а не сокет. */
static void describe_client(int client, char *out, size_t size)
{
    struct sockaddr_storage addr;
    socklen_t               len = sizeof addr;
    char                    ip[INET6_ADDRSTRLEN] = "?";
    unsigned                port = 0;

    if (getpeername(client, (struct sockaddr *)&addr, &len) != 0) {
        snprintf(out, size, "не сокет: CGI запущена без сервера, ответ идёт в stdout");
        return;
    }
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip);
        port = ntohs(in->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&addr;
        inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof ip);
        port = ntohs(in6->sin6_port);
    }
    /* IPv4-клиент на IPv6-сокете (сервер с -a ::) выглядит как ::ffff:1.2.3.4 */
    snprintf(out, size, "сокет клиента %s, порт %u",
             strncmp(ip, "::ffff:", 7) == 0 && strchr(ip, '.') ? ip + 7 : ip, port);
}

/* Отправить собранную страницу. Если памяти не хватило — вернуть -1:
 * ответ не отправлен, и сервер сам ответит клиенту 500. */
static int send_page(int client, text_t *page, int head_only)
{
    int code = -1;

    text_str(page, PAGE_FOOT);
    if (!page->failed)
        code = send_response(client, 200, "OK", "text/html; charset=utf-8",
                             page->data, page->len, head_only);
    free(page->data);
    return code;
}

/* Главная страница: всё, что CGI-функция получила от сервера. */
static int info_page(int client, const char *root, const char *data, size_t size,
                     const request_t *req, int head_only)
{
    text_t page = { NULL, 0, 0, 0 };
    char   who[128];
    char   type[128] = "";

    describe_client(client, who, sizeof who);

    text_str(&page, PAGE_HEAD);
    text_str(&page,
             "<p>Эту страницу собрала функция <code>cgi_main()</code> из "
             "<code>example/main.c</code>. Сервер импортировал её из <code>example/cgi.so</code> "
             "(dlopen + dlsym) и вызвал через указатель на функцию в рабочем потоке.</p>\n");

    /* --- аргументы --- */
    text_str(&page, "<h2>Аргументы CGI-функции</h2>\n<table>\n");
    text_str(&page, "<tr><th>char *root</th><td><code>");
    text_html(&page, root, strlen(root));
    text_printf(&page, "</code><br><small>%zu из %d символов (буфер %d байт)</small></td></tr>\n",
                strlen(root), CGI_ROOT_SIZE - 1, CGI_ROOT_SIZE);
    text_str(&page, "<tr><th>char *data</th><td>«голые» данные от клиента — ниже</td></tr>\n");
    text_printf(&page, "<tr><th>size_t size</th><td>%zu байт</td></tr>\n", size);
    text_printf(&page, "<tr><th>int client</th><td>%d — ", client);
    text_html(&page, who, strlen(who));
    text_str(&page, "</td></tr>\n</table>\n");

    /* --- первая строка запроса --- */
    text_str(&page, "<h2>Первая строка запроса</h2>\n<table>\n<tr><th>метод</th><td>");
    text_html(&page, req->method, strlen(req->method));
    text_str(&page, "</td></tr>\n<tr><th>адрес</th><td>");
    text_html(&page, req->target, strlen(req->target));
    text_str(&page, "</td></tr>\n<tr><th>версия</th><td>");
    text_html(&page, req->version, strlen(req->version));
    text_str(&page, "</td></tr>\n</table>\n");

    /* --- тело и поля формы --- */
    if (req->body_len > 0) {
        text_printf(&page, "<h2>Тело запроса: %zu байт</h2>\n", req->body_len);
        header_value(data, req->head_len, "Content-Type", type, sizeof type);
        if (strncasecmp(type, "application/x-www-form-urlencoded", 33) == 0)
            form_table(&page, req->body, req->body_len);
        else
            text_str(&page, "<p>Тело видно в сырых данных ниже — после пустой строки.</p>\n");
    }

    /* --- сырые данные --- */
    text_str(&page, "<h2>Сырые данные от клиента (data)</h2>\n<pre>");
    text_raw(&page, data, size < SHOW_MAX ? size : SHOW_MAX);
    if (size > SHOW_MAX)
        text_str(&page, "\n… (показаны первые 64 КиБ)");
    text_str(&page, "</pre>\n");

    text_str(&page, PAGE_FORM);

    /* --- root в деле: список файлов корневой папки --- */
    text_str(&page, "<h2>Файлы в root</h2>\n");
    list_dir(&page, root, "/");

    text_str(&page, PAGE_LINKS);
    return send_page(client, &page, head_only);
}

/* Страница со списком файлов папки dir (её адрес — url). */
static int dir_page(int client, const char *dir, const char *url, int head_only)
{
    text_t page = { NULL, 0, 0, 0 };

    text_str(&page, PAGE_HEAD);
    text_str(&page, "<h2>Папка ");
    text_html(&page, url, strlen(url));
    text_str(&page, "</h2>\n");
    list_dir(&page, dir, url);
    text_str(&page, "<p><a href=\"/\">← на главную</a></p>\n");
    return send_page(client, &page, head_only);
}

/* ======================================================================== */
/* Файлы из root                                                            */
/* ======================================================================== */

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

/* Отправить открытый файл fd размером st->st_size. */
static int serve_file(int client, int fd, const char *path, const struct stat *st, int head_only)
{
    char        chunk[64 * 1024];
    const char *type = mime_type(path);
    ssize_t     n    = read(fd, chunk, sizeof chunk);

    if (n < 0)
        return send_error(client, 500, "Internal Server Error", head_only);

    /* Расширение неизвестно: есть нулевые байты — двоичный файл, иначе текст. */
    if (type == NULL)
        type = memchr(chunk, 0, (size_t)n) ? "application/octet-stream"
                                           : "text/plain; charset=utf-8";

    if (send_head(client, 200, "OK", type, (size_t)st->st_size) == 0 && !head_only) {
        while (n > 0 && send_all(client, chunk, (size_t)n) == 0)
            n = read(fd, chunk, sizeof chunk);
    }
    return 200;
}

/* Путь не выводит за пределы root: никаких "..", "." и скрытых файлов. */
static int path_is_safe(const char *path)
{
    return path[0] == '/' && strstr(path, "/.") == NULL;
}

/* GET /путь: файл или папка внутри root. */
static int serve_path(int client, const char *root, const char *path, int head_only)
{
    char        file[4096];
    char       *real;
    struct stat st;
    size_t      root_len = strlen(root);
    int         fd, code, inside;

    if (!path_is_safe(path))
        return send_error(client, 403, "Forbidden", head_only);

    /* Вот зачем CGI нужен root: путь к файлу = root + путь из запроса. */
    if (snprintf(file, sizeof file, "%s%s", root, path) >= (int)sizeof file)
        return send_error(client, 414, "URI Too Long", head_only);

    /* Символическая ссылка тоже не должна уводить за пределы root. */
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

    /* O_NONBLOCK — чтобы open() не повис на именованном канале (FIFO). */
    fd = open(real, O_RDONLY | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &st) != 0)
        code = send_error(client, 403, "Forbidden", head_only);
    else if (S_ISDIR(st.st_mode))
        code = dir_page(client, real, path, head_only);
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

/*
 * Сервер вызывает её для каждого запроса (в одном из рабочих потоков):
 *
 *   root   — 256 символов: абсолютный путь к корневой папке (ключ -r);
 *   data   — «голые» данные от клиента: запрос в том виде, как пришёл;
 *   size   — сколько байт в data;
 *   client — сокет: сюда пишем полный HTTP-ответ.
 *
 * Возвращаем HTTP-код ответа (он попадёт в журнал сервера) или -1,
 * если ничего не отправили, — тогда сервер сам ответит 500.
 *
 * Функция не использует глобальных переменных, поэтому её можно безопасно
 * вызывать одновременно из нескольких потоков.
 */
int cgi_main(char *root, char *data, size_t size, int client)
{
    request_t req;
    int       head_only, error;

    /* 1. Из «голых» данных достаём первую строку: метод, адрес, версию. */
    error = parse_request(data, size, &req);
    if (error == 414)
        return send_error(client, 414, "URI Too Long", 0);
    if (error != 0)
        return send_error(client, 400, "Bad Request", 0);
    head_only = strcmp(req.method, "HEAD") == 0;

    /* 2. /echo — вернуть клиенту его же сырые данные. */
    if (strcmp(req.path, "/echo") == 0)
        return send_response(client, 200, "OK", "text/plain; charset=utf-8",
                             data, size, head_only);

    /* 3. Главная страница — для GET и POST. */
    if (strcmp(req.path, "/") == 0 &&
        (strcmp(req.method, "GET") == 0 || strcmp(req.method, "POST") == 0 || head_only))
        return info_page(client, root, data, size, &req, head_only);

    /* 4. Остальные адреса — файлы из root, только для чтения. */
    if (strcmp(req.method, "GET") != 0 && !head_only)
        return send_error(client, 405, "Method Not Allowed", 0);
    return serve_path(client, root, req.path, head_only);
}

/* ======================================================================== */
/* main() — запуск той же CGI без сервера                                   */
/* ======================================================================== */

/*
 * ./example/cgi [ROOT] < запрос
 *
 * Делает то же, что сервер для одного запроса: готовит буфер root на
 * 256 символов, читает «голые» данные (здесь — из stdin) и вызывает
 * CGI-функцию через указатель. Ответ пишется в stdout (дескриптор 1).
 */
int main(int argc, char **argv)
{
    char           root[CGI_ROOT_SIZE]; /* тот самый буфер на 256 символов */
    cgi_function_t cgi = cgi_main;      /* указатель на функцию — как в сервере */
    char          *full, *data = NULL;
    size_t         size = 0, cap = 0, len;
    int            status;

    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr,
                "Использование: %s [ROOT] < запрос\n"
                "Пример: printf 'GET / HTTP/1.1\\r\\n\\r\\n' | %s .\n",
                argv[0], argv[0]);
        return 2;
    }

    /* root: абсолютный путь, не длиннее 255 символов. */
    full = realpath(argc == 2 ? argv[1] : ".", NULL);
    if (full == NULL) {
        perror(argc == 2 ? argv[1] : ".");
        return 1;
    }
    len = strlen(full);
    if (len >= CGI_ROOT_SIZE) {
        fprintf(stderr, "путь к корню длиннее %d символов\n", CGI_ROOT_SIZE - 1);
        free(full);
        return 1;
    }
    memset(root, 0, sizeof root);
    memcpy(root, full, len);
    free(full);

    /* «Голые» данные — всё, что пришло в stdin, байт в байт. */
    for (;;) {
        ssize_t n;

        if (size == cap) {
            char *bigger;

            cap    = cap ? cap * 2 : 4096;
            bigger = realloc(data, cap + 1); /* +1 — для '\0', как в сервере */
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

    fprintf(stderr, "\n[cgi_main вернула %d]\n", status);
    return status < 0 ? 1 : 0;
}
