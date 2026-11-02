# Builds the desktop and startup screen for the Jornada, and checked PC copies.
ROOT := $(abspath ../..)
CC := $(ROOT)/tools/sh3/bin/sh3-linux-musl-gcc
UI := ../ui/ui.c
DEPS := ../ui/ui.h ../ui/font_data.h $(UI)
TARGET_FLAGS := -std=gnu11 -O2 -Wall -Wextra -I../ui -static -s
HOST_FLAGS := -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I../ui

all: $(ROOT)/build/finder $(ROOT)/build/pocketsplash $(ROOT)/build/finder-host

$(ROOT)/build/finder: finder.c $(DEPS)
	$(CC) $(TARGET_FLAGS) -o $@ finder.c $(UI)

$(ROOT)/build/pocketsplash: splash.c $(DEPS)
	$(CC) $(TARGET_FLAGS) -o $@ splash.c $(UI)

$(ROOT)/build/finder-host: finder.c $(DEPS)
	gcc $(HOST_FLAGS) -o $@ finder.c $(UI)
