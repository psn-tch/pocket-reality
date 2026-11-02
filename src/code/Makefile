# Builds the Lua runner for the Jornada, and a checked PC copy for tests.
# Lua itself stays as downloaded, except for turning on its 32-bit numbers.
ROOT := $(abspath ../..)
CC := $(ROOT)/tools/sh3/bin/sh3-linux-musl-gcc
LUA_VENDOR := $(ROOT)/vendor/lua/lua-5.4.7/src
LUA := $(ROOT)/build/lua-src
SRC := pocketlua.c ../ui/ui.c
DEPS := ../ui/ui.h ../ui/font_data.h $(LUA)/luaconf.h

all: $(ROOT)/build/pocketlua $(ROOT)/build/pocketlua-host

$(LUA)/luaconf.h: $(wildcard $(LUA_VENDOR)/*)
	mkdir -p $(LUA)
	cp $(LUA_VENDOR)/*.c $(LUA_VENDOR)/*.h $(LUA)/
	sed -i 's/^#define LUA_32BITS\t0/#define LUA_32BITS\t1/' $@

$(ROOT)/build/pocketlua: $(SRC) $(DEPS)
	$(CC) -std=gnu11 -O2 -Wall -I../ui -I$(LUA) -DLUA_USE_POSIX -static -s -o $@ $(SRC) \
		$(filter-out %/lua.c %/luac.c,$(wildcard $(LUA)/*.c)) -lm

$(ROOT)/build/pocketlua-host: $(SRC) $(DEPS)
	gcc -std=gnu11 -O1 -g -Wall -fsanitize=address -I../ui -I$(LUA) -DLUA_USE_POSIX -o $@ $(SRC) \
		$(filter-out %/lua.c %/luac.c,$(wildcard $(LUA)/*.c)) -lm
