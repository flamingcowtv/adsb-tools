#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "log.h"
#include "peer.h"
#include "uuid.h"

#pragma GCC diagnostic ignored "-Wformat-nonliteral"

static FILE *log_stream = NULL;
static char *log_path = NULL;
static uint8_t log_id[UUID_LEN];
static int log_rotate_fd;
static struct peer log_rotate_peer;

static char log_module = 'L';

static void log_rotate() {
	uint8_t old_log_id[UUID_LEN], new_log_id[UUID_LEN];
	uuid_gen(new_log_id);
	LOG(log_id, "Switching to new log with ID %s at: %s", new_log_id, log_path);
	memcpy(old_log_id, log_id, UUID_LEN);
	memcpy(log_id, new_log_id, UUID_LEN);
	assert(!fclose(log_stream));
	log_stream = fopen(log_path, "a");
	assert(log_stream);
	setlinebuf(log_stream);
	LOG(log_id, "Log start after switch from log ID %s", old_log_id);
}

static void log_rotate_handler(struct peer *peer) {
	char buf[1];
	assert(read(peer->fd, buf, 1) == 1);
	assert(buf[0] == 'L');
	log_rotate();
}

static void log_rotate_signal(int __attribute__ ((unused)) signum) {
	assert(write(log_rotate_fd, "L", 1) == 1);
}

void log_init() {
	int fd = dup(STDERR_FILENO);
	assert(fd >= 0);
	log_stream = fdopen(fd, "a");
	assert(log_stream);
	setlinebuf(log_stream);

	uuid_gen(log_id);
	LOG(log_id, "Log start");
}

void log_init2() {
	int rotate_fds[2];
	assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, rotate_fds));
	log_rotate_peer.fd = rotate_fds[0];
	assert(!shutdown(log_rotate_peer.fd, SHUT_WR));
	log_rotate_peer.event_handler = log_rotate_handler;
	peer_epoll_add(&log_rotate_peer, EPOLLIN);

	log_rotate_fd = rotate_fds[1];
	signal(SIGHUP, log_rotate_signal);
}

void log_cleanup() {
	LOG(log_id, "Log end");
	assert(!fclose(log_stream));
	assert(!close(log_rotate_fd));
	assert(!close(log_rotate_peer.fd));
	if (log_path) {
		free(log_path);
		log_path = NULL;
	}
}

bool log_reopen(const char *path) {
	if (log_path) {
		free(log_path);
	}
	log_path = strdup(path);
	assert(log_path);
	log_rotate();
	return true;
}

void log_write(char type, const uint8_t *id, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	assert(fprintf(log_stream, "%c %s: ", type, id) > 0);
	assert(vfprintf(log_stream, fmt, ap) > 0);
	assert(fprintf(log_stream, "\n") == 1);
	va_end(ap);
}
