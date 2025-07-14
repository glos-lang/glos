CC = cc
CFLAGS = `cat compile_flags.txt` -g

QBE_DIR = thirdparty/libqbe
LIBQBE_DIR = $(QBE_DIR)/lib
LIBQBE_PATH = $(LIBQBE_DIR)/libqbe.a

HEADERS = $(wildcard src/*.h)
SOURCES = $(HEADERS:.h=.c)
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all
all: bin/test bin/glos bin/runtime.o

bin/test: src/basic.h src/basic.o src/test.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ src/basic.o src/test.c

bin/glos: $(OBJECTS) $(LIBQBE_PATH) src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) src/main.c -L$(LIBQBE_DIR) -lqbe

bin/runtime.o: src/runtime.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBQBE_PATH):
	$(MAKE) -C $(QBE_DIR)
