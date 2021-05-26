CFLAGS=-O2 -Wall -Wextra -Werror -std=gnu18 $(shell pkg-config --cflags libpulse)
LDFLAGS=$(shell pkg-config --libs libpulse)

all: sltpwmt

sltpwmt: sltpwmt.o
