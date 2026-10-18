/*
 * fcgi.c — библиотека FastCGI-сервера: fcgi_run(), приём
 * соединений, протокол FastCGI, вызов приложения.
 *
 * Это НЕ программа: функции main() здесь нет. Код компонуется с
 * main.c пользователя на этапе сборки (см. Makefile:
 * libfcgi-server.a), а функция-обработчик библиотеке передаётся
 * обычным указателем — аргументом fcgi_run().
 *
 * HTTP здесь нет совсем: протокол FastCGI говорят мы, а HTTP
 * разбирает веб-сервер перед нами (nginx, Apache, lighttpd,
 * Caddy). Путь одного запроса:
 *
 *   fcgi_run() -> accept() -> thread_pool_push()      главный поток
 *                                       |
 *                                       v  очередь пула
 *   fcgi_connection()                             рабочий поток
 *     1. BEGIN_REQUEST   — начало запроса (роль, keep-alive)
 *     2. PARAMS          — копим переменные окружения
 *     3. STDIN           — копим тело запроса
 *     4. app(...)        — вызываем функцию пользователя;
 *                             ответ уходит записями STDOUT
 *     5. END_REQUEST     — конец запроса; без FCGI_KEEP_CONN
 *                             соединение закрывается
 */
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 + XSI: realpath(), SA_RESETHAND */

#include "fcgi.h"
#include "thread.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/* send() без сигнала SIGPIPE там, где это поддерживается (Linux, *BSD). */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* ======================================================================== */
/* Константы протокола FastCGI (см. fastcgi-protocol.dev)      */
/* ======================================================================== */

enum {
    FCGI_VERSION            = 1,   /* версия протокола */
    FCGI_HEADER_SIZE        = 8,   /* заголовок записи */
    FCGI_MAX_CONTENT        = 65535, /* максимум данных в записи */

    FCGI_BEGIN_REQUEST      = 1,
    FCGI_ABORT_REQUEST      = 2,
    FCGI_END_REQUEST        = 3,
    FCGI_PARAMS             = 4,
    FCGI_STDIN              = 5,
    FCGI_STDOUT             = 6,
    FCGI_STDERR             = 7,
    FCGI_DATA               = 8,
    FCGI_GET_VALUES         = 9,
    FCGI_GET_VALUES_RESULT  = 10,
    FCGI_UNKNOWN_TYPE       = 11,

    FCGI_RESPONDER  = 1, /* единственная поддерживаемая роль */
    FCGI_AUTHORIZER = 2,
    FCGI_FILTER     = 3,

    FCGI_REQUEST_COMPLETE = 0, /* protocolStatus в END_REQUEST */
    FCGI_CANT_MPX_CONN    = 1,
    FCGI_OVERLOADED       = 2,
    FCGI_UNKNOWN_ROLE     = 3,

    FCGI_KEEP_CONN = 1 /* флаг в BEGIN_REQUEST: не закрывать
                            соединение после запроса */
};

/* Настройки и общее состояние сервера. Указатель на эту структуру
 * получает каждый рабочий поток (аргумент arg обработчика
 * fcgi_connection). */
typedef struct {
    char            root[FCGI_ROOT_SIZE]; /* корневая папка —
                                                   передаётся в
                                                   приложение */
    fcgi_function_t app; /* указатель на функцию пользователя (ее
                                  адрес передала программа в
                                  fcgi_run()) */
    size_t          max_request; /* предельный размер тела запроса,
                                        байт */
    int             timeout_ms;  /* таймаут чтения запроса и записи
                                        ответа */
    long            threads;     /* число рабочих потоков — для
                                        GET_VALUES */
    int             quiet;       /* 1 — не печатать журнал запросов */
} server_t;

/* Флаг остановки: его выставляет обработчик SIGINT/SIGTERM. */
static volatile sig_atomic_t stop_requested = 0;

/* Состояние одного соединения. Мультиплексирование не
 * поддерживается, поэтому активным может быть только один
 * запрос. */
typedef struct {
    int    fd;
    int    request_id;  /* 0 — запроса нет */
    int    keep_conn;   /* флаг FCGI_KEEP_CONN из BEGIN_REQUEST */
    int    params_done; /* пришла пустая запись PARAMS */
    int    stdin_done;  /* пришла пустая запись STDIN */
    char  *params;      /* копилка для PARAMS */
    size_t params_len, params_cap;
    char  *body;        /* копилка для STDIN (тело запроса) */
    size_t body_len, body_cap;
} conn_t;

/* ======================================================================== */
/* Вспомогательные функции                                              */
/* ======================================================================== */

/* Монотонное время в микросекундах (не зависит от перевода часов). */
static long long now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Отправить все len байт: send() может отправить только часть. */
static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* То же через write(): для сырого режима, где fd — не сокет,
 * а файл или pipe (send() там не работает). */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Прочитать ровно len байт: recv() может вернуть часть. */
