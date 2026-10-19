# Makefile - Sensor gateway (giữa kỳ, Step 0-7)
CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -Iinclude -g
LDLIBS  = -lsqlite3 -pthread

SRC = src/main.c src/sbuffer.c src/logger.c src/connmgr.c src/adapters.c \
      src/datamgr.c src/storagemgr.c

all: server sensor_node test_parse

server: $(SRC) include/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

sensor_node: test/sensor_node.c src/adapters.c
	$(CC) $(CFLAGS) -o $@ test/sensor_node.c src/adapters.c

test_parse: test/test_parse.c src/adapters.c
	$(CC) $(CFLAGS) -o $@ test/test_parse.c src/adapters.c

# make debug: bật in thông tin chẩn đoán
debug: CFLAGS += -DDEBUG
debug: clean all

test: test_parse
	./test_parse

clean:
	rm -f server sensor_node test_parse

# xoá luôn dữ liệu chạy thử
distclean: clean
	rm -f gateway.log sensor.db logFifo

.PHONY: all debug test clean distclean
