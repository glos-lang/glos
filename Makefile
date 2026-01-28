CC = cc
CFLAGS = `cat compile_flags.txt` -ggdb

HEADERS = $(wildcard src/*.h)
SOURCES = $(HEADERS:.h=.c)
OBJECTS = $(SOURCES:.c=.o)

glos: src/main.c $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@ -c
