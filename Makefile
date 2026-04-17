CC       := gcc
CXX      := g++
CFLAGS   := $(shell pkg-config --cflags wayland-client)
CXXFLAGS := $(shell pkg-config --cflags gtk+-3.0 wayland-client) \
            -std=c++17 -Wall -Wextra -Wno-unused-parameter
LIBS     := $(shell pkg-config --libs gtk+-3.0 wayland-client)

# wayland-protocols provides the keyboard-shortcuts-inhibit XML
WP_DIR      := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
INHIBIT_XML := $(WP_DIR)/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml

TARGET := keybindz

.PHONY: all clean install

all: inhibit-protocol.h inhibit-protocol.o $(TARGET)

inhibit-protocol.h: $(INHIBIT_XML)
	wayland-scanner client-header $< $@

inhibit-protocol.c: $(INHIBIT_XML)
	wayland-scanner private-code $< $@

# Compile the C protocol glue with gcc (not g++) to preserve C linkage correctly.
inhibit-protocol.o: inhibit-protocol.c inhibit-protocol.h
	$(CC) $(CFLAGS) -c -o $@ inhibit-protocol.c

$(TARGET): keybindz.cpp inhibit-protocol.h inhibit-protocol.o
	$(CXX) $(CXXFLAGS) -o $@ keybindz.cpp inhibit-protocol.o $(LIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)

clean:
	rm -f $(TARGET) inhibit-protocol.h inhibit-protocol.c inhibit-protocol.o