static int read_full(int fd, char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) /* собеседник закрыл соединение */
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Дописать в растущий буфер. Возвращает 0 или -1 (нет памяти). */
static int buf_grow(char **buf, size_t *len, size_t *cap,
                      const char *data, size_t add)
{
    size_t need = *len + add;
    size_t ncap;
    char  *p;

    if (add == 0)
        return 0;
    if (need <= *cap)
        goto copy;

    ncap = *cap ? *cap : 4096;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2)
            return -1;
        ncap *= 2;
    }
    p = realloc(*buf, ncap);
    if (p == NULL)
        return -1;
    *buf = p;
    *cap = ncap;

copy:
    memcpy(*buf + *len, data, add);
    *len = need;
    return 0;
}

/* ======================================================================== */
/* Записи FastCGI                                                        */
/* ======================================================================== */

/*
 * Заголовок записи (8 байт):
 *   byte 0    версия протокола (1)
 *   byte 1    тип записи
 *   byte 2-3  номер запроса (big-endian)
 *   byte 4-5  длина данных (big-endian)
 *   byte 6    длина padding
 *   byte 7    зарезервирован
 */
static int rec_send(int fd, int type, int request_id,
                      const char *content, size_t len)
{
    unsigned char head[FCGI_HEADER_SIZE];

    if (len > FCGI_MAX_CONTENT)
        return -1;
    head[0] = FCGI_VERSION;
    head[1] = (unsigned char)type;
    head[2] = (unsigned char)(request_id >> 8);
    head[3] = (unsigned char)request_id;
    head[4] = (unsigned char)(len >> 8);
    head[5] = (unsigned char)len;
    head[6] = 0; /* padding — не нужен */
    head[7] = 0;
    return send_all(fd, (const char *)head, sizeof head) == 0 &&
                   (len == 0 || send_all(fd, content, len) == 0)
               ? 0
               : -1;
}

/* Длинный ответ режется на записи по FCGI_MAX_CONTENT байт. */
static int send_stream(int fd, int type, int request_id,
                         const char *buf, size_t len)
{
    while (len > 0) {
        size_t chunk = len > FCGI_MAX_CONTENT ? FCGI_MAX_CONTENT : len;

        if (rec_send(fd, type, request_id, buf, chunk) != 0)
            return -1;
        buf += chunk;
        len -= chunk;
    }
    return 0;
}

