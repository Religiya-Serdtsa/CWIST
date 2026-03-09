CC      = gcc
CFLAGS  = -Wall -Wextra -g -I../../../include -I../../../lib -I../../../lib/libttak/include
LDFLAGS = -L../../../ -L../../../lib/cjson -L../../../lib/libttak/lib -L../../../lib/uriparser/build
LIBS    = -lcwist -lcjson -lttak -lpthread -ldl -luriparser

TARGET = step-2-mux-router

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
