CC = gcc
CFLAGS = -Wall -Wextra -g -I../../include
LDFLAGS = -L../../
LIBS = -lcwist -lcjson -luriparser -lssl -lcrypto -lpthread -ldl

SRCS = main.c
TARGET = cde_json_viewer

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)