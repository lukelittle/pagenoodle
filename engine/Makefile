# Pagenoodle engine build.
#   make            native static lib + CLI (for tests / debugging)
#   make test       run the C test-suite
#   make wasm       WebAssembly build via Emscripten -> ../web/public/engine/
SRC := $(wildcard src/*.c) vendor/miniz/miniz.c
OPT ?= -O2 -g
NODE ?= node
CFLAGS := $(OPT) -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
          -Iinclude -Isrc -Ivendor/miniz -D_POSIX_C_SOURCE=200809L \
          -DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
BUILD := build
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC))
WASM_OUT := ../web/public/engine

all: $(BUILD)/pnoodle $(BUILD)/test_engine

$(BUILD)/%.o: %.c src/*.h include/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/libnoodle.a: $(OBJ)
	ar rcs $@ $^

$(BUILD)/pnoodle: cli/pnoodle.c $(BUILD)/libnoodle.a
	$(CC) $(CFLAGS) $< $(BUILD)/libnoodle.a -lm -o $@

$(BUILD)/test_engine: tests/test_engine.c $(BUILD)/libnoodle.a
	$(CC) $(CFLAGS) $< $(BUILD)/libnoodle.a -lm -o $@

test: $(BUILD)/test_engine $(BUILD)/pnoodle
	./$(BUILD)/test_engine tests/fixtures

asan:
	$(MAKE) BUILD=build-asan OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" CC="$(CC) -fsanitize=address,undefined" test

EXPORTS := $(shell grep -oE '\bnd_[a-z_]+' include/noodle.h | sort -u | sed 's/^/_/' | paste -sd, -)
wasm:
	@mkdir -p $(WASM_OUT)
	emcc $(SRC) -O3 -Iinclude -Isrc -Ivendor/miniz \
	  -DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
	  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createNoodleEngine -sENVIRONMENT=worker,node \
	  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1048576 -sFILESYSTEM=0 \
	  -sEXPORTED_FUNCTIONS=$(EXPORTS),_malloc,_free \
	  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToNewUTF8,HEAPU8,HEAP32,HEAPF64 \
	  -o $(WASM_OUT)/noodle.mjs

clean:
	rm -rf build build-asan build-wasm

.PHONY: all test asan wasm clean

# Run the same C test-suite against the WebAssembly build under Node.js
wasm-test:
	@mkdir -p build-wasm
	emcc $(SRC) tests/test_engine.c -O2 -Iinclude -Isrc -Ivendor/miniz \
	  -DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -D_GNU_SOURCE \
	  -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576 -sNODERAWFS=1 -sENVIRONMENT=node -o build-wasm/test_engine.js
	$(NODE) build-wasm/test_engine.js tests/fixtures

.PHONY: wasm-test
