#include <assert.h>
#include <time.h>
#include <jansson.h>

#include "buf.h"
#include "json.h"
#include "packet.h"

#include "stats.h"

static struct stats_state {
	uint64_t total_count;
	uint64_t type_count[NUM_TYPES];
	struct timespec start;
} stats_state = {
	.type_count = { 0 },
};

void stats_init() {
	assert(!clock_gettime(CLOCK_MONOTONIC_COARSE, &stats_state.start));
}

void stats_serialize(struct packet *packet, struct buf *buf) {
	if (packet) {
		stats_state.total_count++;
		stats_state.type_count[packet->type]++;
	}
	if (stats_state.total_count % 1000 != 0) {
		return;
	}
	json_t *counts = json_object();
	for (int i = 0; i < NUM_TYPES; i++) {
		if (i == PACKET_TYPE_NONE) {
			continue;
		}
		json_object_set_new(counts, packet_type_names[i], json_integer((json_int_t) stats_state.type_count[i]));
	}
	struct timespec now;
	assert(!clock_gettime(CLOCK_MONOTONIC_COARSE, &now));
	json_t *out = json_pack("{sIso}",
			"uptime_seconds", (json_int_t) (now.tv_sec - stats_state.start.tv_sec),
			"packet_counts", counts);
	assert(json_dump_callback(out, json_buf_append_callback, buf, 0) == 0);
	json_decref(out);
	buf_chr(buf, buf->length++) = '\n';
}
