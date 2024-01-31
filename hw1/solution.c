#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include "libcoro.h"

struct my_context {
	char *name;
	char *filename;
	int **arr_p;
	int *arr;
	int *size_p;
	int sec_start;
	int nsec_start;
	int sec_finish;
	int nsec_finish;
	int sec_total;
	int nsec_total;
	int nsec_limit;
};

static struct my_context *my_context_new(const char *name, char *filename, int **data_p, int* size_p, int limit)
{
	struct my_context *ctx = malloc(sizeof(*ctx));
	ctx->name = strdup(name);
	ctx->filename = filename;
	ctx->arr_p = data_p;
	ctx->size_p = size_p;
	ctx->nsec_limit = limit;
	return ctx;
}

static void my_context_delete(struct my_context *ctx)
{
	free(ctx->name);
	free(ctx);
}

static void stop_timer(struct my_context *ctx) {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	ctx->sec_finish = time.tv_sec;
	ctx->nsec_finish = time.tv_nsec;
}

static void start_timer(struct my_context *ctx) {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	ctx->sec_start = time.tv_sec;
	ctx->nsec_start = time.tv_nsec;
}

static void calculate_time(struct my_context *ctx) {
	ctx->sec_total += ctx->sec_finish - ctx->sec_start;
	if (ctx->nsec_finish - ctx->nsec_start < 0) {
		ctx->nsec_total += 1000000000 + ctx->nsec_finish - ctx->nsec_start;
		ctx->sec_total--;
	} else {
		ctx->nsec_total += ctx->nsec_finish - ctx->nsec_start;
	}
}

static bool is_exceed(struct my_context *ctx) {
	stop_timer(ctx);
	int current_quant = (ctx->sec_finish - ctx->sec_start) * 1000000000 + 
						(ctx->nsec_finish - ctx->nsec_start);
	return current_quant > ctx->nsec_limit ? true : false;
}

void swap(int *a, int *b) {
  int t = *a;
  *a = *b;
  *b = t;
}

int partition(int *array, int left, int right) {
  int pivot = array[right];
  int i = (left - 1);

  for (int j = left; j < right; j++) {
    if (array[j] <= pivot) {
      i++;
      swap(&array[i], &array[j]);
    }
  }
  swap(&array[i + 1], &array[right]);
  return (i + 1);
}

void quick_sort(int *array, int low, int high, struct my_context *ctx) {
  if (low < high) {
    int pi = partition(array, low, high);
    quick_sort(array, low, pi - 1, ctx);
    quick_sort(array, pi + 1, high, ctx);

	if (is_exceed(ctx)) {
		stop_timer(ctx);
		calculate_time(ctx);
		coro_yield();
		start_timer(ctx);
	}
  }
}

static int coroutine_func_f(void *context)
{
	struct coro *this = coro_this();
	struct my_context *ctx = context;
	start_timer(ctx);
	char *filename = ctx->filename;
	FILE *in = fopen(filename, "r");
	if (!in) {
		my_context_delete(ctx);
		return 1;
	}
	int size = 0;
	int cap = 100;
	ctx->arr = malloc(cap * sizeof(int));

	while (fscanf(in, "%d", ctx->arr + size) == 1) {
		++size;
		if (size == cap) {
			cap *= 2;
			ctx->arr = realloc(ctx->arr, cap * sizeof(int));
		}
	}
	cap = size;
	ctx->arr = realloc(ctx->arr, cap * sizeof(int));

	fclose(in);
	quick_sort(ctx->arr, 0, size - 1, ctx);
	*ctx->arr_p = ctx->arr; 
	*ctx->size_p = size;
	
	stop_timer(ctx);
	calculate_time(ctx);

	printf("%s info:\nswitch count %lld\nworked %d us\n\n",
	 	ctx->name,
	    coro_switch_count(this),
		ctx->sec_total * 1000000 + ctx->nsec_total / 1000
	);
	my_context_delete(ctx);
	return 0;
}

int merge(int **data, int *size, int *idx, int cnt) {
	int min_idx = -1;
	int curr_min = INT_MAX;
	for (int i = 0; i < cnt; ++i) {
		if ((size[i] > idx[i]) && (data[i][idx[i]] < curr_min)) {
			curr_min = data[i][idx[i]];
			min_idx = i;
		}
	}
	return min_idx;
}

int main(int argc, char **argv)
{
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	int file_count = argc - 2;
	coro_sched_init();
	int *p[file_count];
	int **data[file_count];
	int s[file_count];
	int *size[file_count];
	for (int i = 0; i < file_count; ++i) {

		char name[16];
		sprintf(name, "coro_%d", i);
		data[i] = &p[i]; 
		size[i] = &s[i];
		coro_new(coroutine_func_f, 
				 my_context_new(name, argv[i + 2], data[i], size[i], 
				 atoi(argv[1]) * 1000 / file_count));
	}
	struct coro *c;
	while ((c = coro_sched_wait()) != NULL) {
		coro_delete(c);
	}
	int idx[file_count];
	int total_size = 0;
	for (int i = 0; i < file_count; ++i) {
		p[i] = *data[i];
		s[i] = *size[i];
		idx[i] = 0;
		total_size += s[i];
	}

	FILE *out = fopen("out.txt", "w");

	int min_idx = 0;
	
	while(min_idx != -1) {
		min_idx = merge(p, s, idx, file_count);
		if (min_idx != -1) {
			fprintf(out, "%d ", p[min_idx][idx[min_idx]]);
			idx[min_idx] += 1;
		}
	}
	fclose(out);
	
	for (int i = 0; i < file_count; ++i) {
		free(*data[i]);
	}

	struct timespec finish;
	clock_gettime(CLOCK_MONOTONIC, &finish);
	
	printf("total time: %ld us\n", 
			(finish.tv_sec - start.tv_sec) * 1000000 + (finish.tv_nsec - start.tv_nsec) / 1000);

	return 0;
}
