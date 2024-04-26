#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <float.h>
#include <math.h>
#include <errno.h>

enum status {
	NOT_STARTED = 0,
	RUNNING,
	FINISHED, 
	DETACHED
};

struct thread_task {
	thread_task_f function;
	void *arg;

	int status;

	struct thread_pool *pool;
	void *result;

	pthread_mutex_t mutex;
	pthread_cond_t finished; // for waiting tasks in join

	struct thread_task *next; // tasks queue (singly linked list)
};

struct thread_pool {
	pthread_t *threads;

	int max_thread_count;
	int current_thread_count;
	int waiting_thread_count;

	int queue_tasks_size;
	struct thread_task *queue_tasks_front;
	struct thread_task *queue_tasks_back;

	pthread_cond_t new_task; // for new tasks and termination signals
	pthread_mutex_t mutex;

	bool terminate;
};

static struct thread_task *pop_front(struct thread_pool *pool) {
	struct thread_task *task = pool->queue_tasks_front;
	pool->queue_tasks_front = task->next;
	if (!pool->queue_tasks_front) {
		pool->queue_tasks_back = NULL;
	}
	task->next = NULL;
	pool->queue_tasks_size--;
	return task;
}

static void push_back(struct thread_pool *pool, struct thread_task *task) {
	pthread_mutex_lock(&task->mutex);
	task->pool = pool;
	task->status = NOT_STARTED;

	if (!pool->queue_tasks_front) {
		pool->queue_tasks_back = pool->queue_tasks_front = task;
	} else {
		pool->queue_tasks_back->next = task;
		pool->queue_tasks_back = task;
	}
	pool->queue_tasks_size++;
	pthread_mutex_unlock(&task->mutex);
}

static void execute(struct thread_task *task) {
	pthread_mutex_lock(&task->mutex);
	if (task->status != DETACHED) {
		task->status = RUNNING;
	}
	pthread_mutex_unlock(&task->mutex);
	task->pool->waiting_thread_count--;
	pthread_mutex_unlock(&task->pool->mutex);

	void *result = task->function(task->arg);

	pthread_mutex_lock(&task->pool->mutex);
	task->pool->waiting_thread_count++;
	pthread_mutex_lock(&task->mutex); 
	if (task->status != DETACHED) {
		task->status = FINISHED;
	}
	task->result = result;
}

static void *thread_f(void *args) {
	struct thread_pool *pool = args;
	pthread_mutex_lock(&pool->mutex);
	pool->waiting_thread_count++;
	while(true) {
		while (!pool->queue_tasks_size && !pool->terminate) {
			pthread_cond_wait(&pool->new_task, &pool->mutex);
		}
		if (pool->terminate) {
			pthread_mutex_unlock(&pool->mutex);
			return NULL;
		}
		struct thread_task *task = pop_front(pool);
		execute(task);
		if (task->status == DETACHED) {
			task->pool = NULL;
			pthread_mutex_unlock(&task->mutex);
			thread_task_delete(task);
		} else {	
			pthread_cond_signal(&task->finished);
			pthread_mutex_unlock(&task->mutex);
		}
	}
	return NULL;
}


static struct thread_pool *init_pool(int max_thread_count) {
	struct thread_pool *pool = malloc(sizeof(struct thread_pool));
	pool->threads = malloc(sizeof(pthread_t) * max_thread_count);
	
	pool->max_thread_count = max_thread_count;
	pool->current_thread_count = 0;
	pool->waiting_thread_count = 0;

	pool->queue_tasks_size = 0;
	pool->queue_tasks_front = NULL;
	pool->queue_tasks_back = NULL;

	pthread_mutex_init(&pool->mutex, NULL);
	pthread_cond_init(&pool->new_task, NULL);

	pool->terminate = false;

	return pool;
}

static void free_pool(struct thread_pool *pool) {
	pool->terminate = true;
	pthread_cond_broadcast(&pool->new_task); 

	for (int i = 0; i < pool->current_thread_count; ++i) {
		pthread_mutex_unlock(&pool->mutex);
		pthread_join(pool->threads[i], NULL);
	}
	free(pool->threads);

	pthread_cond_destroy(&pool->new_task);
	pthread_mutex_destroy(&pool->mutex);	
	free(pool);
}

