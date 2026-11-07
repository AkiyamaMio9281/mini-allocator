CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -pedantic -pthread
BUILD  = build

LIB = mini_allocator.c mini_allocator.h

.PHONY: all test tsan bench clean

all: test

# Unit and stress tests under AddressSanitizer and UBSan.
test: $(BUILD)/test
	./$(BUILD)/test

# Same tests under ThreadSanitizer to catch data races.
tsan: $(BUILD)/tsan
	./$(BUILD)/tsan

# Optimized benchmark, no sanitizers.
bench: $(BUILD)/bench
	./$(BUILD)/bench

$(BUILD)/test: test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -o $@ test.c mini_allocator.c

$(BUILD)/tsan: test.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -g -fsanitize=thread -o $@ test.c mini_allocator.c

$(BUILD)/bench: bench.c $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) -O2 -o $@ bench.c mini_allocator.c

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
