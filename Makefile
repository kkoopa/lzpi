TARGET=lzpi
CFLAGS += -std=c11 -Ofast -D_POSIX_C_SOURCE=200112L -Wall -Wextra -pedantic

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) $(LDFLAGS) -o$@ $@.c

.PHONY: clean test
clean:
	$(RM) $(TARGET)

test: $(TARGET)
	./$(TARGET) <$(TARGET) | ./$(TARGET) -d | cmp -s $(TARGET) - && echo "OK" || echo "ERR"
