C      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread -static
KDIR   ?= /lib/modules/$(shell uname -r)/build

obj-m := monitor.o

.PHONY: all clean user kernel

all: user kernel

user: engine cpu_hog io_pulse memory_hog

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o io_pulse io_pulse.c

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o memory_hog memory_hog.c

kernel:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f engine cpu_hog io_pulse memory_hog