/* Число в 4 байта big-endian. */
static void put32(unsigned char *p, unsigned long value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

/* Конец запроса: appStatus — код приложения, protocolStatus —
 * FCGI_REQUEST_COMPLETE при успехе. */
static int send_end(int fd, int request_id, int app_status,
                      int protocol_status)
{
    unsigned char content[8];

    put32(content, (unsigned long)app_status);
    content[4] = (unsigned char)protocol_status;
    content[5] = content[6] = content[7] = 0;
    return rec_send(fd, FCGI_END_REQUEST, request_id,
                      (const char *)content, sizeof content);
}

/* Записать пару «имя/значение» в формате PARAMS. Имя задано
 * длиной (оно указывает внутрь чужого буфера), значение —
 * строка. Возвращает новую длину или старую, если место
 * не кончилось. */
static size_t put_pair(char *buf, size_t cap, size_t at,
                         const char *name, size_t nlen,
                         const char *value)
{
    size_t vlen = strlen(value);
    size_t need = (nlen < 128 ? 1 : 4) + (vlen < 128 ? 1 : 4) +
                  nlen + vlen;

    if (at + need > cap)
        return at;
    if (nlen < 128)
        buf[at++] = (char)nlen;
    else {
        put32((unsigned char *)(buf + at), nlen | 0x80000000UL);
        at += 4;
    }
    if (vlen < 128)
        buf[at++] = (char)vlen;
    else {
        put32((unsigned char *)(buf + at), vlen | 0x80000000UL);
        at += 4;
    }
    memcpy(buf + at, name, nlen);
    at += nlen;
    memcpy(buf + at, value, vlen);
    return at + vlen;
}

/* Декодировать длину имени/значения из PARAMS/GET_VALUES. */
static size_t read_length(const char *buf, size_t len, size_t *pos)
{
    size_t value;

    if (*pos >= len)
        return SIZE_MAX;
    if ((unsigned char)buf[*pos] < 128)
        return (size_t)(unsigned char)buf[(*pos)++];
    if (*pos + 4 > len)
        return SIZE_MAX;
    value = ((size_t)(unsigned char)buf[*pos] & 0x7f) << 24;
    value |= (size_t)(unsigned char)buf[*pos + 1] << 16;
    value |= (size_t)(unsigned char)buf[*pos + 2] << 8;
    value |= (size_t)(unsigned char)buf[*pos + 3];
    *pos += 4;
    return value;
}

/* Ответ на GET_VALUES: только известные нам переменные, в том же
 * порядке, в каком их спросили. */
static void answer_get_values(int fd, const char *content, size_t len,
                                  long threads)
{
    char   result[512];
    char   num[32];
    size_t rlen = 0, pos = 0;

    snprintf(num, sizeof num, "%ld", threads);

    while (pos < len) {
        size_t      nlen, vlen;
        const char *name;
        const char *value = NULL;

        /* порядок в записи: длина имени, длина значения,
         * имя, значение */
        nlen = read_length(content, len, &pos);
        if (nlen == SIZE_MAX)
            break;
        vlen = read_length(content, len, &pos);
        if (vlen == SIZE_MAX || pos + nlen + vlen > len)
            break;
        name = content + pos;
        pos += nlen + vlen; /* само значение нам не нужно */

        if (nlen == 14 && memcmp(name, "FCGI_MAX_CONNS", 14) == 0)
            value = num;
        else if (nlen == 13 && memcmp(name, "FCGI_MAX_REQS", 13) == 0)
            value = num;
        else if (nlen == 15 && memcmp(name, "FCGI_MPXS_CONNS", 15) == 0)
            value = "0"; /* мультиплексирование не поддерживается */

        if (value != NULL)
            rlen = put_pair(result, sizeof result, rlen, name,
                                nlen, value);
    }
    rec_send(fd, FCGI_GET_VALUES_RESULT, 0, result, rlen);
}

/* ======================================================================== */
/* Окружение запроса (fcgi_env_t)                                    */
/* ======================================================================== */

struct fcgi_env {
    struct {
        char *name;
        char *value;
    } *items;
    size_t count;
    size_t cap;
};

fcgi_env_t *fcgi_env_create(void)
{
    return calloc(1, sizeof(fcgi_env_t));
}

/* Внутренняя версия: имя и значение заданы длиной, а не '\0'. */
static int fcgi_env_add_n(fcgi_env_t *env, const char *name,
                              size_t name_len, const char *value,
                              size_t value_len)
{
    char *n, *v;
    size_t i;

    if (env == NULL || name == NULL || value == NULL)
        return -1;

    /* Двоичные данные в FastCGI-переменных не встречаются почти
     * никогда (в CGI-окружении их и быть не может), поэтому
     * значения хранятся как строки: '\0' просто обрежет значение. */
    for (i = 0; i < name_len && name[i] != '\0'; i++)
        ;
    n = malloc(i + 1);
    if (n == NULL)
        return -1;
    memcpy(n, name, i);
    n[i] = '\0';

    for (i = 0; i < value_len && value[i] != '\0'; i++)
        ;
    v = malloc(i + 1);
    if (v == NULL) {
        free(n);
        return -1;
    }
    memcpy(v, value, i);
    v[i] = '\0';

    for (i = 0; i < env->count; i++) {
        if (strcmp(env->items[i].name, n) == 0) { /* заменить */
            free(env->items[i].value);
            env->items[i].value = v;
            free(n);
            return 0;
        }
    }

    if (env->count == env->cap) {
        size_t ncap = env->cap ? env->cap * 2 : 16;
        void  *p    = realloc(env->items, ncap * sizeof *env->items);

        if (p == NULL) {
            free(n);
            free(v);
            return -1;
        }
        env->items = p;
        env->cap   = ncap;
    }
    env->items[env->count].name  = n;
    env->items[env->count].value = v;
    env->count++;
    return 0;
}

int fcgi_env_add(fcgi_env_t *env, const char *name, const char *value)
{
    return fcgi_env_add_n(env, name, strlen(name), value, strlen(value));
}

const char *fcgi_env_get(const fcgi_env_t *env, const char *name)
{
    size_t i;

    if (env == NULL || name == NULL)
        return NULL;
    for (i = 0; i < env->count; i++)
        if (strcmp(env->items[i].name, name) == 0)
            return env->items[i].value;
    return NULL;
}

void fcgi_env_free(fcgi_env_t *env)
{
    size_t i;

    if (env == NULL)
        return;
    for (i = 0; i < env->count; i++) {
        free(env->items[i].name);
        free(env->items[i].value);
    }
    free(env->items);
    free(env);
}

/* Собрать карту переменных из потока PARAMS. */
static int parse_params(const char *buf, size_t len, fcgi_env_t *env)
{
    size_t pos = 0;

    while (pos < len) {
        size_t nlen = read_length(buf, len, &pos);
        size_t vlen;

        if (nlen == SIZE_MAX)
            return -1;
        vlen = read_length(buf, len, &pos);
        if (vlen == SIZE_MAX || pos + nlen + vlen > len)
            return -1;
        if (fcgi_env_add_n(env, buf + pos, nlen, buf + pos + nlen,
                               vlen) != 0)
            return -1;
        pos += nlen + vlen;
    }
    return 0;
}

/* ======================================================================== */
/* Поток ответа (fcgi_stream_t)                                      */
/* ======================================================================== */

int fcgi_write(fcgi_stream_t *out, const char *buf, size_t len)
{
    int rc;

    if (out->framed)
        rc = send_stream(out->fd, FCGI_STDOUT, out->request_id,
                             buf, len);
    else
        rc = write_all(out->fd, buf, len); /* отладка: «голые» байты */
    if (rc != 0)
        return -1;
    out->written += len;
    return 0;
}

int fcgi_write_err(fcgi_stream_t *out, const char *buf, size_t len)
{
    if (out->framed)
        return send_stream(out->fd, FCGI_STDERR, out->request_id,
                               buf, len);
    /* в режиме отладки ошибки и так идут в терминал */
    return write_all(STDERR_FILENO, buf, len);
}

int fcgi_printf(fcgi_stream_t *out, const char *fmt, ...)
{
    char    buf[4096];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n > sizeof buf - 1) /* слишком длинная — обрежем */
        n = (int)sizeof buf - 1;
    return fcgi_write(out, buf, (size_t)n);
}

