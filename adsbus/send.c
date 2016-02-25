#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "airspy_adsb.h"
#include "beast.h"
#include "buf.h"
#include "json.h"
#include "peer.h"
#include "proto.h"
#include "raw.h"
#include "stats.h"
#include "uuid.h"

#include "send.h"

struct send {
	struct peer peer;
	struct peer *on_close;
	char id[UUID_LEN];
	struct serializer *serializer;
	struct send *prev;
	struct send *next;
};

typedef void (*serializer)(struct packet *, struct buf *);
struct serializer {
	char *name;
	serializer serialize;
	struct send *send_head;
} serializers[] = {
	{
		.name = "airspy_adsb",
		.serialize = airspy_adsb_serialize,
	},
	{
		.name = "beast",
		.serialize = beast_serialize,
	},
	{
		.name = "json",
		.serialize = json_serialize,
	},
	{
		.name = "proto",
		.serialize = proto_serialize,
	},
	{
		.name = "raw",
		.serialize = raw_serialize,
	},
	{
		.name = "stats",
		.serialize = stats_serialize,
	},
};
#define NUM_SERIALIZERS (sizeof(serializers) / sizeof(*serializers))

static void send_del(struct send *send) {
	fprintf(stderr, "S %s (%s): Connection closed\n", send->id, send->serializer->name);
	peer_epoll_del((struct peer *) send);
	assert(!close(send->peer.fd));
	if (send->prev) {
		send->prev->next = send->next;
	} else {
		send->serializer->send_head = send->next;
	}
	if (send->next) {
		send->next->prev = send->prev;
	}
	peer_call(send->on_close);
	free(send);
}

static void send_del_wrapper(struct peer *peer) {
	send_del((struct send *) peer);
}

static bool send_hello(int fd, struct serializer *serializer) {
	struct buf buf = BUF_INIT;
	serializer->serialize(NULL, &buf);
	if (buf.length == 0) {
		return true;
	}
	if (send(fd, buf_at(&buf, 0), buf.length, MSG_NOSIGNAL) != buf.length) {
		return false;
	}
	return true;
}

void send_init() {
}

void send_cleanup() {
	for (int i = 0; i < NUM_SERIALIZERS; i++) {
		struct serializer *serializer = &serializers[i];
		while (serializer->send_head) {
			send_del(serializer->send_head);
		}
	}
}

struct serializer *send_get_serializer(char *name) {
	for (int i = 0; i < NUM_SERIALIZERS; i++) {
		if (strcasecmp(serializers[i].name, name) == 0) {
			return &serializers[i];
		}
	}
	return NULL;
}

void send_new(int fd, struct serializer *serializer, struct peer *on_close) {
	struct send *send = malloc(sizeof(*send));
	assert(send);

	send->peer.fd = fd;
	send->peer.event_handler = send_del_wrapper;
	send->on_close = on_close;
	uuid_gen(send->id);
	send->serializer = serializer;
	send->prev = NULL;
	send->next = serializer->send_head;
	serializer->send_head = send;

	peer_epoll_add((struct peer *) send, EPOLLIN);

	fprintf(stderr, "S %s (%s): New send connection\n", send->id, serializer->name);

	if (!send_hello(fd, serializer)) {
		fprintf(stderr, "S %s: Failed to write hello\n", send->id);
		send_del(send);
		return;
	}
}

void send_new_wrapper(int fd, void *passthrough, struct peer *on_close) {
	send_new(fd, (struct serializer *) passthrough, on_close);
}

void send_write(struct packet *packet) {
	for (int i = 0; i < NUM_SERIALIZERS; i++) {
		struct serializer *serializer = &serializers[i];
		if (serializer->send_head == NULL) {
			continue;
		}
		struct buf buf = BUF_INIT;
		serializer->serialize(packet, &buf);
		if (buf.length == 0) {
			continue;
		}
		struct send *send_obj = serializer->send_head;
		while (send_obj) {
			if (send(send_obj->peer.fd, buf_at(&buf, 0), buf.length, MSG_NOSIGNAL) != buf.length) {
				// peer_loop() will see this shutdown and call send_del
				shutdown(send_obj->peer.fd, SHUT_RD | SHUT_WR);
			}
			send_obj = send_obj->next;
		}
	}
}

void send_print_usage() {
	fprintf(stderr, "\nSupported send formats:\n");
	for (int i = 0; i < NUM_SERIALIZERS; i++) {
		fprintf(stderr, "\t%s\n", serializers[i].name);
	}
}
