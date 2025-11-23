# Makefile — final version with recursive sources, preserved object paths,
# and include flags set early so headers like "clouds/dsp/mu_law.h" resolve.

# Project
TARGET = guitar_pedal

CPP_STANDARD = -std=gnu++20

# Features
USE_FATFS = 1
USE_DAISYSP_LGPL = 1

# Paths
DEPS_DIR = dependencies
DAISYSP_DIR ?= $(DEPS_DIR)/DaisySP
LIBDAISY_DIR ?= $(DEPS_DIR)/libDaisy
BUILD_DIR = build

# Included as system instead of regular includes to avoid warnings from 3rd party deps
C_INCLUDES += -isystem ./dependencies/q/q/q_lib/include
C_INCLUDES += -isystem ./dependencies/q/infra/include
C_INCLUDES += -isystem ./dependencies/stmlib

# Sources (GNU Make >= 4 supports ** in wildcard)
CPP_SOURCES = $(wildcard *.cpp) \
              $(wildcard Effect-Modules/*.cpp) \
              $(wildcard Hardware-Modules/*.cpp) \
              $(wildcard Util/*.cpp) \
              $(wildcard clouds/**/*.cpp)

# Preserve directory structure under BUILD_DIR to avoid name collisions
OBJECTS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CPP_SOURCES))

# Debug printing
$(info CPP_SOURCES = $(CPP_SOURCES))
$(info OBJECTS     = $(OBJECTS))

# Boot options
APP_TYPE = BOOT_SRAM

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