size_t fcgi_stream_bytes(const fcgi_stream_t *out)
{
    return out->written;
}

/* ======================================================================== */
/* Обработка запроса                                                     */
/* ======================================================================== */

/* Ответ, который шлём вместо приложения, если оно не смогло
 * ответить само: веб-сервер превратит «Status:» в код
 * HTTP-ответа. */
static void send_fallback(fcgi_stream_t *out, int code, const char *reason)
{
    char body[128];

    snprintf(body, sizeof body, "%d %s\n", code, reason);
    fcgi_printf(out, "Status: %d %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                code, reason, strlen(body), body);
}

/* IP-адрес собеседника текстом — для журнала. */
static void peer_address(int fd, char *out, size_t size)
{
    struct sockaddr_storage addr;
    socklen_t               len = sizeof addr;

    snprintf(out, size, "-");
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0)
        return;

    if (addr.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr,
                  out, (socklen_t)size);
    else if (addr.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr,
                  out, (socklen_t)size);

    /* IPv4-клиент на IPv6-сокете выглядит как "::ffff:1.2.3.4" —
     * убираем префикс. */
    if (strncmp(out, "::ffff:", 7) == 0 && strchr(out + 7, '.') != NULL)
        memmove(out, out + 7, strlen(out + 7) + 1);
}

/* Строка журнала: [время] адрес "МЕТОД URI ПРОТОКОЛ" код длительность. */
static void log_request(const char *addr, const fcgi_env_t *env,
                          int status, long long elapsed_us)
{
    char        line[512];
    char        when[32];
    char        code[16];
    const char *method, *uri, *version;
    int         n;
    time_t      now = time(NULL);
    struct tm   tm;

    method  = fcgi_env_get(env, "REQUEST_METHOD");
    uri     = fcgi_env_get(env, "REQUEST_URI");
    version = fcgi_env_get(env, "SERVER_PROTOCOL");

    n = snprintf(line, sizeof line, "%s %s %s",
                 method != NULL ? method : "-",
                 uri != NULL ? uri : "-",
                 version != NULL ? version : "-");
    if (n < 0 || (size_t)n >= sizeof line)
        strcpy(line, "-");

    if (status > 0)
        snprintf(code, sizeof code, "%d", status);
    else
        snprintf(code, sizeof code, "-");

    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    /* Один вызов printf на строку: stdio сам блокирует поток
     * вывода, поэтому строки из разных потоков не перемешаются. */
    printf("[%s] %s \"%s\" %s %.1f ms\n", when, addr, line, code,
           (double)elapsed_us / 1000.0);
}

/*
 * Собрать запрос (env + тело) и вызвать приложение, затем отправить
 * END_REQUEST. Ответ приложения уходит в STDOUT сразу, порциями —
 * он может быть сколь угодно большим.
 */
static void process_request(server_t *server, conn_t *c)
{
    fcgi_env_t    *env = fcgi_env_create();
    fcgi_stream_t  out;
    char           root[FCGI_ROOT_SIZE];
    long long      start = now_us();
    int            status = 500;

    out.fd         = c->fd;
    out.request_id = c->request_id;
    out.framed     = 1;
    out.written    = 0;

    /* Копируем все 256 байт root: приложение может менять свой
     * буфер, никак не влияя на другие потоки. */
    memcpy(root, server->root, FCGI_ROOT_SIZE);

    if (env == NULL || parse_params(c->params, c->params_len, env) != 0) {
        send_fallback(&out, 500, "Internal Server Error");
        status = 500;
    } else {
        /* Вызов функции пользователя через указатель, переданный в
         * fcgi_run(). Адрес известен уже на этапе компоновки
         * программы — никакой динамической загрузки во время
         * работы. */
        status = server->app(root, env, c->body, c->body_len, &out);
        if (status < 0) { /* приложение: ответ не отправлен */
            if (out.written == 0)
                send_fallback(&out, 500, "Internal Server Error");
            status = 500;
        }
    }

    /* Конец потока STDOUT — пустая запись, затем END_REQUEST. */
    rec_send(c->fd, FCGI_STDOUT, c->request_id, NULL, 0);
    send_end(c->fd, c->request_id, status > 0 ? status : 0,
               FCGI_REQUEST_COMPLETE);

    if (!server->quiet) {
        char addr[INET6_ADDRSTRLEN];

        peer_address(c->fd, addr, sizeof addr);
        log_request(addr, env, status, now_us() - start);
    }
    fcgi_env_free(env);
}

