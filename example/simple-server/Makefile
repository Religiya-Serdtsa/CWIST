CC = gcc
CFLAGS = -Wall -Wextra -g -I../../include
LDFLAGS = -L../../
LIBS = -lcwist -lcjson -lsqlite3 -luriparser -lssl -lcrypto -lpthread -lstdc++ -lm -lz

SRCS = main.c
TARGET = simple_server

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)
