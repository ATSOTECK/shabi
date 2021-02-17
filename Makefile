CFLAGS=-g
LDLIBS=-lpthread
CC=gcc

all: shabi.x

shabi.x: main.c
	${CC} ${CFLAGS} -o $@ $< ${LDLIBS}
