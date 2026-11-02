# Builds Pocket Write for the Jornada, and a checked PC copy for tests.
ROOT := $(abspath ../..)
CC := $(ROOT)/tools/sh3/bin/sh3-linux-musl-gcc
SRC := write.c lock.c ../third_party/monocypher.c ../ui/ui.c
DEPS := lock.h ../third_party/monocypher.h ../ui/ui.h ../ui/font_data.h
INC := -I../ui -I../third_party

$(ROOT)/build/write: $(SRC) $(DEPS)
	$(CC) -std=gnu11 -O2 -Wall -Wextra $(INC) -static -s -o $@ $(SRC)

$(ROOT)/build/write-host: $(SRC) $(DEPS)
	gcc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined $(INC) -o $@ $(SRC)

all: $(ROOT)/build/write $(ROOT)/build/write-host
