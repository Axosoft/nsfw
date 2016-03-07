CC=clang++
LDFLAGS=-std=c++0x -framework CoreServices
CFLAGS=-c -pthread

all: main

main: main.o FSEventsService.o RunLoop.o Lock.o
	$(CC) $(LDFLAGS) main.o FSEventsService.o RunLoop.o Lock.o -o run

main.o:
	$(CC) $(LDFLAGS) $(CFLAGS) src/main.cpp

RunLoop.o: includes/osx/FSEventsService.h includes/osx/RunLoop.h includes/Lock.h
	$(CC) $(LDFLAGS) $(CFLAGS) src/osx/RunLoop.cpp

Lock.o: includes/Lock.h
	$(CC) $(LDFLAGS) $(CFLAGS) src/Lock.cpp

FSEventsService.o: includes/osx/RunLoop.h includes/osx/FSEventsService.h includes/osx/FSTree.h
	$(CC) $(LDFLAGS) $(CFLAGS) src/osx/FSEventsService.cpp

clean:
	rm *.o run
