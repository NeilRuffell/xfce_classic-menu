# Simple Makefile for XFCE Classic Menu Plugin

# Build configuration
CC = gcc
CFLAGS = -Wall -fPIC $(shell pkg-config --cflags gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 garcon-1 exo-2 libwnck-3.0)
LIBS = $(shell pkg-config --libs gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 garcon-1 exo-2 libwnck-3.0)
LDFLAGS = -shared -Wl,--export-dynamic

# Installation paths
PREFIX ?= /usr
PLUGIN_DIR = $(PREFIX)/lib/xfce4/panel/plugins
DESKTOP_DIR = $(PREFIX)/share/xfce4/panel/plugins

# Source files
SRC = panel-plugin/classic-menu.c \
      panel-plugin/applications-menu.c \
      panel-plugin/appimage-thumbs.c \
      panel-plugin/places-menu.c \
      panel-plugin/system-menu.c
OBJ = $(SRC:.c=.o)
TARGET = libclassic-menu.so
DESKTOP = panel-plugin/classic-menu.desktop

# Default target
all: $(TARGET) $(DESKTOP)

# Build the plugin
$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Generate desktop file
$(DESKTOP): panel-plugin/classic-menu.desktop.in
	cp $< $@

# Install
install: all
	install -d $(DESTDIR)$(PLUGIN_DIR)
	install -d $(DESTDIR)$(DESKTOP_DIR)
	install -m 755 $(TARGET) $(DESTDIR)$(PLUGIN_DIR)/
	install -m 644 $(DESKTOP) $(DESTDIR)$(DESKTOP_DIR)/
	@echo ""
	@echo "Installed! Now run: xfce4-panel --restart"
	@echo ""

# Uninstall
uninstall:
	rm -f $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)
	rm -f $(DESTDIR)$(DESKTOP_DIR)/classic-menu.desktop

# Clean build artifacts
clean:
	rm -f $(OBJ) $(TARGET) $(DESKTOP)

# Clean everything including autotools files
distclean: clean
	rm -rf autom4te.cache/ .deps/ .libs/ m4/
	rm -f Makefile Makefile.in aclocal.m4 config.* configure
	rm -f compile depcomp install-sh missing libtool ltmain.sh stamp-h1
	rm -f panel-plugin/Makefile panel-plugin/Makefile.in
	rm -f po/Makefile.in.in

.PHONY: all install uninstall clean distclean