int thread_pool_new(int max_thread_count, struct thread_pool **pool) {
	if (max_thread_count < 1 || max_thread_count > TPOOL_MAX_THREADS) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}
	*pool = init_pool(max_thread_count);
	return 0;
}

int thread_pool_thread_count(const struct thread_pool *pool) {
	return pool->current_thread_count;
}

int thread_pool_delete(struct thread_pool *pool) {
	pthread_mutex_lock(&pool->mutex);
	if (pool->current_thread_count != pool->waiting_thread_count || pool->queue_tasks_size) {
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_HAS_TASKS;
	}
	
	free_pool(pool);
	return 0;
}

int thread_pool_push_task(struct thread_pool *pool, struct thread_task *task) {
	pthread_mutex_lock(&pool->mutex);
	if (pool->queue_tasks_size > TPOOL_MAX_TASKS) {
		pthread_mutex_unlock(&pool->mutex);
		return TPOOL_ERR_TOO_MANY_TASKS;
	}

	push_back(pool, task);
	if (pool->waiting_thread_count == 0 && pool->current_thread_count < pool->max_thread_count) {
		pthread_create(pool->threads + pool->current_thread_count, NULL, thread_f, pool);
		pool->current_thread_count++;
	}

	pthread_cond_signal(&pool->new_task);
	pthread_mutex_unlock(&pool->mutex);
	return 0;
}

static struct thread_task *init_task(thread_task_f function, void *arg) {
	struct thread_task *task = malloc(sizeof(struct thread_task));
	task->function = function;
	task->arg = arg; 

	task->status = NOT_STARTED;

	task->pool = NULL;
	task->result = NULL;

	pthread_mutex_init(&task->mutex, NULL);
	pthread_cond_init(&task->finished, NULL);

	task->next = NULL;
	
	return task;
}

int thread_task_new(struct thread_task **task, thread_task_f function, void *arg) {
	*task = init_task(function, arg);
	return 0;
}

bool thread_task_is_finished(const struct thread_task *task) {
	return task->status == FINISHED;
}

bool thread_task_is_running(const struct thread_task *task) {
	return task->status == RUNNING;
}

int thread_task_join(struct thread_task *task, void **result) {
	pthread_mutex_lock(&task->mutex);
	if (!task->pool) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}
	while (task->status != FINISHED) {
		pthread_cond_wait(&task->finished, &task->mutex);
	}
	task->pool = NULL;
	*result = task->result;
	pthread_mutex_unlock(&task->mutex);
	return 0;
}

#ifdef NEED_TIMED_JOIN

int thread_task_timed_join(struct thread_task *task, double timeout, void **result) {
	pthread_mutex_lock(&task->mutex);
	if (!task->pool) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	if (timeout < 0 && fabs(timeout) > DBL_EPSILON) { 
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TIMEOUT;	
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	ts.tv_nsec += (long)((timeout - floor(timeout)) * 1000000000);
	ts.tv_sec += (long)timeout + ts.tv_nsec / 1000000000;
	ts.tv_nsec %= 1000000000;

	int err = 0;
	while (task->status != FINISHED && err != ETIMEDOUT) {
		err = pthread_cond_timedwait(&task->finished, &task->mutex, &ts);
	}
	if (err == ETIMEDOUT) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TIMEOUT;
	}
	*result = task->result;
	task->pool = NULL;
	pthread_mutex_unlock(&task->mutex);

	return 0;
}

#endif

int thread_task_delete(struct thread_task *task) {
	if (task->pool) {
		return TPOOL_ERR_TASK_IN_POOL;
	}
	pthread_mutex_destroy(&task->mutex);
    pthread_cond_destroy(&task->finished);

	free(task);
	return 0;
}

#ifdef NEED_DETACH

int thread_task_detach(struct thread_task *task) {
	pthread_mutex_lock(&task->mutex);
	if (!task->pool) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}
	if (task->status == FINISHED) {
		task->pool = NULL;
		pthread_mutex_unlock(&task->mutex);
		thread_task_delete(task);
	} else {
		task->status = DETACHED;
		pthread_mutex_unlock(&task->mutex);
	}
	return 0;
}

#endif
