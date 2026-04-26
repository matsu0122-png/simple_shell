CC = gcc
CFLAGS = -Wall -Wextra -g

TARGET = my_shell
SRC = my_shell.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)