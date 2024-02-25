GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -ldl -rdynamic -I ../utils/heap_help/

all: parser.c solution.c
	gcc $(GCC_FLAGS) parser.c solution.c ../utils/heap_help/heap_help.c -o main

.PHONY: clean
clean:
	rm main
	rm out.txt