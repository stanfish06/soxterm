CC = gcc
CFLAGS = -Wall -O2 $(shell pkg-config --cflags xft fontconfig)
LDLIBS = $(shell pkg-config --libs xft fontconfig) -lX11 -lm

SRC = main.c screen.c pty.c
OBJ = $(SRC:.c=.o)
TARGET = soxterm

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

main.o: screen.h pty.h
screen.o: config.h screen.h pty.h
pty.o: pty.h

clean:
	rm -f $(OBJ) $(TARGET)

format:
	clang-format -i *.c

install: all
	cp soxterm ~/.local/bin/
