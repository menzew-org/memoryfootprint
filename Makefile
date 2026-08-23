CXX ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

TARGET := build/memftprt
SOURCES := src/main.cpp src/cli.cpp src/artifact_store.cpp src/memory_report.cpp \
	src/runtime/hotspot_nmt.cpp src/linux/cgroup_utils.cpp src/linux/proc_memory.cpp \
	src/linux/capture_linux.cpp src/linux/working_set.cpp
OBJECTS := $(patsubst %.cpp,build/obj/%.o,$(SOURCES))
TEST_TARGET := build/quiescence_target
TEST_SOURCE := tests/quiescence_target.cpp
SIGNAL_TEST_TARGET := build/signal_target
SIGNAL_TEST_SOURCE := tests/signal_target.cpp
LIFECYCLE_TEST_TARGET := build/lifecycle_target
LIFECYCLE_TEST_SOURCE := tests/lifecycle_target.cpp
WORKING_SET_TEST_TARGET := build/working_set_target
WORKING_SET_TEST_SOURCE := tests/working_set_target.cpp
MODULE_TEST_TARGET := build/module_test
MODULE_TEST_SOURCES := tests/module_test.cpp src/artifact_store.cpp src/memory_report.cpp \
	src/runtime/hotspot_nmt.cpp src/linux/cgroup_utils.cpp src/linux/proc_memory.cpp \
	src/linux/working_set.cpp
MODULE_TEST_OBJECTS := $(patsubst %.cpp,build/obj/%.o,$(MODULE_TEST_SOURCES))
DEPENDENCIES := $(sort $(OBJECTS:.o=.d) $(MODULE_TEST_OBJECTS:.o=.d))

.PHONY: all clean install test study-test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(@D)
	$(CXX) $(OBJECTS) $(LDFLAGS) -o $@

build/obj/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(TEST_TARGET): $(TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_SOURCE) $(LDFLAGS) -o $@

$(SIGNAL_TEST_TARGET): $(SIGNAL_TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SIGNAL_TEST_SOURCE) $(LDFLAGS) -o $@

$(LIFECYCLE_TEST_TARGET): $(LIFECYCLE_TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIFECYCLE_TEST_SOURCE) $(LDFLAGS) -o $@

$(WORKING_SET_TEST_TARGET): $(WORKING_SET_TEST_SOURCE)
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WORKING_SET_TEST_SOURCE) $(LDFLAGS) -o $@

$(MODULE_TEST_TARGET): $(MODULE_TEST_OBJECTS)
	@mkdir -p $(@D)
	$(CXX) $(MODULE_TEST_OBJECTS) $(LDFLAGS) -o $@

test: $(TARGET) $(TEST_TARGET) $(SIGNAL_TEST_TARGET) $(LIFECYCLE_TEST_TARGET) \
	$(WORKING_SET_TEST_TARGET) $(MODULE_TEST_TARGET)
	./$(MODULE_TEST_TARGET)
	./tests/smoke.sh
	$(MAKE) study-test

study-test:
	python3 -m unittest discover -s studies/jvm-footprint/tests -p 'test_*.py'

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/memftprt"

clean:
	rm -rf build

-include $(DEPENDENCIES)
