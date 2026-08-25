# Toolchain
# Use GCC by default when no compiler was explicitly provided
# Override it with: make CC=clang
ifeq ($(origin CC),default)
	CC := gcc
endif

# Output directories
BIN_DIR := bin
OBJ_DIR := obj

# Platform
ifeq ($(OS),Windows_NT)
	EXE := .exe

	# Use the native Windows shell
	SHELL := cmd.exe
	.SHELLFLAGS := /C

	# Convert forward slashes to backslashes
	path = $(subst /,\,$(1))

	MKDIR = if not exist "$(call path,$(1))" mkdir "$(call path,$(1))"
	COPY  = copy /Y "$(call path,$(1))" "$(call path,$(2))" >NUL
	RMDIR = if exist "$(call path,$(1))" rmdir /S /Q "$(call path,$(1))"
else
	EXE :=

	MKDIR = mkdir -p "$(1)"
	COPY  = cp -f "$(1)" "$(2)"
	RMDIR = rm -rf "$(1)"
endif

# Target executables
EXECUTABLES := \
	gust_pak \
	gust_elixir \
	gust_g1t \
	gust_enc \
	gust_ebm \
	gust_gmpk

# Sources
COMMON_SRC := util.c parson.c

gust_pak_SRC    := gust_pak.c    $(COMMON_SRC)
gust_elixir_SRC := gust_elixir.c $(COMMON_SRC) miniz_tinfl.c miniz_tdef.c
gust_g1t_SRC    := gust_g1t.c    $(COMMON_SRC)
gust_enc_SRC    := gust_enc.c    $(COMMON_SRC)
gust_ebm_SRC    := gust_ebm.c    $(COMMON_SRC)
gust_gmpk_SRC   := gust_gmpk.c   $(COMMON_SRC)

# Generated files
TARGETS := $(addprefix $(BIN_DIR)/,$(addsuffix $(EXE),$(EXECUTABLES)))
ALL_SRC := $(sort $(foreach program,$(EXECUTABLES),$($(program)_SRC)))
ALL_OBJ := $(patsubst %.c,$(OBJ_DIR)/%.o,$(ALL_SRC))
ALL_DEP := $(ALL_OBJ:.o=.d)
GUST_ENC_JSON_SRC := gust_enc.json
GUST_ENC_JSON_DST := $(BIN_DIR)/gust_enc.json

# Compiler and linker flags
CPPFLAGS += \
	-UNDEBUG \
	-D_GNU_SOURCE

CFLAGS += \
	-std=c99 \
	-pipe \
	-fvisibility=hidden \
	-Wall \
	-Wextra \
	-Werror \
	-Wno-sequence-point \
	-Wno-unknown-pragmas \
	-Wno-strict-aliasing \
	-O2

LDFLAGS += -s

ifeq ($(OS),Windows_NT)
	LDFLAGS += -municode
else
	LDLIBS += -lm
endif

# Main targets
.PHONY: all clean rebuild

all: $(TARGETS) $(GUST_ENC_JSON_DST)

rebuild:
	@$(MAKE) clean
	@$(MAKE) all

# Linking
define LINK_PROGRAM

$(BIN_DIR)/$(1)$(EXE): $$(patsubst %.c,$$(OBJ_DIR)/%.o,$$($(1)_SRC)) | $(BIN_DIR)
	@echo [L] $$@
	@$$(CC) $$(LDFLAGS) -o $$@ $$^ $$(LDLIBS)

endef

$(foreach program,$(EXECUTABLES),$(eval $(call LINK_PROGRAM,$(program))))

# Compilation
$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	@echo [C] $<
	@$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

# Copy gust_enc.json
$(GUST_ENC_JSON_DST): $(GUST_ENC_JSON_SRC) | $(BIN_DIR)
	@echo [COPY] $<
	@$(call COPY,$<,$@)

# Create directories
$(BIN_DIR):
	@$(call MKDIR,$@)

$(OBJ_DIR):
	@$(call MKDIR,$@)

# Cleanup
clean:
	@echo [CLEAN]
	@$(call RMDIR,$(BIN_DIR))
	@$(call RMDIR,$(OBJ_DIR))

# Include all header dependencies
-include $(ALL_DEP)
