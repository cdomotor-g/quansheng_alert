/* Host test for the station table + lookup.  make -C test */
#include <stdio.h>
#include <string.h>
#include "app/alert_stations.h"
#include "app/alert_stations_gen.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	const char *name; uint8_t kind;

	CHECK(ALERT_STATIONS_COUNT > 0, "table is empty");

	// sorted, unique, every name offset inside the pool and NUL terminated
	for (int i = 0; i < ALERT_STATIONS_COUNT; i++) {
		if (i) CHECK(gAlertSites[i].base_id > gAlertSites[i - 1].base_id, "not sorted/unique at %d", i);
		CHECK(gAlertSites[i].name_off < sizeof(gAlertNames), "name_off out of range at %d", i);
		CHECK(strlen(&gAlertNames[gAlertSites[i].name_off]) <= 13, "name too long at %d", i);
		CHECK((gAlertSites[i].kinds & 0x8000u) == 0, "kinds bit 15 set at %d", i);
		for (int o = 0; o < 5; o++)
			CHECK(((gAlertSites[i].kinds >> (3 * o)) & 7u) <= 6, "bad kind code at %d/%d", i, o);
	}

	// every address the table claims resolves to its site's name
	int addresses = 0;
	for (int i = 0; i < ALERT_STATIONS_COUNT; i++) {
		for (int o = 0; o < 5; o++) {
			const uint8_t k = (gAlertSites[i].kinds >> (3 * o)) & 7u;
			if (!k) continue;
			addresses++;
			const bool hit = ALERT_LookupStation(gAlertSites[i].base_id + o, &name, &kind);
			CHECK(hit && kind == k && strcmp(name, &gAlertNames[gAlertSites[i].name_off]) == 0,
			      "lookup %u failed", gAlertSites[i].base_id + o);
		}
	}

	// misses: below the first site, above the last, and an unused offset
	CHECK(!ALERT_LookupStation(0, &name, &kind) && name[0] == 0 && kind == ALERT_KIND_NONE, "id 0 must miss");
	CHECK(!ALERT_LookupStation(8191, &name, &kind) || gAlertSites[ALERT_STATIONS_COUNT - 1].base_id + 4 >= 8191, "id 8191 must miss");

	CHECK(strcmp(ALERT_KindLabel(ALERT_KIND_RAIN), "RAIN") == 0, "kind label");
	CHECK(ALERT_StationCount() == ALERT_STATIONS_COUNT, "count");
	CHECK(ALERT_StationSource()[0] != 0, "source");

	printf("%s: %d site(s), %d address(es), %d failure(s)\n", fails ? "FAILED" : "PASSED",
	       ALERT_STATIONS_COUNT, addresses, fails);
	return fails ? 1 : 0;
}