/* Обнулить состояние запроса, оставив fd соединения. */
static void conn_reset(conn_t *c)
{
    int fd = c->fd;

    free(c->params);
    free(c->body);
    memset(c, 0, sizeof *c);
    c->fd      = fd;
    c->params  = NULL;
    c->body    = NULL;
}

static void conn_free(conn_t *c)
{
    free(c->params);
    free(c->body);
}

/* ======================================================================== */
/* Соединение (выполняется в рабочем потоке)                         */
/* ======================================================================== */

/*
 * Ждать данных не позже deadline. Возвращает 1 — есть что
 * читать, 0 — таймаут или ошибка (соединение надо закрыть).
 */
static int wait_readable(int fd, long long deadline_us)
{
    for (;;) {
        struct pollfd pfd;
        long long     left = (deadline_us - now_us()) / 1000;
        int           ready;

        if (left <= 0)
            return 0;
        pfd.fd      = fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, (int)left);
        if (ready < 0 && errno == EINTR)
            continue;
        return ready > 0;
    }
}

/*
 * Обработчик соединения. Пул потоков вызывает его через указатель
 * thread_task_t (см. thread.h); arg — указатель на server_t.
 */
static void fcgi_connection(int client, void *arg)
{
    server_t *server = arg;
    conn_t    c;
    long long deadline;
    int       stop = 0; /* закрыть соединение после записи */

    memset(&c, 0, sizeof c);
    c.fd     = client;
    deadline = now_us() + (long long)server->timeout_ms * 1000;

    while (!stop) {
        unsigned char head[FCGI_HEADER_SIZE];
        char         *content = NULL;
        int           type, request_id, content_len, padding;
        int           overflow = 0; /* код отказа: 413 или 431 */

        /* 1. Заголовок записи. Медленный (или зависший) веб-сервер
         *    не держит поток вечно: общий срок на запрос. */
        if (!wait_readable(client, deadline))
            break;
        if (read_full(client, (char *)head, sizeof head) != 0)
            break;
        if (head[0] != FCGI_VERSION)
            break; /* не FastCGI — закрываем */

        type        = head[1];
        request_id  = ((int)head[2] << 8) | head[3];
        content_len = ((int)head[4] << 8) | head[5];
        padding     = head[6];

        /* 2. Данные записи (padding отбрасываем). */
        if (content_len > 0) {
            content = malloc((size_t)content_len);
            if (content == NULL)
                break;
            if (read_full(client, content, (size_t)content_len) != 0) {
                free(content);
                break;
            }
        }
        if (padding > 0) {
            char trash[255];

            if (read_full(client, trash, (size_t)padding) != 0) {
                free(content);
                break;
            }
        }

        /* 3. Что делать с записью. */
        switch (type) {
        case FCGI_BEGIN_REQUEST: {
            int role, flags, keep;

            if (content_len != 8 || request_id == 0) {
                stop = 1; /* битая запись — нечего отвечать */
                break;
            }
            role  = ((int)content[0] << 8) | content[1];
            flags = content[2];

            if (c.request_id != 0 && request_id != c.request_id) {
                /* Мультиплексирование не поддерживаем: сообщаем и
                 * закрываем соединение. */
                send_end(client, request_id, 0, FCGI_CANT_MPX_CONN);
                stop = 1;
                break;
            }

            keep             = (flags & FCGI_KEEP_CONN) != 0;
            c.request_id     = request_id;
            c.keep_conn      = keep;
            deadline         = now_us() + (long long)server->timeout_ms * 1000;

            if (role != FCGI_RESPONDER) {
                /* Роль не поддерживаем — конец запроса; соединение
                 * живёт, если веб-сервер просил keep-alive. */
                send_end(client, request_id, 0, FCGI_UNKNOWN_ROLE);
                conn_reset(&c);
                if (!keep)
                    stop = 1;
            }
            break;
        }

        case FCGI_ABORT_REQUEST:
            /* Веб-сервер передумал: выбрасываем накопленное. */
            conn_reset(&c);
            break;

        case FCGI_PARAMS:
            if (request_id != c.request_id || c.request_id == 0) {
                stop = 1;
                break;
            }
            if (content_len == 0) {
                c.params_done = 1; /* конец потока PARAMS */
                break;
            }
            if (buf_grow(&c.params, &c.params_len, &c.params_cap,
                             content, (size_t)content_len) != 0) {
                stop = 1;
                break;
            }
            if (c.params_len > server->max_request) {
                overflow = 431; /* переменные слишком большие */
                stop = 1;
            }
            break;

        case FCGI_STDIN:
            if (request_id != c.request_id || c.request_id == 0) {
                stop = 1;
                break;
            }
            if (content_len == 0) {
                c.stdin_done = 1; /* конец потока STDIN */
                break;
            }
            if (buf_grow(&c.body, &c.body_len, &c.body_cap,
                             content, (size_t)content_len) != 0) {
                stop = 1;
                break;
            }
            if (c.body_len > server->max_request) {
                overflow = 413; /* тело слишком большое */
                stop = 1;
            }
            break;

        case FCGI_GET_VALUES:
            answer_get_values(client, content != NULL ? content : "",
                                (size_t)content_len, server->threads);
            break;

        case FCGI_DATA:
            break; /* роль FILTER не поддерживается — молча ждём */

        case FCGI_END_REQUEST:
        case FCGI_STDOUT:
        case FCGI_STDERR:
            stop = 1; /* от веб-сервера такие записи не приходят */
            break;

        default: {
            /* Неизвестный тип — отвечаем по протоколу и живём
             * дальше. */
            unsigned char unknown[1];

            unknown[0] = (unsigned char)type;
            rec_send(client, FCGI_UNKNOWN_TYPE, 0,
                       (const char *)unknown, 1);
            break;
        }
        }
        free(content);

        /* 4. Переполнение: отвечаем сами и закрываем соединение. */
        if (overflow != 0) {
            fcgi_stream_t out;

            out.fd         = client;
            out.request_id = request_id;
            out.framed     = 1;
            out.written    = 0;
            send_fallback(&out, overflow,
                            overflow == 413
                                ? "Content Too Large"
                                : "Request Header Fields Too Large");
            rec_send(client, FCGI_STDOUT, request_id, NULL, 0);
            send_end(client, request_id, 0, FCGI_REQUEST_COMPLETE);
            if (!server->quiet)
                log_request("-", NULL, overflow, 0);
            break;
        }

        /* 5. Запрос целиком получен — обрабатываем. */
        if (c.request_id != 0 && c.params_done && c.stdin_done) {
            int keep;

            process_request(server, &c);
            keep = c.keep_conn;
            conn_reset(&c);
            if (!keep)
                stop = 1;
        }
    }

    conn_free(&c);
    socket_close(client, 0);
}

