CC = gcc
TARGET=smallsh
CFLAGS = -std=c11 -Wall -Werror -g3 -O0

$(TARGET): smallsh.c
	$(CC) smallsh.c -o $@ $(CFLAGS)

all: $(TARGET)

clean:
	rm -f *.o $(TARGET)

.PHONY: clean, all, install