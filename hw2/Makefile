GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all: main leaks

main: parser.c solution.c
	gcc $(GCC_FLAGS) parser.c solution.c -o main

leaks: parser.c solution.c
	gcc $(GCC_FLAGS) parser.c solution.c ../utils/heap_help/heap_help.c -ldl -rdynamic -I ../utils/heap_help/ -o leaks

.PHONY: clean
clean:
	rm -f main
	rm -f leaks
	rm -f out.txt