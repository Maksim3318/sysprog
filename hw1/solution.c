#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include "libcoro.h"
#include "heap_help.h"

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
	/** ADD HERE YOUR OWN MEMBERS, SUCH AS FILE NAME, WORK TIME, ... */
};

static struct my_context *my_context_new(const char *name, char *filename, int **data_p, int* size_p)
{
	struct my_context *ctx = malloc(sizeof(*ctx));
	ctx->name = strdup(name);
	ctx->filename = filename;
	ctx->arr_p = data_p;
	ctx->size_p = size_p;
	return ctx;
}

static void my_context_delete(struct my_context *ctx)
{
	free(ctx->name);
	free(ctx);
}

// function to swap elements
void swap(int *a, int *b) {
  int t = *a;
  *a = *b;
  *b = t;
}

// function to find the partition position
int partition(int *array, int low, int high) {
  int pivot = array[high];
  int i = (low - 1);

  for (int j = low; j < high; j++) {
    if (array[j] <= pivot) {
      i++;
      swap(&array[i], &array[j]);
    }
  }
  swap(&array[i + 1], &array[high]);
  return (i + 1);
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

void quickSort(int *array, int low, int high, struct my_context *ctx) {
  if (low < high) {
    int pi = partition(array, low, high);
    quickSort(array, low, pi - 1, ctx);
    quickSort(array, pi + 1, high, ctx);

	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	ctx->sec_finish = time.tv_sec;
	ctx->nsec_finish = time.tv_nsec;
	
	calculate_time(ctx);
	
	coro_yield();
	
	clock_gettime(CLOCK_MONOTONIC, &time);
	ctx->sec_start = time.tv_sec;
	ctx->nsec_start = time.tv_nsec;	
  }
}

static int coroutine_func_f(void *context)
{
	struct coro *this = coro_this();
	struct my_context *ctx = context;
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	ctx->sec_start = start.tv_sec;
	ctx->nsec_start = start.tv_nsec;
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
	quickSort(ctx->arr, 0, size, ctx);
	*ctx->arr_p = ctx->arr; 
	*ctx->size_p = size;
	
	struct timespec finish;
	clock_gettime(CLOCK_MONOTONIC, &finish);
	ctx->sec_finish = finish.tv_sec;
	ctx->nsec_finish = finish.tv_nsec;
	
	calculate_time(ctx);

	printf("%s info:\nswitch count %lld\nworked %dms\n\n",
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
	coro_sched_init();
	int *p[argc - 1];
	int **data[argc - 1];
	int s[argc - 1];
	int *size[argc - 1];
	for (int i = 0; i < argc - 1; ++i) {

		char name[16];
		sprintf(name, "coro_%d", i);
		data[i] = &p[i]; 
		size[i] = &s[i];
		coro_new(coroutine_func_f, my_context_new(name, argv[i + 1], data[i], size[i]));
	}
	struct coro *c;
	while ((c = coro_sched_wait()) != NULL) {
		coro_delete(c);
	}
	int idx[argc - 1];
	int total_size = 0;
	for (int i = 0; i < argc - 1; ++i) {
		p[i] = *data[i];
		s[i] = *size[i];
		idx[i] = 0;
		total_size += s[i];
	}

	FILE *out = fopen("out.txt", "w");

	int min_idx = 0;
	
	while(min_idx != -1) {
		min_idx = merge(p, s, idx, argc - 1);
		if (min_idx != -1) {
			fprintf(out, "%d ", p[min_idx][idx[min_idx]]);
			idx[min_idx] += 1;
		}
	}
	fclose(out);
	
	for (int i = 0; i < argc - 1; ++i) {
		free(*data[i]);
	}

	return 0;
}
