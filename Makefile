CFLAGS := -std=gnu99 -O2 -Wall -fPIC -g

all: getenv target

getenv: getenv.c
	$(CC) $(CFLAGS) $< -o $@

target: target.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f getenv target

.PHONY: all clean
