CXX ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

TARGET := build/memftprt
SOURCE := src/memftprt.cpp
TEST_TARGET := build/quiescence_target
TEST_SOURCE := tests/quiescence_target.cpp
SIGNAL_TEST_TARGET := build/signal_target
SIGNAL_TEST_SOURCE := tests/signal_target.cpp
LIFECYCLE_TEST_TARGET := build/lifecycle_target
LIFECYCLE_TEST_SOURCE := tests/lifecycle_target.cpp

.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCE) $(LDFLAGS) -o $@

$(TEST_TARGET): $(TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_SOURCE) $(LDFLAGS) -o $@

$(SIGNAL_TEST_TARGET): $(SIGNAL_TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SIGNAL_TEST_SOURCE) $(LDFLAGS) -o $@

$(LIFECYCLE_TEST_TARGET): $(LIFECYCLE_TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIFECYCLE_TEST_SOURCE) $(LDFLAGS) -o $@

test: $(TARGET) $(TEST_TARGET) $(SIGNAL_TEST_TARGET) $(LIFECYCLE_TEST_TARGET)
	./tests/smoke.sh

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/memftprt"

clean:
	rm -rf build
