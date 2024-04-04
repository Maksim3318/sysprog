GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all: test.o thread_pool.o
	gcc $(GCC_FLAGS) -g test.o thread_pool.o -o main

test.o: test.c thread_pool.h
	gcc $(GCC_FLAGS) -g -c test.c -o test.o -I ../utils

thread_pool.o: thread_pool.c thread_pool.h
	gcc $(GCC_FLAGS) -g -c thread_pool.c -o thread_pool.o

leaks: test.c thread_pool.c thread_pool.h ../utils/heap_help/heap_help.c
	gcc $(GCC_FLAGS) -ldl -rdynamic -I ../utils thread_pool.c test.c ../utils/heap_help/heap_help.c -o leaks

.PHONY: clean
clean:
	rm -f main
	rm -f leaks
	rm -f *.o
