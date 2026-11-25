# Renders the knob's screens on a PC: the real marine_ui and knob_logic with LVGL, simulated data (like the automatic demo).
# Needs gcc, make, python3 with Pillow 10.1 or newer, the firmware's sdkconfig and managed_components (build the firmware once).
#   make -C tools/ui-render CAPTION="v0.1.0"      writes build/img/rendering-*.png
#   make -C tools/ui-render manual                writes build/m/img/manual/*.png
# The screens are renderings, not photos: say so where they are shown.
REPO ?= ../..
LVGL ?= $(REPO)/managed_components/lvgl__lvgl
BUILD ?= build
CAPTION ?= development build
UI = $(REPO)/components/marine_ui
LOGIC = $(REPO)/components/knob_logic
CFLAGS = -O2 -w -Istub -I$(LVGL) -I$(UI)/include -I$(LOGIC) -DLV_KCONFIG_PRESENT -DLV_CONF_SKIP=1 -include $(BUILD)/sdkconfig_lv.h
SRC = ui_render.c $(shell find $(LVGL)/src $(UI) $(LOGIC) -name '*.c')
OBJ = $(addprefix $(BUILD)/,$(subst ../,,$(SRC:.c=.o)))

images: $(BUILD)/ui_render
	mkdir -p $(BUILD)/out && cd $(BUILD) && ./ui_render
	python3 render.py $(BUILD) "$(CAPTION)"

# The manual's pictures, for docs/img/manual
manual: $(BUILD)/ui_render
	mkdir -p $(BUILD)/m/out && cd $(BUILD)/m && ../ui_render manual
	python3 render.py $(BUILD)/m manual

$(BUILD)/ui_render: $(OBJ)
	$(CC) -o $@ $^ -lm

$(BUILD)/sdkconfig_lv.h: $(REPO)/sdkconfig Makefile
	mkdir -p $(BUILD)
	grep '^CONFIG_LV_' $< | sed -E 's/[[:space:]]+$$//; s/=y$$/=1/; s/^([A-Z0-9_]+)=(.*)$$/#define \1 \2/' > $@

define rule
$(BUILD)/$(subst ../,,$(1:.c=.o)): $(1) $(BUILD)/sdkconfig_lv.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) -c -o $$@ $$<
endef
$(foreach f,$(SRC),$(eval $(call rule,$(f))))

.PHONY: images manual
