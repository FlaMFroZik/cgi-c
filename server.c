/*
 * server.c — библиотека сервера: server_run(), приём соединений, сокеты.
 *
 * Это НЕ программа: функции main() здесь нет. Код компонуется с main.c
 * пользователя на этапе сборки (см. Makefile: libcgi-server.a), а CGI-функцию
 * библиотека получает от него обычным указателем — аргументом server_run().
 *
 * Путь одного запроса:
 *
 *   server_run() -> accept() -> thread_pool_push()      главный поток
 *                                    |
 *                                    v  очередь пула
 *   socket_client()                                       рабочий поток
 *     1. socket_read_request()  — читаем «голые» данные клиента
 *     2. server->cgi(...)       — вызываем CGI-функцию через указатель,
 *                                 передавая ей root (256 символов) и данные
 *     3. socket_close()         — закрываем соединение
 */
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 + XSI: realpath(), SA_RESETHAND */

#include "server.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* send() без сигнала SIGPIPE там, где это поддерживается (Linux, *BSD). */
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

/* Настройки и общее состояние сервера. Указатель на эту структуру получает
 * каждый рабочий поток (аргумент arg обработчика socket_client). */
typedef struct {
    char           root[CGI_ROOT_SIZE]; /* корневая папка — передаётся в CGI */
    cgi_function_t cgi;                 /* указатель на CGI-функцию (ее адрес
                                           передала программа в server_run()) */
    size_t         max_request;         /* предельный размер запроса, байт */
    int            timeout_ms;          /* таймаут чтения запроса и записи ответа */
    int            quiet;               /* 1 — не печатать журнал запросов */
} server_t;

/* Флаг остановки: его выставляет обработчик SIGINT/SIGTERM. */
static volatile sig_atomic_t stop_requested = 0;

/* ======================================================================== */
/* Вспомогательные функции                                                  */
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

