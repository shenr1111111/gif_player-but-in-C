
CC      = gcc
TARGET  = gif_player
SRC     = gif_player.c

CFLAGS  = -O2 -Wall -Wextra -Wno-unused-result -std=c99 \
          $(shell sdl2-config --cflags)

LDFLAGS = $(shell sdl2-config --libs) \
          -lSDL2_image

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
	@echo "Installed to ~/.local/bin/$(TARGET)"

uninstall:
	rm -f $(HOME)/.local/bin/$(TARGET)
	@echo "Uninstalled $(TARGET)"

clean:
	rm -f $(TARGET)
