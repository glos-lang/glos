CC = cc
CFLAGS = `cat compile_flags.txt` -g

QBEDIR = src/libqbe/lib
QBELIB = $(QBEDIR)/libqbe.a

HEADERS = $(wildcard src/*.h)
SOURCES = $(HEADERS:.h=.c)
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all
all: bin/test bin/glos

bin/test: src/basic.h src/basic.o src/test.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ src/basic.o src/test.c

bin/glos: $(OBJECTS) $(QBELIB) src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) src/main.c -L$(QBEDIR) -lqbe

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(QBELIB):
	$(MAKE) -C src/libqbe
