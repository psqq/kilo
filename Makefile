BIN = kilo

.PHONY: all build clean rebuild b c reb run r

all: build

build:
	mkdir -p build
	cd build && cmake .. && make
	cp build/$(BIN) ./

clean:
	rm -rf build

rebuild: clean build

run: build
	./$(BIN)

# aliases
b: build
c: clean
reb: rebuild
r: run
