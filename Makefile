CXX = C:/msys64/ucrt64/bin/g++.exe
CXXFLAGS = -std=c++20 -O3 -flto -municode -Wall -Wextra -Iinclude
LDFLAGS = -static -s -lpowrprof -lpsapi -ladvapi32 -luser32 -lshell32 -lgdi32 -lkernel32 -lwtsapi32 -lpdh

SRCDIR = src
INCDIR = include
BUILDDIR = build

SOURCES = $(wildcard $(SRCDIR)/core/*.cpp) $(wildcard $(SRCDIR)/optimizer/*.cpp) $(wildcard $(SRCDIR)/telemetry/*.cpp) $(SRCDIR)/main.cpp
OBJECTS = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SOURCES))
TARGET = surface_optimizer.exe

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/core/%.o: $(SRCDIR)/core/%.cpp
	@mkdir -p $(BUILDDIR)/core
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/optimizer/%.o: $(SRCDIR)/optimizer/%.cpp
	@mkdir -p $(BUILDDIR)/optimizer
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/telemetry/%.o: $(SRCDIR)/telemetry/%.cpp
	@mkdir -p $(BUILDDIR)/telemetry
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILDDIR) $(TARGET)