CXX      := g++
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -Werror -fPIC -fvisibility=hidden -Iinclude -Isrc
LDFLAGS  := -pthread -lm

# Enable AVX2/FMA when running on x86_64 host supporting AVX2
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
  HAS_AVX2 := $(shell grep -o avx2 /proc/cpuinfo 2>/dev/null | head -n 1)
  ifeq ($(HAS_AVX2),avx2)
    CXXFLAGS += -mavx2 -mfma
  endif
endif

BUILD_DIR := build
LIB_SRCS  := src/uso_source.cpp src/fir_resampler.cpp src/correlator.cpp src/frame_decoder.cpp src/dsp_core_api.cpp
LIB_OBJS  := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(LIB_SRCS))
TARGET    := $(BUILD_DIR)/libDspCore.so
TEST_BIN  := $(BUILD_DIR)/test_dsp_core

.PHONY: all test clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DDSPCORE_EXPORTS -c $< -o $@

$(TARGET): $(LIB_OBJS)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)

$(TEST_BIN): tests/test_dsp_core.cpp $(TARGET)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude -Isrc tests/test_dsp_core.cpp \
		-L$(BUILD_DIR) -Wl,-rpath,'$$ORIGIN' -lDspCore $(LDFLAGS) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(BUILD_DIR)
