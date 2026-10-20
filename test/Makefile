# Host-side unit tests (plain gcc, no cross toolchain needed)
CC      ?= gcc
CFLAGS  ?= -O1 -g -Wall -Wextra -Werror -std=c11 -I..

all: test_alert_decode test_alert_scan test_alert_demod test_alert_stations
	./test_alert_decode
	./test_alert_scan
	./test_alert_demod
	./test_alert_stations
	@command -v node >/dev/null 2>&1 && node test_k5protocol.mjs || echo "  skip  node protocol tests (node not installed)"

test_alert_decode: test_alert_decode.c ../app/alert_decode.c ../app/alert_decode.h
	$(CC) $(CFLAGS) test_alert_decode.c ../app/alert_decode.c -o $@

# shared with quansheng_alert_v3: inversion, the idle gate, the sample scanner
test_alert_scan: test_alert_scan.c ../app/alert_decode.c ../app/alert_decode.h ../app/alert_stations_gen.h
	$(CC) $(CFLAGS) test_alert_scan.c ../app/alert_decode.c -o $@

# the software AFSK demodulator, fed synthesised bursts, through to decoded frames
test_alert_demod: test_alert_demod.c ../app/alert_demod.c ../app/alert_demod.h ../app/alert_decode.c ../app/alert_decode.h
	$(CC) $(CFLAGS) test_alert_demod.c ../app/alert_demod.c ../app/alert_decode.c -lm -o $@

test_alert_stations: test_alert_stations.c ../app/alert_stations.c ../app/alert_stations.h ../app/alert_stations_gen.h
	$(CC) $(CFLAGS) test_alert_stations.c ../app/alert_stations.c -o $@

clean:
	rm -f test_alert_decode test_alert_scan test_alert_demod test_alert_stations

.PHONY: all clean
