CFLAGS=-g
LDLIBS=-lpthread
CC=gcc

all: shabi

shabi: main.c
	${CC} ${CFLAGS} -o $@ $< ${LDLIBS}
	
run: shabi
	./shabi main.c
