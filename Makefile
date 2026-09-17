# treenity - portable Makefile (GNU Make on Linux/macOS/MinGW).
#
# Targets:
#   make            build the core static library
#   make test       build and run the unit + scenario tests
#   make example    build the desktop example
#   make run        build and run the desktop example
#   make clean      remove build artefacts
#
# On Windows the CMake build (see CMakeLists.txt) is recommended; this Makefile
# also works with MinGW make.

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
CPPFLAGS += -Iinclude -Isrc -Isim
LDLIBS  += -lm

ifeq ($(OS),Windows_NT)
  MKDIR = mkdir
  RMDIR = rmdir /s /q
else
  MKDIR = mkdir -p
  RMDIR = rm -rf
endif

BUILD   := build

# Source files are discovered through vpath so object files can live flat in
# $(BUILD), which keeps directory creation simple on every platform.
vpath %.c src/core src/mac src/link src/routing src/api sim

CORE_NAMES := ringbuf timer frame dupcache airtime neighbor beacon routing treenet
CORE_OBJ   := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE_NAMES)))
SIM_OBJ    := $(BUILD)/sim.o

TEST_SRC := tests/unit/test_main.c tests/unit/test_core.c tests/unit/test_mesh.c tests/unit/test_corrupt.c

LIB      := $(BUILD)/libtreenet.a

.PHONY: all test example run fuzz repeaters clean

all: $(LIB)

$(BUILD):
	$(MKDIR) $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/treenet_tests: $(TEST_SRC) $(SIM_OBJ) $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests/unit $(TEST_SRC) $(SIM_OBJ) $(LIB) -o $@ $(LDLIBS)

test: $(BUILD)/treenet_tests
	$(BUILD)/treenet_tests

$(BUILD)/treenet_example: examples/desktop_mesh/main.c $(SIM_OBJ) $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) examples/desktop_mesh/main.c $(SIM_OBJ) $(LIB) -o $@ $(LDLIBS)

example: $(BUILD)/treenet_example

run: $(BUILD)/treenet_example
	$(BUILD)/treenet_example

$(BUILD)/fuzz: tests/fuzz/fuzz_frame.c $(SIM_OBJ) $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/fuzz/fuzz_frame.c $(SIM_OBJ) $(LIB) -o $@ $(LDLIBS)

fuzz: $(BUILD)/fuzz
	$(BUILD)/fuzz 200000

$(BUILD)/treenet_repeaters: tests/sim/test_repeaters.c $(SIM_OBJ) $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/sim/test_repeaters.c $(SIM_OBJ) $(LIB) -o $@ $(LDLIBS)

repeaters: $(BUILD)/treenet_repeaters
	$(BUILD)/treenet_repeaters

clean:
	$(RMDIR) $(BUILD)
