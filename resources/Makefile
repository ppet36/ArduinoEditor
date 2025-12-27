# resources/Makefile

SVG            := ArduinoEditor.svg
ICONSET_DIR    := ArduinoEditor.iconset
ICNS           := ArduinoEditor.icns
ICO            := ArduinoEditor.ico
LINUX_PNG      := ArduinoEditor.png

# temp SVG for renderers that don't support filters (librsvg)
RENDER_SVG     := .ArduinoEditor.render.svg

SHELL := /bin/bash

.PHONY: all iconset icns ico linuxpng clean tools

all: iconset icns ico linuxpng

tools:
	@set -e; \
	if command -v inkscape >/dev/null 2>&1; then \
	  echo "Renderer: inkscape"; \
	elif command -v rsvg-convert >/dev/null 2>&1; then \
	  echo "Renderer: rsvg-convert (filters will be stripped)"; \
	else \
	  echo "ERROR: Need 'inkscape' or 'rsvg-convert' to render SVG -> PNG." >&2; \
	  exit 1; \
	fi; \
	if ! command -v iconutil >/dev/null 2>&1; then \
	  echo "ERROR: 'iconutil' not found (expected on macOS)." >&2; \
	  exit 1; \
	fi; \
	if command -v magick >/dev/null 2>&1; then \
	  echo "ImageMagick: magick"; \
	elif command -v convert >/dev/null 2>&1; then \
	  echo "ImageMagick: convert"; \
	else \
	  echo "WARNING: No ImageMagick found ('magick' or 'convert'). .ico target will fail." >&2; \
	fi

# Create a simplified SVG without filters for librsvg (feDropShadow etc.)
$(RENDER_SVG): $(SVG)
	@set -e; \
	# remove <filter>...</filter> blocks and any filter="url(#...)" attributes
	perl -0777 -pe 's/<filter\b.*?<\/filter>\s*//sg; s/\sfilter="url\(#.*?\)"//sg' "$(SVG)" > "$(RENDER_SVG)"

define RENDER_PNG
	@set -e; \
	out="$1"; size="$2"; \
	if command -v inkscape >/dev/null 2>&1; then \
	  inkscape "$(RENDER_SVG)" --export-type=png --export-filename="$$out" -w "$$size" -h "$$size" >/dev/null; \
	else \
	  rsvg-convert -w "$$size" -h "$$size" "$(RENDER_SVG)" -o "$$out"; \
	fi
endef

iconset: tools $(SVG) $(RENDER_SVG)
	@set -e; \
	rm -rf "$(ICONSET_DIR)"; \
	mkdir -p "$(ICONSET_DIR)"; \
	\
	# Standard macOS iconset names (iconutil expects these)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_16x16.png,16)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_16x16@2x.png,32)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_32x32.png,32)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_32x32@2x.png,64)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_64x64.png,64)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_64x64@2x.png,128)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_128x128.png,128)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_128x128@2x.png,256)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_256x256.png,256)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_256x256@2x.png,512)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_512x512.png,512)
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_512x512@2x.png,1024)
	\
	# Extra size useful for Windows ICO
	$(call RENDER_PNG,$(ICONSET_DIR)/icon_48x48.png,48)
	\
	# Your "short" filenames (keep repo layout)
	cp -f "$(ICONSET_DIR)/icon_16x16.png"        "$(ICONSET_DIR)/icon_16.png"
	cp -f "$(ICONSET_DIR)/icon_32x32.png"        "$(ICONSET_DIR)/icon_32.png"
	cp -f "$(ICONSET_DIR)/icon_64x64.png"        "$(ICONSET_DIR)/icon_64.png"
	cp -f "$(ICONSET_DIR)/icon_128x128.png"      "$(ICONSET_DIR)/icon_128.png"
	cp -f "$(ICONSET_DIR)/icon_256x256.png"      "$(ICONSET_DIR)/icon_256.png"
	cp -f "$(ICONSET_DIR)/icon_512x512.png"      "$(ICONSET_DIR)/icon_512.png"
	\
	echo "OK: regenerated $(ICONSET_DIR)/"

icns: $(ICNS)

$(ICNS): iconset
	@set -e; \
	iconutil -c icns "$(ICONSET_DIR)" -o "$(ICNS)"; \
	echo "OK: $(ICNS)"

ico: $(ICO)

$(ICO): iconset
	@set -e; \
	if command -v magick >/dev/null 2>&1; then IM="magick"; else IM="convert"; fi; \
	if ! command -v $$IM >/dev/null 2>&1; then \
	  echo "ERROR: Need ImageMagick ('magick' or 'convert') to build $(ICO)" >&2; \
	  exit 1; \
	fi; \
	$$IM \
	  "$(ICONSET_DIR)/icon_16x16.png" \
	  "$(ICONSET_DIR)/icon_32x32.png" \
	  "$(ICONSET_DIR)/icon_48x48.png" \
	  "$(ICONSET_DIR)/icon_64x64.png" \
	  "$(ICONSET_DIR)/icon_128x128.png" \
	  "$(ICONSET_DIR)/icon_256x256.png" \
	  "$(ICO)"; \
	echo "OK: $(ICO)"

linuxpng: $(LINUX_PNG)

$(LINUX_PNG): iconset
	@set -e; \
	cp -f "$(ICONSET_DIR)/icon_256.png" "$(LINUX_PNG)"; \
	echo "OK: $(LINUX_PNG) (from $(ICONSET_DIR)/icon_256.png)"

clean:
	rm -rf "$(ICONSET_DIR)"
	rm -f "$(ICNS)" "$(ICO)" "$(LINUX_PNG)" "$(RENDER_SVG)"

