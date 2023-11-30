# Use Homebrew-provided clang
CC=/opt/homebrew/opt/llvm/bin/clang

# Compiler flags
CFLAGS = -I. -O2 -g -std=gnu17 -Wall -Wextra -Wfloat-equal -Wundef 
CFLAGS += -Wshadow -Wpointer-arith -Wcast-align -Wstrict-prototypes
CFLAGS += -Wwrite-strings -Waggregate-return -Wcast-qual $(CCINCLUDES)
#CFLAGS += -fsanitize=address,undefined # massive performance decrease (seems to be ASAN's printf wrapper) but catches bugs

# .h files go here
INCLUDES = linenoise.h

# .o files go here
OBJ = main.o linenoise.o

# Generate all the .o files
%.o: %.c $(INCLUDES)
	$(CC) -c -o $@ $< $(CFLAGS)

# Link and create the ./lc3vm executable
lc3vm: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

# Don't do weird stuff if there's a file called clean
.PHONY: clean

clean:
	rm *.o lc3vm
