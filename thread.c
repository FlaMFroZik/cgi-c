/*
 * thread.c — пул рабочих потоков с очередью соединений.
 *
 * Очередь — кольцевой буфер сокетов, защищённый мьютексом. Главный поток
 * добавляет в неё соединения (thread_pool_push), рабочие потоки ждут на
 * условной переменной и забирают их по одному (thread_worker).
 */
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 + XSI */

#include "thread.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct thread_pool {
    pthread_mutex_t lock;     /* защищает все поля ниже */
    pthread_cond_t  ready;    /* «в очереди есть работа» или «пора выходить» */

    int            *queue;    /* кольцевой буфер сокетов */
    size_t          capacity; /* размер буфера */
    size_t          head;     /* индекс первого сокета в очереди */
    size_t          count;    /* сколько сокетов в очереди */
    int             stopping; /* 1 — пул останавливается */

    pthread_t      *threads;  /* идентификаторы рабочих потоков */
    size_t          started;  /* сколько потоков реально запущено */

    thread_task_t   task;     /* указатель на функцию-обработчик */
    void           *arg;      /* её второй аргумент */
};

/* Тело рабочего потока: взять соединение из очереди -> обработать -> повторить. */
static void *thread_worker(void *data)
{
    thread_pool_t *pool = data;

    for (;;) {
        int client;

        pthread_mutex_lock(&pool->lock);
        while (pool->count == 0 && !pool->stopping)
            pthread_cond_wait(&pool->ready, &pool->lock);

        if (pool->count == 0) { /* пул останавливается, а очередь пуста */
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }

        client     = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pthread_mutex_unlock(&pool->lock);

        /* Соединение обрабатывается уже без блокировки — параллельно
         * с другими потоками. Вызов идёт через указатель на функцию. */
        pool->task(client, pool->arg);
    }
}

thread_pool_t *thread_pool_create(size_t threads, size_t queue_size,
                                  thread_task_t task, void *arg)
{
    thread_pool_t *pool;
    sigset_t       block, old;
    size_t         i;

    if (threads == 0 || queue_size == 0 || task == NULL)
        return NULL;

    pool = calloc(1, sizeof *pool);
    if (pool == NULL)
        return NULL;

    pool->queue   = calloc(queue_size, sizeof *pool->queue);
    pool->threads = calloc(threads, sizeof *pool->threads);
    if (pool->queue == NULL || pool->threads == NULL) {
        free(pool->queue);
        free(pool->threads);
        free(pool);
        return NULL;
    }
    pool->capacity = queue_size;
    pool->task     = task;
    pool->arg      = arg;
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->ready, NULL);

    /*
     * Сигналы остановки (Ctrl+C, kill) должен получать только главный поток:
     * тогда он сразу проснётся и перестанет принимать соединения. Новый поток
     * наследует маску сигналов создателя, поэтому на время создания рабочих
     * потоков SIGINT и SIGTERM блокируются, а потом маска восстанавливается.
     */
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);

    for (i = 0; i < threads; i++) {
        int err = pthread_create(&pool->threads[i], NULL, thread_worker, pool);
        if (err != 0) {
            fprintf(stderr, "thread: не удалось создать поток: %s\n", strerror(err));
            break;
        }
        pool->started++;
    }

    pthread_sigmask(SIG_SETMASK, &old, NULL);

    if (pool->started < threads) {
        thread_pool_destroy(pool);
        return NULL;
    }
    return pool;
}

int thread_pool_push(thread_pool_t *pool, int client)
{
    int result = -1;

    pthread_mutex_lock(&pool->lock);
    if (!pool->stopping && pool->count < pool->capacity) {
        pool->queue[(pool->head + pool->count) % pool->capacity] = client;
        pool->count++;
        pthread_cond_signal(&pool->ready); /* разбудить один свободный поток */
        result = 0;
    }
    pthread_mutex_unlock(&pool->lock);
    return result;
}

void thread_pool_destroy(thread_pool_t *pool)
{
    size_t i;

    if (pool == NULL)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->ready); /* разбудить все потоки */
    pthread_mutex_unlock(&pool->lock);

    /* Потоки выходят, только когда очередь опустела, — ни одно принятое
     * соединение не теряется. */
    for (i = 0; i < pool->started; i++)
        pthread_join(pool->threads[i], NULL);

    pthread_cond_destroy(&pool->ready);
    pthread_mutex_destroy(&pool->lock);
    free(pool->threads);
    free(pool->queue);
    free(pool);
}
