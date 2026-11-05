# KingCobra IDE (kcide) - pure C11, no external libraries.
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-misleading-indentation -Wno-format-truncation
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:src/%.c=build/%.o)
BIN     := kcide

all: $(BIN)
$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)
build/%.o: src/%.c src/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -rf build $(BIN)
.PHONY: all clean

# unit tests + fuzzers for the editor core (document model, undo/redo, highlighter)
test: build/test_core
	./build/test_core
	@python3 tests/test_debug.py ../kingcobra 2>/dev/null || { [ $$? -eq 127 ] && echo "skip  debugger protocol (python3 not found)"; }
build/test_core: tests/test_core.c src/doc.c src/syntax.c src/lang.c src/vt.c src/util.c src/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -o $@ tests/test_core.c src/doc.c src/syntax.c src/lang.c src/vt.c src/util.c
.PHONY: test
