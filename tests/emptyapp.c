/*
 * emptyapp.c — приложение для самопроверки библиотеки.
 *
 * Обработчик НАМЕРЕННО ничего не пишет в поток ответа и возвращает
 * успешный код. Библиотека обязана не отправить пустой STDOUT
 * (веб-сервер счёл бы это ошибкой), а сформировать полноценный
 * ответ Status: 500 Internal Server Error.
 *
 * Собирается и запускается tests/test_fcgi.py (см. make test).
 */
#include "fcgi.h"

static int app(char *root, fcgi_env_t *env, char *data, size_t size,
               fcgi_stream_t *out)
{
    (void)root;
    (void)env;
    (void)data;
    (void)size;
    (void)out;
    return 200;
}

int main(int argc, char **argv)
{
    return fcgi_run(app, argc, argv);
}
