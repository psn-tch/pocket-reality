# Builds Wolfenstein 3D for the Jornada, from the Wolf4SDL sources in vendor/.
# GAME picks the data the build reads: shareware v1.4 by default.
ROOT := $(abspath ../..)
WOLF := $(ROOT)/vendor/wolf4sdl
TOOLS := $(ROOT)/tools/sh3-cxx/bin/sh3-linux-musl-
GAME ?= -DVERSIONALREADYCHOSEN -DCARMACIZED -DUPLOAD
OPT ?= -O2

SRC := id_ca id_in id_pm id_us_1 id_vh id_vl signon wl_act1 wl_act2 wl_agent wl_atmos \
	wl_cloudsky wl_debug wl_dir3dspr wl_draw wl_floorceiling wl_game wl_inter wl_main \
	wl_menu wl_parallax wl_play wl_shade wl_state wl_text states

FLAGS := $(GAME) -I$(CURDIR)/shim -I$(WOLF) -D_DEFAULT_SOURCE -w -fno-exceptions -fno-rtti \
	-fomit-frame-pointer -fno-strict-aliasing -fpermissive

all: $(ROOT)/build/pwolf

host: $(ROOT)/build/pwolf-host

$(ROOT)/build/pwolf: $(addprefix $(ROOT)/build/wolf/,$(addsuffix .o,$(SRC))) \
		$(ROOT)/build/wolf/sd_silent.o $(ROOT)/build/wolf/pocket_wolf.o
	$(TOOLS)g++ -static -o $@.debug $^
	$(TOOLS)strip -o $@ $@.debug

$(ROOT)/build/wolf/%.o: $(WOLF)/%.cpp
	@mkdir -p $(dir $@)
	$(TOOLS)g++ $(OPT) $(FLAGS) -c -o $@ $<

$(ROOT)/build/wolf/sd_silent.o: sd_silent.cpp
	@mkdir -p $(dir $@)
	$(TOOLS)g++ $(OPT) $(FLAGS) -c -o $@ $<

$(ROOT)/build/wolf/pocket_wolf.o: pocket_wolf.c shim/SDL.h
	@mkdir -p $(dir $@)
	$(TOOLS)gcc $(OPT) -Wall -Wextra -Ishim -c -o $@ $<

$(ROOT)/build/wolf-host/%.o: $(WOLF)/%.cpp
	@mkdir -p $(dir $@)
	g++ -O1 -g -fsanitize=address,undefined $(FLAGS) -c -o $@ $<

$(ROOT)/build/wolf-host/sd_silent.o: sd_silent.cpp
	@mkdir -p $(dir $@)
	g++ -O1 -g -fsanitize=address,undefined $(FLAGS) -c -o $@ $<

$(ROOT)/build/wolf-host/pocket_wolf.o: pocket_wolf.c shim/SDL.h
	@mkdir -p $(dir $@)
	gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -Ishim -c -o $@ $<

$(ROOT)/build/pwolf-host: $(addprefix $(ROOT)/build/wolf-host/,$(addsuffix .o,$(SRC))) \
		$(ROOT)/build/wolf-host/sd_silent.o $(ROOT)/build/wolf-host/pocket_wolf.o
	g++ -fsanitize=address,undefined -o $@ $^
