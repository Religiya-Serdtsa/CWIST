CC      = gcc
CFLAGS  = -Wall -Wextra -g -I../../../include -I../../../lib -I../../../lib/libttak/include
LDFLAGS = -L../../../ -L../../../lib/cjson -L../../../lib/libttak/lib
LIBS    = -lcwist -lcjson -lttak -lpthread -ldl

TARGET = step-1-basic

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