/* Текст к HTTP-коду для ответов, которые сервер отправляет сам. */
static const char *reason_phrase(int code)
{
    switch (code) {
    case 400: return "Bad Request";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

/* ======================================================================== */
/* Чтение запроса                                                           */
/* ======================================================================== */

/*
 * Найти конец заголовков — пустую строку ("\r\n\r\n" или просто "\n\n").
 * Поиск начинается с позиции from. Возвращает позицию первого байта после
 * пустой строки (там начинается тело) или 0, если она ещё не пришла.
 */
static size_t find_head_end(const char *buf, size_t len, size_t from)
{
    size_t i;

    for (i = from; i < len; i++) {
        if (buf[i] != '\n')
            continue;
        if (i + 1 < len && buf[i + 1] == '\n')
            return i + 2;
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n')
            return i + 3;
    }
    return 0;
}

/* Только то, что нужно серверу из заголовков, чтобы дочитать запрос. */
typedef struct {
    long long length;  /* Content-Length или -1, если заголовка нет */
    int       bad;     /* Content-Length испорчен или их несколько разных */
    int       chunked; /* есть Transfer-Encoding — не поддерживается */
    int       expect;  /* Expect: 100-continue — клиент ждёт разрешения слать тело */
} head_info_t;

/* Совпадает ли имя заголовка (без учёта регистра). */
static int header_is(const char *name, size_t len, const char *expected)
{
    return strlen(expected) == len && strncasecmp(name, expected, len) == 0;
}

/* Десятичное число без знака; -1, если это не число. */
static long long parse_length(const char *s, size_t len)
{
    long long value = 0;
    size_t    i;

    if (len == 0)
        return -1;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9' || value > (LLONG_MAX - 9) / 10)
            return -1;
        value = value * 10 + (s[i] - '0');
    }
    return value;
}

/*
 * Пройти по строкам заголовков buf[0..end) и заполнить info.
 * Первая строка ("GET / HTTP/1.1") пропускается. Сервер смотрит только на
 * три заголовка; всё остальное разбирает сама CGI, если ей нужно.
 */
static void parse_head(const char *buf, size_t end, head_info_t *info)
{
    const char *stop = buf + end;
    const char *line = memchr(buf, '\n', end);

    info->length  = -1;
    info->bad     = 0;
    info->chunked = 0;
    info->expect  = 0;

    while (line != NULL && ++line < stop) {
        const char *eol = memchr(line, '\n', (size_t)(stop - line));
        const char *colon;
        size_t      len;

        if (eol == NULL)
            break;
        len = (size_t)(eol - line);
        if (len > 0 && line[len - 1] == '\r')
            len--;

        colon = memchr(line, ':', len);
        if (colon != NULL) {
            size_t      name_len  = (size_t)(colon - line);
            const char *value     = colon + 1;
            size_t      value_len = len - name_len - 1;

            while (value_len > 0 && (*value == ' ' || *value == '\t')) {
                value++;
                value_len--;
            }
            while (value_len > 0 && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t'))
                value_len--;

            if (header_is(line, name_len, "Content-Length")) {
                long long n = parse_length(value, value_len);
                if (n < 0 || (info->length >= 0 && info->length != n))
                    info->bad = 1;
                else
                    info->length = n;
            } else if (header_is(line, name_len, "Transfer-Encoding")) {
                info->chunked = 1;
            } else if (header_is(line, name_len, "Expect")) {
                if (value_len == 12 && strncasecmp(value, "100-continue", 12) == 0)
                    info->expect = 1;
            }
        }
        line = eol;
    }
}

int socket_read_request(int client, size_t max_size, int timeout_ms,
                        char **data, size_t *size)
{
    size_t    cap      = 8192; /* текущий размер буфера */
    size_t    len      = 0;    /* сколько байт уже прочитано */
    size_t    head     = 0;    /* конец заголовков (0 — ещё не найден) */
    size_t    total    = 0;    /* полный размер запроса, когда он известен */
    size_t    scanned  = 0;    /* откуда продолжать поиск конца заголовков */
    long long deadline = now_us() + (long long)timeout_ms * 1000;
    int       status   = -1;
    char     *buf;

    *data = NULL;
    *size = 0;

    if (cap > max_size)
        cap = max_size;
    buf = malloc(cap + 1); /* +1 — место для '\0' после данных */
    if (buf == NULL)
        return 500;

    for (;;) {
        struct pollfd pfd;
        long long     left;
        ssize_t       n;

        /* 1. Пришёл конец заголовков? Тогда из них узнаём размер тела. */
        if (head == 0) {
            head    = find_head_end(buf, len, scanned);
            scanned = len > 2 ? len - 2 : 0;
            if (head != 0) {
                head_info_t info;

                parse_head(buf, head, &info);
                if (info.bad) {
                    status = 400;
                    break;
                }
                if (info.chunked) { /* тело без длины — просим Content-Length */
                    status = 411;
                    break;
                }
                if (info.length > 0 && (unsigned long long)info.length > max_size - head) {
                    status = 413;
                    break;
                }
                total = head + (info.length > 0 ? (size_t)info.length : 0);

                /* curl и другие клиенты перед большим телом ждут «100 Continue». */
                if (info.expect && len < total)
                    send_all(client, "HTTP/1.1 100 Continue\r\n\r\n", 25);
            }
        }

        /* 2. Запрос получен целиком. */
        if (head != 0 && len >= total) {
            status = 0;
            break;
        }

        /* 3. Буфер заполнен — увеличиваем вдвое, но не больше max_size. */
        if (len == cap) {
            char *bigger;

            if (cap >= max_size) {
                status = head != 0 ? 413 : 431;
                break;
            }
            cap    = cap * 2 < max_size ? cap * 2 : max_size;
            bigger = realloc(buf, cap + 1);
            if (bigger == NULL) {
                status = 500;
                break;
            }
            buf = bigger;
        }

        /* 4. Ждём новые данные, но не дольше общего таймаута на запрос:
         *    медленный клиент не займёт поток навсегда. */
        left = (deadline - now_us()) / 1000;
        if (left <= 0) {
            status = len > 0 ? 408 : -1;
            break;
        }
        pfd.fd      = client;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        n = poll(&pfd, 1, (int)left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            break;
        if (n == 0) {
            status = len > 0 ? 408 : -1;
            break;
        }

        /* 5. Читаем всё, что пришло. */
        n = recv(client, buf + len, cap - len, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        if (n == 0) { /* клиент закончил передачу */
            if (len > 0 && head == 0) {
                /* Данные без пустой строки в конце (не HTTP?) — всё равно
                 * отдаём их CGI как есть: это и есть «голые» данные. */
                total  = len;
                status = 0;
            } else if (len > 0) {
                status = 400; /* тело оборвалось раньше Content-Length */
            }
            break;
        }
        len += (size_t)n;
    }

    if (status == 0 && len > total)
        len = total; /* лишние байты (следующий запрос) CGI не передаём */

    buf[len] = '\0';
    *data    = buf;
    *size    = len;
    return status;
}

/* ======================================================================== */
/* Сокеты                                                                   */
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
        fprintf(stderr, "socket: неверный адрес %s:%s: %s\n", address, port, gai_strerror(err));
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

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, SOMAXCONN) == 0)
            break;

        saved = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);

    if (fd < 0) {
        fprintf(stderr, "socket: не удалось открыть %s:%s: %s\n", address, port, strerror(saved));
        return -1;
    }

    /* Неблокирующий: если клиент отвалится между poll() и accept(),
     * accept() не зависнет, а вернёт EAGAIN. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

void socket_send_status(int client, int code)
{
    const char *reason = reason_phrase(code);
    char        body[128];
    char        buf[512];
    int         body_len, n;

    body_len = snprintf(body, sizeof body, "%d %s\n", code, reason);
    n = snprintf(buf, sizeof buf,
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 code, reason, body_len, body);
    if (n > 0 && (size_t)n < sizeof buf)
        send_all(client, buf, (size_t)n);
}

void socket_close(int client, int linger)
{
    char      trash[4096];
    size_t    drained  = 0;
    long long deadline = now_us() + (linger ? 2000000 : 0); /* до 2 с */

    /* FIN: «ответ закончен». Клиент, читающий до конца потока, сразу
     * понимает, что получил всё. */
    shutdown(client, SHUT_WR);

    /*
     * Если закрыть сокет, в котором остались непрочитанные входящие данные,
     * ядро отправит RST, и клиент может потерять уже отправленный ему ответ.
     * Поэтому выбрасываем то, что клиент успел прислать, а при linger ещё
     * и немного ждём остаток (например, тело, которое мы отклонили с 413).
     */
    for (;;) {
        struct pollfd pfd;
        long long     left = (deadline - now_us()) / 1000;
        ssize_t       n;
        int           ready;

        pfd.fd      = client;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, left > 0 ? (int)left : 0);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            break;

        n = recv(client, trash, sizeof trash, 0);
        if (n <= 0) /* клиент закрыл соединение или ошибка */
            break;
        drained += (size_t)n;
        if (drained >= 1024 * 1024) /* бесконечно не читаем */
            break;
    }
    close(client);
}