/* ======================================================================== */
/* Сокеты                                                                */
/* ======================================================================== */

int socket_listen(const char *address, const char *port)
{
    struct addrinfo  hints;
    struct addrinfo *list, *ai;
    int              fd = -1, err, saved = 0, one = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;   /* IPv4 или IPv6 — смотря какой адрес */
    hints.ai_socktype = SOCK_STREAM; /* TCP */
    hints.ai_flags    = AI_PASSIVE | AI_NUMERICSERV;

    err = getaddrinfo(address, port, &hints, &list);
    if (err != 0) {
        fprintf(stderr, "fcgi: неверный адрес %s:%s: %s\n", address,
                port, gai_strerror(err));
        return -1;
    }

    for (ai = list; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved = errno;
            continue;
        }

        /* Можно сразу перезапустить сервер на том же порту. */
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (ai->ai_family == AF_INET6) {
            int zero = 0; /* "::" принимает и IPv4-клиентов */
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        }

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, SOMAXCONN) == 0)
            break;

        saved = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);

    if (fd < 0) {
        fprintf(stderr, "fcgi: не удалось открыть %s:%s: %s\n", address,
                port, strerror(saved));
        return -1;
    }

    /* Неблокирующий: если клиент отвалится между poll() и
     * accept(), accept() не зависнет, а вернёт EAGAIN. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int socket_listen_unix(const char *path)
{
    struct sockaddr_un addr;
    int                fd;

    if (strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "fcgi: путь к сокету длиннее %zu символов: %s\n",
                sizeof addr.sun_path - 1, path);
        return -1;
    }

    /* Сталкиваться с файлом старого сервера не будем — но только
     * если это действительно сокет, обычный файл не трогаем. */
    {
        struct stat st;

        if (stat(path, &st) == 0) {
            if (!S_ISSOCK(st.st_mode)) {
                fprintf(stderr,
                        "fcgi: %s существует и это не сокет\n", path);
                return -1;
            }
            unlink(path);
        }
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "fcgi: не удалось создать сокет: %s\n",
                strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(fd, SOMAXCONN) != 0) {
        fprintf(stderr, "fcgi: не удалось открыть %s: %s\n", path,
                strerror(errno));
        close(fd);
        return -1;
    }

    /* Чтобы веб-сервер, работающий от другого пользователя, мог
     * подключиться. */
    chmod(path, 0666);

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* Настроить принятый сокет клиента. */
static void setup_client(int client, int timeout_ms)
{
    struct timeval tv;
    int            one   = 1;
    int            flags = fcntl(client, F_GETFL);

    /* На BSD/macOS принятый сокет наследует O_NONBLOCK слушающего —
     * снимаем: приложение работает с обычным блокирующим сокетом. */
    if (flags >= 0)
        fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
    fcntl(client, F_SETFD, FD_CLOEXEC);

    /* Отправлять части ответа сразу, без задержки алгоритма Нейгла.
     * На Unix-сокете setsockopt просто не сработает — это нормально. */
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    /* Таймауты на чтение и запись: зависший веб-сервер не займёт
     * поток навсегда. */
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

void socket_close(int fd, int linger)
{
    char      trash[4096];
    size_t    drained  = 0;
    long long deadline = now_us() + (linger ? 2000000 : 0); /* до 2 с */

    /* FIN: «ответ закончен». Веб-сервер, читающий записи до конца
     * потока, сразу понимает, что получил всё. */
    shutdown(fd, SHUT_WR);

    /*
     * Если закрыть сокет, в котором остались непрочитанные входящие
     * данные, ядро отправит RST, и веб-сервер может потерять уже
     * отправленный ему ответ. Поэтому выбрасываем то, что успело
     * прийти, а при linger ещё и немного ждём остаток.
     */
    for (;;) {
        struct pollfd pfd;
        long long     left = (deadline - now_us()) / 1000;
        ssize_t       n;
        int           ready;

        pfd.fd      = fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, left > 0 ? (int)left : 0);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;

        n = recv(fd, trash, sizeof trash, 0);
        if (n <= 0) /* веб-сервер закрыл соединение или ошибка */
            break;
        drained += (size_t)n;
        if (drained >= 1024 * 1024) /* бесконечно не читаем */
            break;
    }
    close(fd);
}

