CFLAGS = -Wall -Wextra -std=c11 -I.
LDFLAGS =

# Assumes yyjson.h and yyjson.c are in the current directory.
# If they are in a subdirectory, adjust CFLAGS, e.g., -Iyyjson
SRCS = jisp.c yyjson.c
OBJS = $(SRCS:.c=.o)
TARGET = jisp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
