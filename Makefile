TARGET = sauna

CC = gcc
CFLAGS = -g -Wall

NVIDIA = 1
XEONPHI = 1

ifeq ($(NVIDIA),1)
   CFLAGS += -DNVIDIA -I/usr/include/nvidia/gdk
   LIBS += -lnvidia-ml
endif
ifeq ($(XEONPHI),1)
   CFLAGS += -DXEONPHI -I/usr/include/nvidia/gdk
   LIBS += -lmicmgmt
endif

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
