#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "log.h"
#include "server.h"
#include "uuid.h"
#include "wakeup.h"

#include "peer.h"

uint32_t peer_count_in = 0, peer_count_out = 0, peer_count_out_in = 0;

static int peer_epoll_fd;
static int peer_shutdown_fd;
static bool peer_shutdown_flag = false;
static struct list_head peer_always_trigger_head = LIST_HEAD_INIT(peer_always_trigger_head);

static void peer_shutdown_handler(struct peer *peer) {
	log_write('X', server_id, "Shutting down");
	assert(!close(peer->fd));
	free(peer);
	peer_shutdown_flag = true;
}

void peer_init() {
	peer_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	assert(peer_epoll_fd >= 0);

	int shutdown_fds[2];
	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, shutdown_fds));

	struct peer *shutdown_peer = malloc(sizeof(*shutdown_peer));
	assert(shutdown_peer);
	shutdown_peer->fd = shutdown_fds[0];
	shutdown_peer->event_handler = peer_shutdown_handler;
	peer_epoll_add(shutdown_peer, EPOLLRDHUP);

	peer_shutdown_fd = shutdown_fds[1];
	signal(SIGINT, peer_shutdown);
	signal(SIGTERM, peer_shutdown);
}

void peer_cleanup() {
	assert(!close(peer_epoll_fd));
}

void peer_shutdown(int __attribute__((unused)) signal) {
	if (peer_shutdown_fd != -1) {
		assert(!close(peer_shutdown_fd));
		peer_shutdown_fd = -1;
	}
}

void peer_epoll_add(struct peer *peer, uint32_t events) {
	struct epoll_event ev = {
		.events = events,
		.data = {
			.ptr = peer,
		},
	};
	peer->always_trigger = false;
	int res = epoll_ctl(peer_epoll_fd, EPOLL_CTL_ADD, peer->fd, &ev);
	if (res == -1 && errno == EPERM) {
		// Not a socket
		if (events) {
			list_add(&peer->peer_always_trigger_list, &peer_always_trigger_head);
			peer->always_trigger = true;
		}
	} else {
		assert(!res);
	}
}

void peer_epoll_del(struct peer *peer) {
	int res = epoll_ctl(peer_epoll_fd, EPOLL_CTL_DEL, peer->fd, NULL);
	if (res == -1 && errno == EPERM) {
		if (peer->always_trigger) {
			list_del(&peer->peer_always_trigger_list);
		}
	} else {
		assert(!res);
	}
}

void peer_call(struct peer *peer) {
	if (peer_shutdown_flag || !peer) {
		return;
	}
	peer->event_handler(peer);
}

void peer_loop() {
	log_write('X', server_id, "Starting event loop");
	while (!peer_shutdown_flag) {
		if (!(peer_count_in + peer_count_out_in)) {
			log_write('X', server_id, "No remaining inputs");
			peer_shutdown(0);
		} else if (!(peer_count_out + peer_count_out_in)) {
			log_write('X', server_id, "No remaining outputs");
			peer_shutdown(0);
		}
#define MAX_EVENTS 10
		struct epoll_event events[MAX_EVENTS];
		int delay = list_is_empty(&peer_always_trigger_head) ? wakeup_get_delay() : 0;
		int nfds = epoll_wait(peer_epoll_fd, events, MAX_EVENTS, delay);
		if (nfds == -1 && errno == EINTR) {
			continue;
		}
		assert(nfds >= 0);

    for (int n = 0; n < nfds; n++) {
			struct peer *peer = events[n].data.ptr;
			peer_call(peer);
		}

		{
			struct peer *iter, *next;
			list_for_each_entry_safe(iter, next, &peer_always_trigger_head, peer_always_trigger_list) {
				peer_call(iter);
			}
		}

		wakeup_dispatch();
	}
}
