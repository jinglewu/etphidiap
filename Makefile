#elan touchpad updater Makefile
CC = g++

main: etphid_updater.o 
	${CC} etphid_updater.o -o etphid_updater

etphid_updater.o: etphid_updater.c
	${CC} etphid_updater.c -c

clean:
	rm -rf etphid_updater.o