/* Настроить принятый сокет клиента. */
static void setup_client(int client, int timeout_ms)
{
    struct timeval tv;
    int            one   = 1;
    int            flags = fcntl(client, F_GETFL);

    /* На BSD/macOS принятый сокет наследует O_NONBLOCK слушающего — снимаем:
     * CGI работает с обычным блокирующим сокетом. */
    if (flags >= 0)
        fcntl(client, F_SETFL, flags & ~O_NONBLOCK);
    fcntl(client, F_SETFD, FD_CLOEXEC); /* не наследовать при exec() из CGI */

    /* Отправлять части ответа сразу, без задержки алгоритма Нейгла. */
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    /* Таймауты на чтение и запись: даже если CGI сама читает из сокета или
     * пишет большой ответ, зависший клиент не займёт поток навсегда. */
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* ======================================================================== */
/* Обработка соединения (выполняется в рабочем потоке)                      */
/* ======================================================================== */

/* IP-адрес клиента текстом — для журнала. */
static void peer_address(int client, char *out, size_t size)
{
    struct sockaddr_storage addr;
    socklen_t               len = sizeof addr;

    snprintf(out, size, "?");
    if (getpeername(client, (struct sockaddr *)&addr, &len) != 0)
        return;

    if (addr.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, out, (socklen_t)size);
    else if (addr.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr, out, (socklen_t)size);

    /* IPv4-клиент на IPv6-сокете выглядит как "::ffff:1.2.3.4" — убираем префикс. */
    if (strncmp(out, "::ffff:", 7) == 0 && strchr(out + 7, '.') != NULL)
        memmove(out, out + 7, strlen(out + 7) + 1);
}

/* Строка журнала: [время] адрес "первая строка запроса" код длительность. */
static void log_request(const char *addr, const char *data, size_t size,
                        int status, long long elapsed_us)
{
    char      line[160];
    char      when[32];
    char      code[16];
    size_t    i, n = 0;
    time_t    now = time(NULL);
    struct tm tm;

    /* Первая строка запроса ("GET / HTTP/1.1"), непечатные символы -> '?'. */
    for (i = 0; i < size && data[i] != '\r' && data[i] != '\n'; i++) {
        unsigned char c = (unsigned char)data[i];

        if (n + 4 >= sizeof line) {
            memcpy(line + n, "...", 3);
            n += 3;
            break;
        }
        line[n++] = (c >= 0x20 && c < 0x7f && c != '"') ? (char)c : '?';
    }
    line[n] = '\0';

    if (status > 0)
        snprintf(code, sizeof code, "%d", status);
    else
        snprintf(code, sizeof code, "-");

    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    /* Один вызов printf на строку: stdio сам блокирует поток вывода,
     * поэтому строки из разных потоков не перемешаются. */
    printf("[%s] %s \"%s\" %s %.1f ms\n", when, addr, n > 0 ? line : "-", code,
           (double)elapsed_us / 1000.0);
}

/*
 * Обработчик соединения. Пул потоков вызывает его через указатель
 * thread_task_t (см. thread.h); arg — указатель на server_t.
 */
static void socket_client(int client, void *arg)
{
    server_t *server = arg;
    char      root[CGI_ROOT_SIZE]; /* личная копия root для этого запроса */
    char      addr[INET6_ADDRSTRLEN];
    char     *data   = NULL;
    size_t    size   = 0;
    long long start  = now_us();
    int       status = 0;
    int       result;

    peer_address(client, addr, sizeof addr);
    result = socket_read_request(client, server->max_request, server->timeout_ms, &data, &size);

    if (result == 0) {
        /* Копируем все 256 байт root: CGI может менять свой буфер,
         * никак не влияя на другие потоки. */
        memcpy(root, server->root, CGI_ROOT_SIZE);
        root[CGI_ROOT_SIZE - 1] = '\0'; /* строка гарантированно заканчивается в буфере */

        /* Вызов CGI-функции через указатель, переданный в server_run().
         * Адрес известен уже на этапе компоновки программы — никакой
         * динамической загрузки во время работы. */
        status = server->cgi(root, data, size, client);
        if (status < 0) { /* CGI сообщила, что ответ не отправлен */
            socket_send_status(client, 500);
            status = 500;
        }
    } else if (result > 0) { /* запрос не прочитан: 400, 408, 411, 413, 431... */
        socket_send_status(client, result);
        status = result;
    }

    if (!server->quiet && result >= 0)
        log_request(addr, data, size, status, now_us() - start);

    free(data);
    socket_close(client, result > 0);
}

/* ======================================================================== */
/* server_run()                                                             */
/* ======================================================================== */

static void on_signal(int sig)
{
    static const char msg[] =
        "\nserver: остановка... (повторный Ctrl+C — выйти немедленно)\n";
    ssize_t ignored;

    (void)sig;
    stop_requested = 1;
    ignored = write(STDERR_FILENO, msg, sizeof msg - 1); /* printf() здесь нельзя */
    (void)ignored;
}

static void setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    /* Ctrl+C и kill: мягкая остановка. SA_RESETHAND — второй такой же сигнал
     * сработает как обычно и сразу завершит процесс. */
    sa.sa_handler = on_signal;
    sa.sa_flags   = SA_RESETHAND;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Запись в сокет, который клиент уже закрыл, по умолчанию убивает весь
     * процесс сигналом SIGPIPE. Игнорируем его: write()/send() просто вернут
     * ошибку EPIPE. Это важно и для CGI, которая сама пишет в сокет. */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags   = 0;
    sigaction(SIGPIPE, &sa, NULL);
}