/* ======================================================================== */
/* fcgi_run()                                                           */
/* ======================================================================== */

static void on_signal(int sig)
{
    static const char msg[] =
        "\nfcgi: остановка... (повторный Ctrl+C — выйти немедленно)\n";
    ssize_t ignored;

    (void)sig;
    stop_requested = 1;
    ignored = write(STDERR_FILENO, msg, sizeof msg - 1); /* printf() нельзя */
    (void)ignored;
}

static void setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    /* Ctrl+C и kill: мягкая остановка. SA_RESETHAND — второй такой
     * же сигнал сработает как обычно и сразу завершит процесс. */
    sa.sa_handler = on_signal;
    sa.sa_flags   = SA_RESETHAND;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Запись в сокет, который веб-сервер уже закрыл, по умолчанию
     * убивает весь процесс сигналом SIGPIPE. Игнорируем его:
     * send() просто вернёт ошибку EPIPE. */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags   = 0;
    sigaction(SIGPIPE, &sa, NULL);
}

/* Абсолютный путь к корню -> буфер root из FCGI_ROOT_SIZE (256)
 * символов. */
static int set_root(char *root, const char *path)
{
    struct stat st;
    char       *full = realpath(path, NULL);
    size_t      len;

    if (full == NULL) {
        fprintf(stderr, "fcgi: корневая папка %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    len = strlen(full);

    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "fcgi: %s — не папка\n", full);
        free(full);
        return -1;
    }
    if (len >= FCGI_ROOT_SIZE) {
        fprintf(stderr, "fcgi: путь к корню длиннее %d символов: %s\n",
                FCGI_ROOT_SIZE - 1, full);
        free(full);
        return -1;
    }

    memset(root, 0, FCGI_ROOT_SIZE);
    memcpy(root, full, len);
    free(full);
    return 0;
}

/* Целое число из text в диапазоне [min, max]; 0 — успех. */
static int parse_number(const char *text, long min, long max, long *out)
{
    char *end;
    long  value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min ||
        value > max)
        return -1;
    *out = value;
    return 0;
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Использование: %s [ключи]\n"
            "\n"
            "  -p PORT   TCP-порт (по умолчанию %s)\n"
            "  -a ADDR   адрес для прослушивания (по умолчанию %s;\n"
            "               IPv6 — ::)\n"
            "  -s PATH   Unix-сокет вместо TCP (если задан, -a и -p\n"
            "               игнорируются)\n"
            "  -r ROOT   корневая папка, передаётся приложению как\n"
            "               root (по умолчанию %s)\n"
            "  -t N      число рабочих потоков (по умолчанию %d)\n"
            "  -m BYTES  максимальный размер тела запроса в байтах\n"
            "               (по умолчанию %d)\n"
            "  -T SEC    таймаут чтения запроса и записи ответа\n"
            "               (по умолчанию %d)\n"
            "  -q        не печатать журнал запросов\n"
            "  -h        показать эту справку\n",
            prog, FCGI_DEFAULT_PORT, FCGI_DEFAULT_ADDRESS,
            FCGI_DEFAULT_ROOT, FCGI_DEFAULT_THREADS, FCGI_DEFAULT_MAX,
            FCGI_DEFAULT_TIMEOUT);
}

static int bad_option(int opt, const char *value, const char *range)
{
    fprintf(stderr, "fcgi: неверное значение -%c %s (нужно %s)\n", opt,
            value, range);
    return 2;
}

