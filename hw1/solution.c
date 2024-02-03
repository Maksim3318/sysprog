#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include "libcoro.h"

struct my_context {
	char *name; // coroutine name
	char **file_list; // list of filenames
	int file_count; // number of files
	int *file_idx; // current file index (shared for all coroutines)
	int **arr_p; // pointer to current array (to save allocated array adress)
	int *arr; // current array
	int *size_p; // pointer to array of array sizes
	int sec_start; 
	int nsec_start;
	int sec_finish;
	int nsec_finish;
	int sec_total;
	int nsec_total;
	int nsec_limit;
};

// allocates context object and initialize fields
static struct my_context *my_context_new(const char *name, char **file_list, int file_count, 
										 int *idx, int **data_p, int* size_p, int limit) {
	struct my_context *ctx = malloc(sizeof(*ctx));
	ctx->name = strdup(name);
	ctx->file_list = file_list;
	ctx->file_idx = idx;
	ctx->file_count = file_count;
	ctx->arr_p = data_p;
	ctx->size_p = size_p;
	ctx->nsec_limit = limit;
	return ctx;
}

// deallocates context and fields
static void my_context_delete(struct my_context *ctx)
{
	free(ctx->name);
	free(ctx);
}

// update context finish time
static void stop_timer(struct my_context *ctx) {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	ctx->sec_finish = time.tv_sec;
	ctx->nsec_finish = time.tv_nsec;
}

// update context start time
static void start_timer(struct my_context *ctx) {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	ctx->sec_start = time.tv_sec;
	ctx->nsec_start = time.tv_nsec;
}

// calculate total context time with nanoseconds overflow
static void calculate_time(struct my_context *ctx) {
	ctx->sec_total += ctx->sec_finish - ctx->sec_start;
	if (ctx->nsec_finish - ctx->nsec_start < 0) {
		ctx->nsec_total += 1000000000 + ctx->nsec_finish - ctx->nsec_start;
		ctx->sec_total--;
	} else {
		ctx->nsec_total += ctx->nsec_finish - ctx->nsec_start;
	}
}

// checks if the runtime for the context is exceeded
static bool is_exceed(struct my_context *ctx) {
	stop_timer(ctx);
	int current_quant = (ctx->sec_finish - ctx->sec_start) * 1000000000 + 
						(ctx->nsec_finish - ctx->nsec_start);
	return current_quant > ctx->nsec_limit ? true : false;
}

// swaps 2 int
void swap(int *a, int *b) {
	int t = *a;
	*a = *b;
	*b = t;
}

// find the partition of array
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

// quicksort implementation with coroutine yields every iteration
void quick_sort(int *array, int left, int right, struct my_context *ctx) {
	if (left < right) {
		int pi = partition(array, left, right);
		quick_sort(array, left, pi - 1, ctx);
		quick_sort(array, pi + 1, right, ctx);

		if (is_exceed(ctx)) {
			stop_timer(ctx);
			calculate_time(ctx);
			coro_yield();
			start_timer(ctx);
		}
	}
}

// coroutine function
static int coroutine_func_f(void *context) {
	struct coro *this = coro_this();
	struct my_context *ctx = context;
	start_timer(ctx);

	while (*ctx->file_idx != ctx->file_count) {
		char *filename = ctx->file_list[*ctx->file_idx];
		FILE *in = fopen(filename, "r");
		if (!in) {
			my_context_delete(ctx);
			return 1;
		}
		int size = 0;
		int cap = 100;
		ctx->arr = malloc(cap * sizeof(int));
		
		// read data from textfile
		// reallocate the array if capacity has run out
		while (fscanf(in, "%d", ctx->arr + size) == 1) {
			++size;
			if (size == cap) {
				cap *= 2;
				ctx->arr = realloc(ctx->arr, cap * sizeof(int));
			}
		}

		// shrink to fit
		cap = size;
		ctx->arr = realloc(ctx->arr, cap * sizeof(int));

		fclose(in);

		// returns the address of allocated array, size
		ctx->arr_p[*ctx->file_idx] = ctx->arr; 
		ctx->size_p[*ctx->file_idx] = size;

		// changes current file index to the next file
		(*ctx->file_idx)++;
		
		quick_sort(ctx->arr, 0, size - 1, ctx);
	}

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

// merge cnt sorted arrays
// returns index of array with minimum current value
// data - arrays, size - array sizes, idx - current indexes, cnt - arrays count
// O(cnt*N), N - maximum size of arrays
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

int main(int argc, char **argv) {
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	
	coro_sched_init();

	int file_count = argc - 3;
	int coroutine_count = atoi(argv[2]);

	if(!coroutine_count || !file_count) {
		fprintf(stderr, "Invalid command line arguments. Use the next format:\n");
		fprintf(stderr, "%s T N {files list}\n", argv[0]);
		fprintf(stderr, "T - target latency, N - coroutines count\n");
		return 1;
	}

	int *p[file_count]; // array of pointers to arrays
	int s[file_count]; // array of sizes
	int file_idx = 0;

	for (int i = 0; i < coroutine_count; ++i) {
		char name[16];
		sprintf(name, "coro_%d", i);
		coro_new(coroutine_func_f, 
				 my_context_new(name, argv + 3, file_count, &file_idx, p, s, 
				 atoi(argv[1]) * 1000 / file_count));
	}
	struct coro *c;
	while ((c = coro_sched_wait()) != NULL) {
		coro_delete(c);
	}

	int idx[file_count]; // array of current indexes for merging arrays
	for (int i = 0; i < file_count; ++i) {
		idx[i] = 0;
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
		free(p[i]);
	}

	struct timespec finish;
	clock_gettime(CLOCK_MONOTONIC, &finish);
	
	printf("total time: %ld us\n", 
			(finish.tv_sec - start.tv_sec) * 1000000 + (finish.tv_nsec - start.tv_nsec) / 1000);

	return 0;
}
