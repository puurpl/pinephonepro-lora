# Makefile for helium-lorawan — standalone LoRaWAN tool using RadioLib
# Platform: aarch64 Linux (PinePhone Pro / Mobian)
# No Arduino, no Portduino — uses RadioLib's generic build path

TARGET    := helium-lorawan
BUILDDIR  := build

# RadioLib source directory
RADIOLIB_DIR := $(HOME)/meshtastic-firmware/.pio/libdeps/native/RadioLib/src

# Compiler
CXX := g++

# Flags
CXXFLAGS := -std=c++17 -O2 \
            -Wall -Wextra -Wno-unused-parameter \
            -DRADIOLIB_DEBUG_BASIC=0 \
            -I$(RADIOLIB_DIR) \
            -I. \
            -MMD -MP

LDFLAGS  :=
LDLIBS   :=

# ---------------------------------------------------------------------------
# RadioLib sources (only what's needed for SX1262 + LoRaWAN)
# ---------------------------------------------------------------------------
RADIOLIB_SRCS := \
    Hal.cpp \
    Module.cpp \
    modules/SX126x/SX126x.cpp \
    modules/SX126x/SX126x_commands.cpp \
    modules/SX126x/SX126x_config.cpp \
    modules/SX126x/SX126x_LR_FHSS.cpp \
    modules/SX126x/SX1262.cpp \
    modules/SX126x/SX1261.cpp \
    modules/LLCC68/LLCC68.cpp \
    protocols/PhysicalLayer/PhysicalLayer.cpp \
    protocols/LoRaWAN/LoRaWAN.cpp \
    protocols/LoRaWAN/LoRaWANBands.cpp \
    protocols/Print/Print.cpp \
    protocols/Print/ITA2String.cpp \
    utils/CRC.cpp \
    utils/Cryptography.cpp \
    utils/FEC.cpp \
    utils/Utils.cpp

# Object files: RadioLib sources go into build/radiolib/...
RADIOLIB_OBJS := $(patsubst %.cpp,$(BUILDDIR)/radiolib/%.o,$(RADIOLIB_SRCS))

# Our own sources
APP_SRCS := main.cpp
APP_OBJS := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(APP_SRCS))

ALL_OBJS := $(RADIOLIB_OBJS) $(APP_OBJS)
DEPS     := $(ALL_OBJS:.o=.d)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# RadioLib object files
$(BUILDDIR)/radiolib/%.o: $(RADIOLIB_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Application object files
$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

-include $(DEPS)
