CC ?= gcc

# Baked into the binary and shown in the startup banner. CI overrides this
# with the release tag (make VERSION=v1.2.3); local builds fall back to
# `git describe`, so a dev build identifies itself as e.g.
# "v0.1.1-3-gabc1234-dirty" rather than claiming to be a release.
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

BIN_DIR   := bin
BUILD_DIR := build
SRC_DIR   := src
INC_DIR   := include
TEST_DIR  := tests

TARGET := $(BIN_DIR)/fssh

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

CFLAGS  ?= -Wall -g
CFLAGS  += -I$(INC_DIR) -DFLASSH_VERSION=\"$(VERSION)\" -MMD -MP
LDLIBS  := -lssh -lpthread -lm

# Homebrew keeps libssh outside the default search paths, and Apple silicon
# and Intel Macs use different prefixes.
ifeq ($(shell uname -s),Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
    CFLAGS  += -I$(BREW_PREFIX)/include
    LDFLAGS += -L$(BREW_PREFIX)/lib
endif

.PHONY: all clean test run install

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

# Unit tests cover the pure helpers (history parsing, prompt/echo heuristics).
# Each test includes the .c under test directly so it can reach static
# functions without widening the public headers.
TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/%,$(TEST_SRCS))

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "--- $$t"; ./$$t || exit 1; done
	@echo "All tests passed."

# Links every object except main.o (its own main()) and the one whose .c the
# test already includes verbatim (which would duplicate every symbol).
$(BUILD_DIR)/test_%: $(TEST_DIR)/test_%.c $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $< \
	    $(filter-out $(BUILD_DIR)/main.o $(BUILD_DIR)/$*.o,$(OBJS)) \
	    -o $@ $(LDFLAGS) $(LDLIBS)

install: $(TARGET)
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/fssh

run: $(TARGET)
	@$(TARGET) $(ARGS)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)
