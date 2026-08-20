CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
LDLIBS  := $(shell pkg-config --libs sdl2) -lm
CFLAGS  += $(shell pkg-config --cflags sdl2)

BIN := cubeview
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

bench: $(BIN)
	./$(BIN) --width 1280 --height 720 --ssaa 4 --bench 200

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all run bench clean
