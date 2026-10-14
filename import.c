/*
 * import.c — загрузка CGI-функции из динамической библиотеки (.so).
 *
 * Вся работа с dlopen()/dlsym()/dlclose() собрана здесь: остальной сервер
 * видит только указатель на функцию cgi_function_t (см. import.h).
 */
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 + XSI */

#include "import.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/*
 * dlsym() возвращает void*, а нам нужен указатель на функцию. Стандарт C
 * такое преобразование не описывает, но POSIX требует, чтобы оно работало
 * (размеры указателей совпадают). Проверяем это ещё при компиляции.
 */
_Static_assert(sizeof(cgi_function_t) == sizeof(void *),
               "указатель на функцию должен быть размером с void*");

int import_library(cgi_import_t *cgi, const char *path, const char *name)
{
    char        local[4096];
    void       *library;
    void       *symbol;
    const char *error;

    cgi->library  = NULL;
    cgi->function = NULL;

    /*
     * Если в пути нет '/', dlopen() ищет файл в системных папках
     * (как компоновщик ищет -lfoo), а не в текущей. Добавляем "./",
     * чтобы "-l cgi.so" означало файл cgi.so в текущей папке.
     */
    if (strchr(path, '/') == NULL) {
        int n = snprintf(local, sizeof local, "./%s", path);
        if (n < 0 || (size_t)n >= sizeof local) {
            fprintf(stderr, "import: слишком длинный путь: %s\n", path);
            return -1;
        }
        path = local;
    }

    /* RTLD_NOW   — разрешить все символы сразу: ошибка всплывёт при старте,
     *              а не посреди обработки запроса;
     * RTLD_LOCAL — символы библиотеки не видны другим библиотекам. */
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "import: не удалось загрузить библиотеку: %s\n", dlerror());
        return -1;
    }

    dlerror(); /* сбросить старую ошибку, чтобы проверить результат dlsym() */
    symbol = dlsym(library, name);
    error  = dlerror();
    if (error != NULL || symbol == NULL) {
        fprintf(stderr, "import: в %s нет функции %s: %s\n", path, name,
                error != NULL ? error : "адрес равен NULL");
        dlclose(library);
        return -1;
    }

    /* void* -> указатель на функцию. memcpy — переносимый способ сделать это
     * без предупреждений компилятора (так рекомендует POSIX). */
    cgi->library = library;
    memcpy(&cgi->function, &symbol, sizeof cgi->function);
    return 0;
}

int import_call(const cgi_import_t *cgi, char *root, char *data, size_t size, int client)
{
    if (cgi == NULL || cgi->function == NULL)
        return -1;

    root[CGI_ROOT_SIZE - 1] = '\0'; /* гарантируем, что строка закончится в буфере */

    /* Вызов CGI-функции через указатель. */
    return cgi->function(root, data, size, client);
}

void import_close(cgi_import_t *cgi)
{
    if (cgi->library != NULL)
        dlclose(cgi->library);
    cgi->library  = NULL;
    cgi->function = NULL;
}