/* Главный цикл: принять соединение и передать его в пул потоков. */
static void accept_loop(int listener, thread_pool_t *pool,
                            const server_t *server)
{
    while (!stop_requested) {
        struct pollfd pfd;
        int           ready, client;

        /* Ждём не дольше секунды, чтобы регулярно проверять флаг
         * остановки (сигнал к тому же прерывает poll() с EINTR). */
        pfd.fd      = listener;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, 1000);
        if (ready < 0 && errno != EINTR) {
            perror("fcgi: poll");
            break;
        }
        if (ready <= 0)
            continue;

        client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                /* Кончились дескрипторы: даём потокам закрыть
                 * соединения. */
                struct timespec pause = { 0, 100 * 1000 * 1000 };
                nanosleep(&pause, NULL);
            }
            continue; /* EAGAIN, ECONNABORTED, EINTR — ждём следующее */
        }

        setup_client(client, server->timeout_ms);
        if (thread_pool_push(pool, client) != 0) {
            /* Все потоки заняты и очередь полна: HTTP-ответа из
             * FastCGI-приложения не отправишь — просто закрываем,
             * веб-сервер увидит ошибку соединения. */
            socket_close(client, 0);
        }
    }
}

/*
 * Точка входа библиотеки: её вызывает main() пользовательской
 * программы (см. example/main.c). Функцию-обработчик сервер
 * получает готовым указателем — её код скомпонован с сервером в
 * одну программу. Блокируется до остановки сервера, поэтому
 * обычно её результат сразу возвращается из main().
 */
int fcgi_run(fcgi_function_t app, int argc, char **argv)
{
    static server_t server; /* static — значит, обнулена при запуске */
    const char     *address  = FCGI_DEFAULT_ADDRESS;
    const char     *port     = FCGI_DEFAULT_PORT;
    const char     *root     = FCGI_DEFAULT_ROOT;
    const char     *sockpath = NULL;
    const char     *host;
    long            threads  = FCGI_DEFAULT_THREADS;
    long            max      = FCGI_DEFAULT_MAX;
    long            timeout  = FCGI_DEFAULT_TIMEOUT;
    long            number;
    thread_pool_t  *pool;
    int             listener, opt;

    if (app == NULL) {
        fprintf(stderr, "fcgi: fcgi_run() получила NULL вместо функции\n");
        return 1;
    }

    /* ---- 1. Ключи командной строки ---- */
    while ((opt = getopt(argc, argv, "p:a:r:s:t:m:T:qh")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_number(optarg, 1, 65535, &number) != 0)
                return bad_option(opt, optarg, "1..65535");
            port = optarg;
            break;
        case 'a': address  = optarg; break;
        case 'r': root     = optarg; break;
        case 's': sockpath = optarg; break;
        case 't':
            if (parse_number(optarg, 1, 1024, &threads) != 0)
                return bad_option(opt, optarg, "1..1024");
            break;
        case 'm':
            if (parse_number(optarg, 1024, 1024L * 1024 * 1024, &max) != 0)
                return bad_option(opt, optarg, "1024..1073741824");
            break;
        case 'T':
            if (parse_number(optarg, 1, 3600, &timeout) != 0)
                return bad_option(opt, optarg, "1..3600");
            break;
        case 'q': server.quiet = 1; break;
        case 'h': usage(stdout, argv[0]); return 0;
        default:  usage(stderr, argv[0]); return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "fcgi: лишний аргумент: %s\n", argv[optind]);
        usage(stderr, argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* журнал — построчно, даже в файл */
    tzset();
    server.app         = app; /* указатель на функцию — от программы */
    server.max_request = (size_t)max;
    server.timeout_ms  = (int)timeout * 1000;
    server.threads     = threads;

    /* ---- 2. Корневая папка -> буфер root на 256 символов ---- */
    if (set_root(server.root, root) != 0)
        return 1;

    /* ---- 3. Сигналы и слушающий сокет ---- */
    setup_signals();
    if (sockpath != NULL)
        listener = socket_listen_unix(sockpath);
    else
        listener = socket_listen(address, port);
    if (listener < 0)
        return 1;

    /* ---- 4. Пул потоков: каждый поток вызывает fcgi_connection() */
    pool = thread_pool_create((size_t)threads, FCGI_QUEUE_SIZE,
                                fcgi_connection, &server);
    if (pool == NULL) {
        fprintf(stderr, "fcgi: не удалось запустить рабочие потоки\n");
        close(listener);
        if (sockpath != NULL)
            unlink(sockpath);
        return 1;
    }

    printf("fcgi: root = %s\n", server.root);
    printf("fcgi: рабочих потоков: %ld\n", threads);
    if (sockpath != NULL)
        printf("fcgi: слушаю Unix-сокет %s\n", sockpath);
    else {
        host = (strcmp(address, "0.0.0.0") == 0 ||
                strcmp(address, "::") == 0) ? "localhost" : address;
        printf("fcgi: слушаю %s, порт %s\n", host, port);
    }
    printf("fcgi: подключите веб-сервер (Ctrl+C — остановка)\n");

    /* ---- 5. Главный цикл ---- */
    accept_loop(listener, pool, &server);

    /* ---- 6. Остановка: не принимаем новые соединения и дожидаемся
     *         текущих запросов ---- */
    close(listener);
    thread_pool_destroy(pool);
    if (sockpath != NULL)
        unlink(sockpath);
    printf("fcgi: остановлен\n");
    return 0;
}