/* Абсолютный путь к корню -> буфер root из CGI_ROOT_SIZE (256) символов. */
static int set_root(char *root, const char *path)
{
    struct stat st;
    char       *full = realpath(path, NULL);
    size_t      len;

    if (full == NULL) {
        fprintf(stderr, "server: корневая папка %s: %s\n", path, strerror(errno));
        return -1;
    }
    len = strlen(full);

    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "server: %s — не папка\n", full);
        free(full);
        return -1;
    }
    if (len >= CGI_ROOT_SIZE) {
        fprintf(stderr, "server: путь к корню длиннее %d символов: %s\n",
                CGI_ROOT_SIZE - 1, full);
        free(full);
        return -1;
    }

    memset(root, 0, CGI_ROOT_SIZE);
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
    if (errno != 0 || end == text || *end != '\0' || value < min || value > max)
        return -1;
    *out = value;
    return 0;
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Использование: %s [ключи]\n"
            "\n"
            "  -p PORT   порт (по умолчанию %s)\n"
            "  -a ADDR   адрес для прослушивания (по умолчанию %s; IPv6 — ::)\n"
            "  -r ROOT   корневая папка, передаётся в CGI как root (по умолчанию %s)\n"
            "  -t N      число рабочих потоков (по умолчанию %d)\n"
            "  -m BYTES  максимальный размер запроса в байтах (по умолчанию %d)\n"
            "  -T SEC    таймаут чтения запроса и записи ответа (по умолчанию %d)\n"
            "  -q        не печатать журнал запросов\n"
            "  -h        показать эту справку\n",
            prog, SERVER_DEFAULT_PORT, SERVER_DEFAULT_ADDRESS, SERVER_DEFAULT_ROOT,
            SERVER_DEFAULT_THREADS, SERVER_DEFAULT_MAX, SERVER_DEFAULT_TIMEOUT);
}

