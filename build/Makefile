# ---------------------------------------------------------
# ArduinoEditor – portable build system (macOS + Linux)
# ---------------------------------------------------------

APP_NAME := ArduinoEditor

# Source files
SOURCE_DIR    := ../src
RESOURCE_DIR  := ../resources

UNAME_S       := $(shell uname -s)
ARCH          := $(shell uname -m)
PLATFORM      := $(UNAME_S)-$(ARCH)

BUILD_DIR     := $(PLATFORM)

# Source files (sources in ../src, objects + deps in current dir)
SRCS := $(wildcard $(SOURCE_DIR)/*.cpp)
OBJS := $(patsubst $(SOURCE_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

TARGET := $(BUILD_DIR)/$(APP_NAME)

CXX         ?= c++
WX_CONFIG   ?= wx-config
LLVM_CONFIG ?= llvm-config

DOMAIN  := ArduinoEditor
LOCDIR  := $(RESOURCE_DIR)/localization

LOCALES := cs_CZ

WX_PREFIX   := $(shell $(WX_CONFIG) --prefix 2>/dev/null)
WX_DATADIR  := $(shell $(WX_CONFIG) --datadir 2>/dev/null)
WX_VER      := $(shell $(WX_CONFIG) --version 2>/dev/null)
WX_VER_MM   := $(shell echo "$(WX_VER)" | cut -d. -f1-2)
# Prefer datadir if available, fallback to prefix/share
WX_LOCALE_ROOTS := \
  $(WX_PREFIX)/locale \
  $(WX_PREFIX)/share/locale \
  $(WX_DATADIR)/locale

WX_LOCALE_DIR := $(if $(WX_DATADIR),$(WX_DATADIR)/locale,$(WX_PREFIX)/share/locale)

POTFILE := $(LOCDIR)/$(DOMAIN).pot


# Base compiler flags
#CXXFLAGS += -std=c++17 -Wall -Wextra -Wformat -Wformat-security -g3 -MMD -MP
CXXFLAGS += -std=c++17 -Wall -Wextra -Wformat -Wformat-security -DwxNO_IMPLICIT_WXSTRING_ENCODING -g3 -MMD -MP
LDFLAGS  +=
LDLIBS   +=

# ---------------------------------------------------------
# wxWidgets detection
# ---------------------------------------------------------

WX_CXXFLAGS := $(shell $(WX_CONFIG) --cxxflags 2>/dev/null)
WX_LIBS     := $(shell $(WX_CONFIG) --libs std,aui,html,stc,net,propgrid 2>/dev/null)

ifeq ($(WX_CXXFLAGS),)
  $(error "wx-config not found. Please install wxWidgets and ensure wx-config is in your PATH.")
endif

CXXFLAGS += $(WX_CXXFLAGS)
LDLIBS   += $(WX_LIBS)

# ---------------------------------------------------------
# libclang + nlohmann::json
# ---------------------------------------------------------

ifeq ($(UNAME_S),Darwin)

    # -----------------------------------------------------
    # macOS (Homebrew-managed libraries)
    # -----------------------------------------------------

    LLVM_PREFIX ?= $(shell brew --prefix llvm 2>/dev/null)
    JSON_PREFIX ?= $(shell brew --prefix nlohmann-json 2>/dev/null)
    CURL_PREFIX ?= $(shell brew --prefix curl 2>/dev/null)

    ifeq ($(LLVM_PREFIX),)
      $(error "LLVM / libclang not found. Install using: brew install llvm")
    endif

    ifeq ($(CURL_PREFIX),)
      $(error "CURL not found. Install using: brew install curl")
    endif

    # libclang
    CXXFLAGS += -I$(LLVM_PREFIX)/include
    LDFLAGS  += -L$(LLVM_PREFIX)/lib

    # nlohmann-json (Homebrew)
    ifneq ($(JSON_PREFIX),)
        CXXFLAGS += -I$(JSON_PREFIX)/include
    else
        # fallback – try pkg-config
        CXXFLAGS += $(shell pkg-config --cflags nlohmann_json 2>/dev/null)
    endif

    LDLIBS   += -lclang

    CXXFLAGS += -I$(CURL_PREFIX)/include
    LDFLAGS  += -L$(CURL_PREFIX)/lib
    LDLIBS   += -lcurl

else ifeq ($(UNAME_S),Linux)

    # -----------------------------------------------------
    # Linux
    # -----------------------------------------------------

    ifneq ($(LLVM_CONFIG),)
      LLVM_INCLUDEDIR := $(shell $(LLVM_CONFIG) --includedir 2>/dev/null)
      LLVM_LDFLAGS    := $(shell $(LLVM_CONFIG) --ldflags    2>/dev/null)
      CLANG_CFLAGS    := -I$(LLVM_INCLUDEDIR)
      CLANG_LDFLAGS   := $(LLVM_LDFLAGS) -lclang
    endif

    $(info llvm-config (= $(LLVM_CONFIG)) CLANG_CFLAGS is $(CLANG_CFLAGS))

    ifeq ($(CLANG_CFLAGS),)
      CLANG_CFLAGS  := $(shell pkg-config --cflags libclang 2>/dev/null)
      CLANG_LDFLAGS := $(shell pkg-config --libs   libclang 2>/dev/null)
      $(info pkg-config CLANG_CFLAGS is $(CLANG_CFLAGS))
    endif

    ifeq ($(CLANG_CFLAGS),)
      $(warning Falling back to hardcoded clang includes, detection failed!)
      CLANG_CFLAGS  := -I/usr/include -I/usr/include/clang
      CLANG_LDFLAGS := -lclang
      $(info system CLANG_CFLAGS is $(CLANG_CFLAGS))
    endif

    CXXFLAGS += $(CLANG_CFLAGS)
    LDFLAGS  += $(CLANG_LDFLAGS)

    # nlohmann-json
    CXXFLAGS += $(shell pkg-config --cflags nlohmann_json 2>/dev/null)

    CXXFLAGS += $(shell pkg-config --cflags gtk+-3.0)
    LDFLAGS  += $(shell pkg-config --libs gtk+-3.0)

    # cURL
    CURL_CFLAGS  := $(shell pkg-config --cflags libcurl 2>/dev/null)
    CURL_LIBS    := $(shell pkg-config --libs   libcurl 2>/dev/null)

    CXXFLAGS += $(CURL_CFLAGS)

    ifneq ($(CURL_LIBS),)
      LDFLAGS += $(CURL_LIBS)
    else
      LDLIBS  += -lcurl
    endif
else

    $(warning Unknown platform: $(UNAME_S). You may need to manually configure paths for libclang and nlohmann::json.)

endif

# ---------------------------------------------------------
# Build targets
# ---------------------------------------------------------


all: $(TARGET) locales

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

# -----------------------
# Localization / gettext
# -----------------------

DOMAIN      := ArduinoEditor

LOCDIR_SRC  := $(RESOURCE_DIR)/localization
LOCDIR_OUT  := $(BUILD_DIR)/localization

LOCALES     := cs_CZ

POTFILE     := $(LOCDIR_SRC)/$(DOMAIN).pot

$(POTFILE): $(SRCS)
	mkdir -p $(LOCDIR_SRC)
	xgettext --language=C++ --keyword=_ \
	         --from-code=UTF-8 \
	         -o $@ $(SRCS)

# Build .mo files into the per-platform output dir:
#   $(BUILD_DIR)/localization/cs_CZ/LC_MESSAGES/ArduinoEditor.mo
$(LOCDIR_OUT)/%/LC_MESSAGES/$(DOMAIN).mo: $(LOCDIR_SRC)/%.po | $(BUILD_DIR)
	mkdir -p $(dir $@)
	msgfmt --statistics -o $@ $<

# Copy wxWidgets standard catalog into output dir.
# Supports:
#   - cs_CZ -> cs fallback
#   - wxstd.mo and wxstd-<major.minor>.mo (e.g. wxstd-3.3.mo)
$(LOCDIR_OUT)/%/LC_MESSAGES/wxstd.mo: | $(BUILD_DIR)
	@loc="$*"; lang="$${loc%%_*}"; \
	names="wxstd.mo wxstd-$(WX_VER_MM).mo"; \
	roots="$(WX_LOCALE_ROOTS)"; \
	found=""; \
	for r in $$roots; do \
	  [ -z "$$r" ] && continue; \
	  for d in "$$loc" "$$lang"; do \
	    for n in $$names; do \
	      p="$$r/$$d/LC_MESSAGES/$$n"; \
	      if [ -f "$$p" ]; then found="$$p"; break 3; fi; \
	    done; \
	  done; \
	done; \
	if [ -n "$$found" ]; then \
	  mkdir -p "$(dir $@)"; \
	  cp "$$found" "$@"; \
	  echo "WX: copied $$found -> $@"; \
	else \
	  echo "WARN: wx std catalog not found for '$$loc' (tried roots: $$roots)"; \
	fi

locales: \
  $(LOCALES:%=$(LOCDIR_OUT)/%/LC_MESSAGES/$(DOMAIN).mo) \
  $(LOCALES:%=$(LOCDIR_OUT)/%/LC_MESSAGES/wxstd.mo)

update-po: $(LOCALES:%=$(LOCDIR_SRC)/%.po)

$(LOCDIR_SRC)/%.po: $(POTFILE)
	@if [ ! -f "$@" ]; then \
	  echo "Creating new PO file $@"; \
	  cp $(POTFILE) $@; \
	else \
	  echo "Merging $@ with $(POTFILE)"; \
	  msgmerge --update --backup=none $@ $(POTFILE); \
	fi


.PHONY: all clean

# Include generated dependency files
-include $(DEPS)

