CC = cc
CFLAGS = `cat compile_flags.txt` -g

QBE_DIR = thirdparty/libqbe
LIBQBE_DIR = $(QBE_DIR)/lib
LIBQBE_PATH = $(LIBQBE_DIR)/libqbe.a

HEADERS = $(wildcard src/*.h)
SOURCES = $(HEADERS:.h=.c)
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all
all: tests/test glos

tests/test: src/basic.h src/basic.o src/test.c
	$(CC) $(CFLAGS) -o $@ src/basic.o src/test.c

glos: $(OBJECTS) $(LIBQBE_PATH) src/main.c
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) src/main.c -L$(LIBQBE_DIR) -lqbe

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBQBE_PATH):
	$(MAKE) -C $(QBE_DIR)
