CXX ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

TARGET := build/memftprt
SOURCES := src/main.cpp src/cli.cpp src/artifact_store.cpp src/memory_report.cpp \
	src/runtime/hotspot_nmt.cpp src/linux/proc_memory.cpp src/linux/capture_linux.cpp
TEST_TARGET := build/quiescence_target
TEST_SOURCE := tests/quiescence_target.cpp
SIGNAL_TEST_TARGET := build/signal_target
SIGNAL_TEST_SOURCE := tests/signal_target.cpp
LIFECYCLE_TEST_TARGET := build/lifecycle_target
LIFECYCLE_TEST_SOURCE := tests/lifecycle_target.cpp
MODULE_TEST_TARGET := build/module_test
MODULE_TEST_SOURCES := tests/module_test.cpp src/memory_report.cpp src/runtime/hotspot_nmt.cpp

.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(TEST_TARGET): $(TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_SOURCE) $(LDFLAGS) -o $@

$(SIGNAL_TEST_TARGET): $(SIGNAL_TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SIGNAL_TEST_SOURCE) $(LDFLAGS) -o $@

$(LIFECYCLE_TEST_TARGET): $(LIFECYCLE_TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIFECYCLE_TEST_SOURCE) $(LDFLAGS) -o $@

$(MODULE_TEST_TARGET): $(MODULE_TEST_SOURCES)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(MODULE_TEST_SOURCES) $(LDFLAGS) -o $@

test: $(TARGET) $(TEST_TARGET) $(SIGNAL_TEST_TARGET) $(LIFECYCLE_TEST_TARGET) $(MODULE_TEST_TARGET)
	./$(MODULE_TEST_TARGET)
	./tests/smoke.sh

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/memftprt"

clean:
	rm -rf build
