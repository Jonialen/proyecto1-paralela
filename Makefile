CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
LDLIBS  := $(shell pkg-config --libs sdl2) -lm
CFLAGS  += $(shell pkg-config --cflags sdl2)

# -MMD -MP makes the compiler emit a .d file listing every header each object
# depends on. Without it, editing a header leaves stale objects behind: struct
# layouts then disagree between translation units and the program corrupts
# memory in ways that look like bugs in the code you just wrote.
DEPFLAGS := -MMD -MP

BIN := cubeview
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN) -n 4

bench: $(BIN)
	./$(BIN) -n 4 --view 96 --ssaa 2 --warmup 4 --bench 20

sweep: $(BIN)
	./scripts/sweep.sh

clean:
	rm -f $(OBJ) $(DEP) $(BIN)

-include $(DEP)

.PHONY: all run bench sweep clean
