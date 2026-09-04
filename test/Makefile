# Host-side unit tests (plain gcc, no cross toolchain needed)
CC      ?= gcc
CFLAGS  ?= -O1 -g -Wall -Wextra -Werror -std=c11 -I..

all: test_alert_decode test_alert_stations
	./test_alert_decode
	./test_alert_stations

test_alert_decode: test_alert_decode.c ../app/alert_decode.c ../app/alert_decode.h
	$(CC) $(CFLAGS) test_alert_decode.c ../app/alert_decode.c -o $@

test_alert_stations: test_alert_stations.c ../app/alert_stations.c ../app/alert_stations.h ../app/alert_stations_gen.h
	$(CC) $(CFLAGS) test_alert_stations.c ../app/alert_stations.c -o $@

clean:
	rm -f test_alert_decode test_alert_stations

.PHONY: all clean
