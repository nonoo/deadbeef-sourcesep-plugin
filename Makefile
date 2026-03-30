CC ?= cc
CFLAGS ?= -O2 -fPIC -std=c99 -Wall -Wextra

GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null || echo "")
INCLUDES = -I/usr/include/deadbeef $(GTK_CFLAGS)
LDFLAGS_SO = -shared
LIBS = $(shell pkg-config --libs gtk+-3.0 sndfile 2>/dev/null || echo "-lsndfile")

TARGET = roformer.so
SRC = roformer.c
INSTALL_DIR = $(HOME)/.local/lib/deadbeef

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS_SO) -o $@ $^ $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	mkdir -p "$(INSTALL_DIR)"
	cp -f "$(TARGET)" "$(INSTALL_DIR)/$(TARGET)"
