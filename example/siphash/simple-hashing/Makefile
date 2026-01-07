CC = gcc
CFLAGS = -Wall -Wextra -g -I../../../include
LDFLAGS = -L../../../
LIBS = -lcwist -lcjson

TARGET = simple-hashing

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
