# Host renderer of the front panel: plays the scenes in scenes.c and writes one PNG per screen to out/.
#   make -C node/firmware/components/pm_ui/host run
# Needs only a C compiler and macOS `sips` (PPM -> PNG). On other systems the PPMs are kept.
COMP := ..
FW := ../../..
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wshadow -O1 -g
INC := -Istubs -I$(COMP)/include -I$(COMP)/src -I$(FW)/components/pm_protocol/include \
       -I$(FW)/components/pm_board/include -I$(FW)/components/pm_port/include -I$(FW)/components/pm_rdm/include
SRC := host_main.c scenes.c \
       $(COMP)/src/ui_core.c $(COMP)/src/ui_gfx.c $(COMP)/src/ui_input.c $(COMP)/src/ui_rdm.c \
       $(COMP)/src/ui_font_data.c \
       $(FW)/components/pm_protocol/src/pm_settings.c $(FW)/components/pm_protocol/src/pm_types.c
BIN := build/ui_host

all: $(BIN)

$(BIN): $(SRC) $(wildcard *.h $(COMP)/src/*.h $(COMP)/include/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) $(INC) $(SRC) -o $@

run: $(BIN)
	@mkdir -p out
	./$(BIN) out

clean:
	rm -rf build

.PHONY: all run clean
