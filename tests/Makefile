CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c99 -I. -I../src/include/microrl

TARGET = test_microrl_utf8
OBJECTS = test_microrl_utf8.o ../src/microrl/microrl.o

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@

test: $(TARGET)
	./$(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

../src/microrl/microrl.o: ../src/microrl/microrl.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o ../src/microrl/microrl.o
