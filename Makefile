# Variables to override
#
# CC                C compiler
# CROSSCOMPILE      crosscompiler prefix, if any
# CFLAGS            compiler flags for compiling all C files
# LDFLAGS           linker flags for linking all binaries
# MIX_COMPILE_PATH  path to the build's ebin directory
# VIDEOCORE_DIR     path to VideoCore libraries. This defaults to "/opt/vc"

# Initialize some variables if not set
LDFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
CC ?= $(CROSSCOMPILE)-gcc

ifeq ($(MIX_COMPILE_PATH),)
  $(error MIX_COMPILE_PATH should be set by elixir_make!)
endif

PREFIX = $(MIX_COMPILE_PATH)/../priv
BUILD  = $(MIX_COMPILE_PATH)/../obj

# Check that we're on a supported build platform
ifeq ($(CROSSCOMPILE),)
    # Not crosscompiling, so check that we're on Linux.
    ifneq ($(shell uname -s),Linux)
        $(warning raspijpgs only works on Linux on a Raspberry Pi. Crosscompilation)
        $(warning is supported by defining at least $$CROSSCOMPILE. See Makefile for)
        $(warning details. If using Nerves, this should be done automatically.)
        $(warning .)
        $(warning Skipping C compilation unless targets explicitly passed to make.)
        DEFAULT_TARGETS = $(PREFIX)
    else
        # On the Raspberry Pi, the VideoCore libraries aren't in the standard
        # locations in /usr/include and /usr/lib.
        VIDEOCORE_DIR ?= /opt/vc
        ifneq ($(wildcard $(VIDEOCORE_DIR)),)
            CFLAGS += -I$(VIDEOCORE_DIR)/include
            CFLAGS += -I$(VIDEOCORE_DIR)/include/interface/vcos/pthreads
            CFLAGS += -I$(VIDEOCORE_DIR)/include/interface/vmcs_host/linux
            LDFLAGS += -L$(VIDEOCORE_DIR)/lib
        else
            $(warning raspijpgs only works on Linux on a Raspberry Pi. The VideoCore)
            $(warning libraries were not found, so assuming this is a non-Raspberry)
            $(warning build and skipping C compilation. If this is wrong, check the)
            $(warning Makefile and define VIDEOCORE_DIR.)
            DEFAULT_TARGETS = $(PREFIX)
        endif
    endif
endif

DEFAULT_TARGETS ?= $(PREFIX) $(PREFIX)/raspijpgs

ASSET_FILES = $(patsubst assets/%,$(PREFIX)/%,$(shell find assets -type f))

DEFAULT_TARGETS += $(ASSET_FILES)

# Link in all of the VideoCore libraries
LDFLAGS +=-lmmal_core -lmmal_util -lmmal_vc_client -Lvcos -lbcm_host -lm

calling_from_make:
	mix compile

all: install

install: $(BUILD) $(DEFAULT_TARGETS)

$(BUILD)/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PREFIX):
	mkdir -p $@

$(BUILD):
	mkdir -p $@

$(PREFIX)/raspijpgs: $(BUILD)/raspijpgs.o $(BUILD)/picam_camera.o $(BUILD)/picam_preview.o
	$(CC) $^ $(LDFLAGS) -o $@

$(PREFIX)/%: assets/%
	@mkdir -p $(@D)
	cp $< $@

clean:
	rm -rf $(PREFIX)/* $(BUILD)/*

.PHONY: all clean calling_from_make install
