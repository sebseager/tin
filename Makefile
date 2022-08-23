CC ?= gcc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -O3 -g3
TARGET = tin
SOURCES = $(wildcard *.c)
HEADERS = $(wildcard *.h)
OBJECTS = $(SOURCES:%.c=%.o)

all: $(TARGET)

$(TARGET): $(OBJECTS) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS)

.PHONY: clean
clean:
	$(RM) -r $(TARGET) $(OBJECTS) $(TARGET).dSYM vgcore.*