static int bad_option(int opt, const char *value, const char *range)
{
    fprintf(stderr, "server: неверное значение -%c %s (нужно %s)\n", opt, value, range);
    return 2;
}

/* Главный цикл: принять соединение и передать его в пул потоков. */
static void accept_loop(int listener, thread_pool_t *pool, const server_t *server)
{
    while (!stop_requested) {
        struct pollfd pfd;
        int           ready, client;

        /* Ждём не дольше секунды, чтобы регулярно проверять флаг остановки
         * (сигнал к тому же прерывает poll() с EINTR). */
        pfd.fd      = listener;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        ready = poll(&pfd, 1, 1000);
        if (ready < 0 && errno != EINTR) {
            perror("server: poll");
            break;
        }
        if (ready <= 0)
            continue;

        client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                /* Кончились дескрипторы: даём потокам закрыть соединения. */
                struct timespec pause = { 0, 100 * 1000 * 1000 };
                nanosleep(&pause, NULL);
            }
            continue; /* EAGAIN, ECONNABORTED, EINTR — ждём следующее */
        }

        setup_client(client, server->timeout_ms);
        if (thread_pool_push(pool, client) != 0) {
            /* Все потоки заняты и очередь полна — отвечаем «перегружен». */
            socket_send_status(client, 503);
            socket_close(client, 0);
        }
    }
}

