CC ?= cc
PKG_CONFIG ?= pkg-config

APP := rf-sweep
SRC := $(wildcard src/*.c src/ui/*.c src/core/*.c)

CFLAGS ?= -O2 -g -Wall -Wextra
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags gtk+-3.0 SoapySDR)
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk+-3.0 SoapySDR) -lm

.PHONY: all clean run

all: $(APP)

$(APP): $(SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

run: $(APP)
	./$(APP)

clean:
	rm -f $(APP)
