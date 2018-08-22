#elan touchpad updater Makefile
CC = gcc

CFLAGS += -g -Wall -fexceptions

main: etphid_updater.o 
	${CC} ${CFLAGS} ${CPPFLAGS} etphid_updater.o -o etphid_updater

etphid_updater.o: etphid_updater.c
	${CC} ${CFLAGS} ${CPPFLAGS} etphid_updater.c -c

clean:
	rm -rf etphid_updater.o
	
