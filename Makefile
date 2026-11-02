
# Reliable-UDP build, unit-test, and experiment-runner targets
# for GCC / MSYS2 UCRT64.
#
# Run from the repository root:
#   make all
#   make test
#   make experiment-runner
#   make clean

CC       = gcc
AR       = ar
BUILD    := build
OBJDIR   := $(BUILD)/obj
BINDIR   := $(BUILD)/tests
TARGET   := $(BUILD)/libreliable_udp.a

CPPFLAGS += -Isrc -Isrc/driver -D_POSIX_C_SOURCE=200809L
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
THREADS  := -pthread
LDLIBS   += -lm

ifeq ($(OS),Windows_NT)
  EXE := .exe
  LDLIBS += -lws2_32
else
  EXE :=
endif

# Define this after EXE so the executable suffix is correct on Windows.
EXPERIMENT_RUNNER := $(BUILD)/experiment_runner$(EXE)

SRC := \
  src/arq/arq.c \
  src/channel/channel_emulator.c \
  src/driver/driver.c \
  src/file/file_manager.c \
  src/file/sha256.c \
  src/monitor/monitor.c \
  src/packet/packet_checksum.c \
  src/packet/packet_creator.c \
  src/packet/packet_dewrapper.c \
  src/packet/packet_serializer.c \
  src/packet/packet_verifier.c \
  src/reconstruct/reconstructor.c \
  src/rto/rto.c \
  src/timer/timer.c \
  src/transport/transport.c

OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC))

.PHONY: all test clean list-tests experiment-runner

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREADS) -c $< -o $@

$(BINDIR):
	@mkdir -p $@

$(BINDIR)/test_sha256$(EXE): tests/unit/common/test_sha256.c src/file/sha256.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BINDIR)/test_file_manager$(EXE): tests/unit/file/test_file_manager.c src/file/file_manager.c src/file/sha256.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BINDIR)/test_rto$(EXE): tests/unit/rto/test_rto.c src/rto/rto.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BINDIR)/test_monitor$(EXE): tests/unit/monitor/test_monitor.c src/monitor/monitor.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREADS) $^ $(LDLIBS) -o $@

$(BINDIR)/test_channel_emulator$(EXE): tests/unit/channel/test_channel_emulator.c src/channel/channel_emulator.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREADS) $^ $(LDLIBS) -o $@

$(BINDIR)/test_reconstructor$(EXE): tests/unit/reconstruct/test_reconstructor.c src/reconstruct/reconstructor.c src/file/sha256.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BINDIR)/test_arq$(EXE): tests/unit/arq/test_arq.c src/arq/arq.c src/packet/packet_creator.c src/packet/packet_dewrapper.c src/packet/packet_checksum.c src/packet/packet_serializer.c src/packet/packet_verifier.c src/rto/rto.c src/timer/timer.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREADS) $^ $(LDLIBS) -o $@

$(BINDIR)/packet_module_black_box_test$(EXE): tests/unit/packet/packet_module_black_box_test.c src/packet/packet_checksum.c src/packet/packet_creator.c src/packet/packet_dewrapper.c src/packet/packet_serializer.c src/packet/packet_verifier.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BINDIR)/test_driver$(EXE): tests/unit/driver/test_driver.c $(SRC) | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREADS) $^ $(LDLIBS) -o $@

# Standalone harness for repeatable transfer experiments.
$(EXPERIMENT_RUNNER): experiments/experiment_runner.c $(SRC) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREADS) $^ $(LDLIBS) -o $@

$(BUILD):
	@mkdir -p $@

experiment-runner: $(EXPERIMENT_RUNNER)

$(BINDIR)/transport_module_black_box_test$(EXE): tests/unit/transport/transport_module_black_box_test.c tests/unit/transport/support/channel_emulator_stub.c src/transport/transport.c | $(BINDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREADS) $^ $(LDLIBS) -o $@

TESTS := \
  $(BINDIR)/test_sha256$(EXE) \
  $(BINDIR)/test_file_manager$(EXE) \
  $(BINDIR)/test_rto$(EXE) \
  $(BINDIR)/test_monitor$(EXE) \
  $(BINDIR)/test_channel_emulator$(EXE) \
  $(BINDIR)/test_reconstructor$(EXE) \
  $(BINDIR)/test_arq$(EXE) \
  $(BINDIR)/packet_module_black_box_test$(EXE) \
  $(BINDIR)/test_driver$(EXE) \
  $(BINDIR)/transport_module_black_box_test$(EXE)

list-tests:
	@printf '%s\n' $(TESTS)

test: $(TESTS)
	@set -e; \
	for t in $(TESTS); do \
	  echo ""; echo "=== RUN $$t ==="; \
	  "$$t"; \
	done
	@echo ""; echo "ALL UNIT TESTS PASSED"

DEMO_TARGET := $(BUILD)/reliable_udp_demo$(EXE)

$(DEMO_TARGET): demo/demo.c $(SRC) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREADS) $^ $(LDLIBS) -static -o $@

.PHONY: demo
demo: $(DEMO_TARGET)

clean:
	rm -rf $(BUILD)