/*
 * Точка входа библиотеки: её вызывает main() пользовательской программы
 * (см. example/main.c). CGI-функцию сервер получает готовым указателем —
 * её код скомпонован с сервером в одну программу. Блокируется до остановки
 * сервера, поэтому обычно её результат сразу возвращается из main().
 */
int server_run(cgi_function_t cgi, int argc, char **argv)
{
    static server_t server; /* static — значит, обнулена при запуске */
    const char     *address  = SERVER_DEFAULT_ADDRESS;
    const char     *port     = SERVER_DEFAULT_PORT;
    const char     *root     = SERVER_DEFAULT_ROOT;
    const char     *host;
    long            threads  = SERVER_DEFAULT_THREADS;
    long            max      = SERVER_DEFAULT_MAX;
    long            timeout  = SERVER_DEFAULT_TIMEOUT;
    long            number;
    thread_pool_t  *pool;
    int             listener, opt;

    if (cgi == NULL) {
        fprintf(stderr, "server: server_run() получила NULL вместо CGI-функции\n");
        return 1;
    }

    /* ---- 1. Ключи командной строки ---- */
    while ((opt = getopt(argc, argv, "p:a:r:t:m:T:qh")) != -1) {
        switch (opt) {
        case 'p':
            if (parse_number(optarg, 1, 65535, &number) != 0)
                return bad_option(opt, optarg, "1..65535");
            port = optarg;
            break;
        case 'a': address  = optarg; break;
        case 'r': root     = optarg; break;
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
        fprintf(stderr, "server: лишний аргумент: %s\n", argv[optind]);
        usage(stderr, argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* журнал — построчно, даже в файл */
    tzset();
    server.cgi         = cgi; /* указатель на CGI-функцию — от программы */
    server.max_request = (size_t)max;
    server.timeout_ms  = (int)timeout * 1000;

    /* ---- 2. Корневая папка -> буфер root на 256 символов ---- */
    if (set_root(server.root, root) != 0)
        return 1;

    /* ---- 3. Сигналы и слушающий сокет ---- */
    setup_signals();
    listener = socket_listen(address, port);
    if (listener < 0)
        return 1;

    /* ---- 4. Пул потоков: каждый поток вызывает socket_client() ---- */
    pool = thread_pool_create((size_t)threads, SERVER_QUEUE_SIZE, socket_client, &server);
    if (pool == NULL) {
        fprintf(stderr, "server: не удалось запустить рабочие потоки\n");
        close(listener);
        return 1;
    }

    host = address;
    if (strcmp(address, "0.0.0.0") == 0 || strcmp(address, "::") == 0)
        host = "localhost";
    printf("server: root = %s\n", server.root);
    printf("server: рабочих потоков: %ld, слушаю %s, порт %s\n", threads, address, port);
    printf("server: откройте http://%s%s%s:%s/  (Ctrl+C — остановка)\n",
           strchr(host, ':') ? "[" : "", host, strchr(host, ':') ? "]" : "", port);

    /* ---- 5. Главный цикл ---- */
    accept_loop(listener, pool, &server);

    /* ---- 6. Остановка: не принимаем новые соединения и дожидаемся
     *         текущих запросов ---- */
    close(listener);
    thread_pool_destroy(pool);
    printf("server: остановлен\n");
    return 0;
}
