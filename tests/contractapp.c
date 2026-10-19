/*
 * contractapp.c — приложение для самопроверки обещаний fcgi.h.
 *
 * Проверяет то, на что может рассчитывать обработчик:
 *   - data не NULL даже у запроса без тела;
 *   - после последнего байта тела стоит '\0' (data[size] == '\0');
 *   - буфер тела можно менять;
 *   - fcgi_printf() пишет строку целиком, не обрезая её.
 *
 * Ответ содержит результат проверки и код 200/500, поэтому
 * tests/test_fcgi.py замечает нарушение контракта.
 */
#include "fcgi.h"

#include <stdio.h>
#include <string.h>

static int app(char *root, fcgi_env_t *env, char *data, size_t size,
               fcgi_stream_t *out)
{
    const char *uri = fcgi_env_get(env, "REQUEST_URI");
    char        body[512];
    int         ok = data != NULL && data[size] == '\0';
    int         n;

    (void)root;

    if (uri != NULL && strcmp(uri, "/long") == 0) {
        /* 9999 байт через fcgi_printf: старый код обрезал их до 4095. */
        static char big[10000];

        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        fcgi_printf(out,
                    "Status: 200 OK\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n%s",
                    sizeof big - 1, big);
        return 200;
    }

    /* fcgi.h разрешает разбирать тело «на месте» — проверим, что
     * буфер действительно наш и запись не падает. */
    if (size > 0)
        data[0] = data[0];

    n = snprintf(body, sizeof body, "data=%s size=%zu nul=%s\n",
                 data != NULL ? "ok" : "null", size,
                 ok ? "ok" : "bad");

    fcgi_printf(out,
                "Status: %d %s\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: %d\r\n"
                "\r\n%s",
                ok ? 200 : 500,
                ok ? "OK" : "Internal Server Error",
                n, body);
    return ok ? 200 : 500;
}

int main(int argc, char **argv)
{
    return fcgi_run(app, argc, argv);
}
