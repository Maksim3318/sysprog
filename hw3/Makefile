GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all: main leaks

main: test.o userfs.o
	gcc $(GCC_FLAGS) test.o userfs.o -o main

test.o: test.c userfs.h
	gcc $(GCC_FLAGS) -c test.c -o test.o -I ../utils

userfs.o: userfs.c
	gcc $(GCC_FLAGS) -c userfs.c -o userfs.o

leaks: test.c userfs.c ../utils/heap_help/heap_help.c
	gcc $(GCC_FLAGS) -ldl -rdynamic -I ../utils userfs.c test.c ../utils/heap_help/heap_help.c -o leaks

.PHONY: clean
clean:
	rm -f main
	rm -f leaks
	rm -f *.o
