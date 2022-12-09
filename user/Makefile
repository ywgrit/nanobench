CC=gcc
CFLAGS=-Wall -g

all: nanoBench
nanoBench: nanoBench_main.o nanoBench.o
nanoBench_main.o: nanoBench_main.c ../common/nanoBench.h
	$(CC) $(CFLAGS) -c nanoBench_main.c -o $@
nanoBench.o: ../common/nanoBench.c ../common/nanoBench.h
	$(CC) $(CFLAGS) -c ../common/nanoBench.c -o $@

clean:
	rm -f nanoBench *.o
