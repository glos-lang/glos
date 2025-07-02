CC = cc
CFLAGS = `cat compile_flags.txt` -g

QBEDIR = src/libqbe/lib
QBELIB = $(QBEDIR)/libqbe.a

HEADERS = $(wildcard src/*.h)
SOURCES = $(HEADERS:.h=.c)
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all
all: bin/test bin/glos

bin/test: bin src/basic.h src/basic.o src/test.c
	$(CC) $(CFLAGS) -o $@ src/basic.o src/test.c

bin/glos: bin $(OBJECTS) $(QBELIB) src/main.c
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) src/main.c -L$(QBEDIR) -lqbe

bin:
	mkdir -p bin

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(QBELIB):
	cd src/libqbe && make
