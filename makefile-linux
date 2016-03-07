CC=g++
CFLAGS=-c -std=c++0x -pthread
LDFLAGS=-std=c++0x -pthread
all: main

main: main.o Lock.o InotifyEventLoop.o InotifyTree.o InotifyService.o NativeInterface.o Queue.o
	$(CC) $(LDFLAGS) main.o Lock.o InotifyEventLoop.o InotifyTree.o InotifyService.o NativeInterface.o Queue.o -o run

main.o: includes/linux/InotifyService.h
	$(CC) $(CFLAGS) src/main.cpp

Lock.o: includes/linux/Lock.h
	$(CC) $(CFLAGS) src/linux/Lock.cpp

InotifyEventLoop.o: includes/linux/Lock.h includes/linux/InotifyEventLoop.h includes/linux/InotifyService.h
	$(CC) $(CFLAGS) src/linux/InotifyEventLoop.cpp

InotifyTree.o: includes/linux/InotifyTree.h
	$(CC) $(CFLAGS) src/linux/InotifyTree.cpp

InotifyService.o: includes/linux/InotifyEventLoop.h includes/linux/InotifyTree.h includes/linux/InotifyService.h
	$(CC) $(CFLAGS) src/linux/InotifyService.cpp

NativeInterface.o: includes/linux/InotifyService.h includes/NativeInterface.h
	$(CC) $(CFLAGS) src/NativeInterface.cpp

Queue.o: includes/Types.h includes/Queue.h
	$(CC) $(CFLAGS) src/Queue.cpp

clean:
	rm *.o run
