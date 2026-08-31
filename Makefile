CC      ?= gcc
BASE    := -std=c11 -O2 -Wall -Wextra
SDL     := $(shell pkg-config --cflags sdl2)
LDLIBS  := $(shell pkg-config --libs sdl2) -lm

# -MMD -MP makes the compiler emit a .d file listing every header each object
# depends on. Without it, editing a header leaves stale objects behind: struct
# layouts then disagree between translation units and the program corrupts
# memory in ways that look like bugs in the code you just wrote.
DEPFLAGS := -MMD -MP

SRC := $(wildcard src/*.c)

# Two binaries from one source tree. The parallel build defines _OPENMP, which
# every omp pragma and every omp_* call is guarded on, so the sequential build
# is a genuine single-threaded program and not just the parallel one with the
# thread count pinned to 1.
PAR_OBJ := $(SRC:src/%.c=build/par/%.o)
SEQ_OBJ := $(SRC:src/%.c=build/seq/%.o)
TASK_OBJ := $(SRC:src/%.c=build/task/%.o)
NESTED_OBJ := $(SRC:src/%.c=build/nested/%.o)
DEP     := $(PAR_OBJ:.o=.d) $(SEQ_OBJ:.o=.d)
DEP     += $(TASK_OBJ:.o=.d) $(NESTED_OBJ:.o=.d)

all: cubeview cubeview-seq

cubeview: $(PAR_OBJ)
	$(CC) $(BASE) -fopenmp -o $@ $^ $(LDLIBS)

cubeview-seq: $(SEQ_OBJ)
	$(CC) $(BASE) -o $@ $^ $(LDLIBS)

# Experimental alternatives. The original targets above remain unchanged and
# each strategy has its own object directory so builds cannot mix macros.
cubeview-task: $(TASK_OBJ)
	$(CC) $(BASE) -fopenmp -o $@ $^ $(LDLIBS)

cubeview-nested: $(NESTED_OBJ)
	$(CC) $(BASE) -fopenmp -o $@ $^ $(LDLIBS)

build/par/%.o: src/%.c | build/par
	$(CC) $(BASE) $(SDL) -fopenmp $(DEPFLAGS) -c $< -o $@

build/seq/%.o: src/%.c | build/seq
	$(CC) $(BASE) $(SDL) $(DEPFLAGS) -c $< -o $@

build/task/%.o: src/%.c | build/task
	$(CC) $(BASE) $(SDL) -fopenmp -DPARALLEL_STRATEGY_TASK $(DEPFLAGS) -c $< -o $@

build/nested/%.o: src/%.c | build/nested
	$(CC) $(BASE) $(SDL) -fopenmp -DPARALLEL_STRATEGY_NESTED $(DEPFLAGS) -c $< -o $@

build/par build/seq build/task build/nested:
	mkdir -p $@

run: cubeview
	./cubeview -n 4

bench: all
	./cubeview-seq -n 4 --view 96 --ssaa 1 --warmup 4 --bench 10
	./cubeview     -n 4 --view 96 --ssaa 1 --warmup 4 --bench 10

sweep: all
	./scripts/sweep.sh

# Test log for the report: 10 repetitions per point on both builds, mean and
# standard deviation, raw runs kept in bitacora.csv.
bitacora: all
	./scripts/bitacora.sh bitacora.csv

clean:
	rm -rf build cubeview cubeview-seq

-include $(DEP)

.PHONY: all run bench sweep bitacora clean cubeview-task cubeview-nested
