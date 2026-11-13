/*-
  buffer_pthread.c -- an executor for buffer.c on POSIX threads

  This file is part of lbzip2.

  lbzip2 is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  lbzip2 is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with lbzip2.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
  Kept apart from buffer.c so that a program with threads of its own, or with
  no POSIX threads, can use buffer.c without it.
*/

#include "buffer.h"

#include <pthread.h>
#include <stdbool.h>            /* bool */
#include <stdlib.h>             /* malloc() */


struct task {
  void (*fn)(void *);
  void *arg;
  struct task *next;
  bool done;
};

struct pool {
  struct lbzip2_executor executor;     /* first, so the two convert */
  pthread_mutex_t mutex;
  pthread_cond_t work;                 /* a task was queued, or stop */
  pthread_cond_t done;                 /* a task finished */
  struct task *head;                   /* queued tasks, oldest first */
  struct task *tail;
  bool stop;
  unsigned nthreads;                   /* threads started */
  pthread_t *threads;
};


static void *
worker(void *arg)
{
  struct pool *pool = arg;

  pthread_mutex_lock(&pool->mutex);
  for (;;) {
    struct task *task;

    while (pool->head == NULL && !pool->stop)
      pthread_cond_wait(&pool->work, &pool->mutex);
    if (pool->head == NULL)
      break;

    task = pool->head;
    pool->head = task->next;
    if (pool->head == NULL)
      pool->tail = NULL;

    pthread_mutex_unlock(&pool->mutex);
    task->fn(task->arg);
    pthread_mutex_lock(&pool->mutex);

    task->done = true;
    pthread_cond_broadcast(&pool->done);
  }
  pthread_mutex_unlock(&pool->mutex);

  return NULL;
}

static void *
submit(void *opaque, void (*fn)(void *), void *arg)
{
  struct pool *pool = opaque;
  struct task *task = malloc(sizeof *task);

  /* Without memory for the task, the caller runs it. */
  if (task == NULL)
    return NULL;
  task->fn = fn;
  task->arg = arg;
  task->next = NULL;
  task->done = false;

  pthread_mutex_lock(&pool->mutex);
  if (pool->tail != NULL)
    pool->tail->next = task;
  else
    pool->head = task;
  pool->tail = task;
  pthread_cond_signal(&pool->work);
  pthread_mutex_unlock(&pool->mutex);

  return task;
}

static void
wait_for(void *opaque, void *handle)
{
  struct pool *pool = opaque;
  struct task *task = handle;

  pthread_mutex_lock(&pool->mutex);
  while (!task->done)
    pthread_cond_wait(&pool->done, &pool->mutex);
  pthread_mutex_unlock(&pool->mutex);

  free(task);
}


static void
stop_pool(struct pool *pool)
{
  unsigned i;

  pthread_mutex_lock(&pool->mutex);
  pool->stop = true;
  pthread_cond_broadcast(&pool->work);
  pthread_mutex_unlock(&pool->mutex);

  for (i = 0; i < pool->nthreads; i++)
    pthread_join(pool->threads[i], NULL);

  pthread_cond_destroy(&pool->done);
  pthread_cond_destroy(&pool->work);
  pthread_mutex_destroy(&pool->mutex);
  free(pool->threads);
  free(pool);
}

struct lbzip2_executor *
lbzip2_pthread_executor_create(unsigned threads)
{
  struct pool *pool;

  if (threads == 0)
    return NULL;
  pool = malloc(sizeof *pool);
  if (pool == NULL)
    return NULL;
  pool->threads = malloc(threads * sizeof *pool->threads);
  if (pool->threads == NULL) {
    free(pool);
    return NULL;
  }

  pool->executor.submit = submit;
  pool->executor.wait = wait_for;
  pool->executor.opaque = pool;
  pool->head = NULL;
  pool->tail = NULL;
  pool->stop = false;
  pool->nthreads = 0;
  pthread_mutex_init(&pool->mutex, NULL);
  pthread_cond_init(&pool->work, NULL);
  pthread_cond_init(&pool->done, NULL);

  while (pool->nthreads < threads) {
    if (pthread_create(&pool->threads[pool->nthreads], NULL, worker,
                       pool) != 0) {
      stop_pool(pool);
      return NULL;
    }
    pool->nthreads++;
  }

  return &pool->executor;
}

void
lbzip2_pthread_executor_destroy(struct lbzip2_executor *executor)
{
  if (executor != NULL)
    stop_pool(executor->opaque);
}
