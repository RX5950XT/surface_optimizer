# ============================================================================
# Makefile for Synthetic Workloads (surface_optimizer test suite)
# ============================================================================

CXX ?= g++
CXXFLAGS = -std=c++20 -O3 -s -static -Wall -Wextra
LIBS = -lpsapi -luser32 -lkernel32 -ladvapi32

TARGETS = synth_cpu_hog.exe synth_mem_leak.exe synth_burst_app.exe synth_idle_bloat.exe

all: $(TARGETS)

synth_cpu_hog.exe: synth_cpu_hog.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

synth_mem_leak.exe: synth_mem_leak.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

synth_burst_app.exe: synth_burst_app.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS) -lgdi32

synth_idle_bloat.exe: synth_idle_bloat.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

clean:
	del /Q /F $(TARGETS) *.o 2>nul || exit 0

.PHONY: all clean
