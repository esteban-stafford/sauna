
TARGET = sauna
LIBS = -lnvidia-ml -lmicmgmt
#EXTRA_LDFLAGS = $(LDFLAGS) -lscif -lmicmgmt
CC = gcc
CFLAGS = -g -Wall -I/usr/include/nvidia/gdk

.PHONY: default all clean

default: $(TARGET)
all: default

OBJECTS = $(patsubst %.c, %.o, $(wildcard *.c))
HEADERS = $(wildcard *.h)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -Wall $(LIBS) -o $@

clean:
	-rm -f *.o
	-rm -f $(TARGET)
