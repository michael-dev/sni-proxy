/* Copyright (c) 2015, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#include "ev.h"
#include "ucl.h"
#include "util.h"
#include "ringbuf.h"
#include "sni-private.h"
#include "defaults.h"

#if defined(__GNUC__)
#  define _PACKED __attribute__ ((packed))
#else
#  define _PACKED
#endif

#if !defined(__GNUC__)
#  ifdef __IBMC__
#    pragma pack(1)
#  else
#    pragma pack(push, 1)
#  endif
#endif

static const unsigned char tls_magic[3] = {0x16, 0x3, 0x1};
static const unsigned int tls_greeting = 0x1;
static const unsigned int sni_type = 0x0;
static const unsigned int sni_host = 0x0;
static const unsigned int tls_alert = 0x15;
static const unsigned int tls_alert_level = 0x2;
static const unsigned int tls_alert_description = 0x28;

struct ssl_header {
	uint8_t tls_type;
	uint8_t ssl_version[2]; /* 3 bytes of magic we'd expect */
	uint8_t len[2];
	uint8_t type;
	uint8_t greeting_len[3];
	uint16_t tls_version;
	uint8_t random[32];
} _PACKED;

struct sni_ext {
	uint8_t slen[2];
	uint8_t type;
	uint8_t hlen[2];
	uint8_t host[1]; /* Extended */
} _PACKED;

struct ssl_alert {
	uint8_t type;
	uint8_t version[2];
	uint8_t len[2];
	uint8_t level;
	uint8_t description;
} _PACKED;

extern int buflen;
extern void proxy_create(struct ssl_session *s);

static inline unsigned int
int_3byte_be(const unsigned char *p) {
	return
	(((unsigned int)(p[2])      ) |
	 ((unsigned int)(p[1]) <<  8) |
	 ((unsigned int)(p[0]) << 16));
}

static inline unsigned int
int_2byte_be(const unsigned char *p) {
	return
	(((unsigned int)(p[1])      ) |
	 ((unsigned int)(p[0]) <<  8));
}

void
terminate_session(struct ssl_session *ssl)
{
	if (ssl->fd != -1) {
		ev_io_stop(ssl->loop, &ssl->io);
		close(ssl->fd);
	}
	if (ssl->bk_fd != -1) {
		ev_io_stop(ssl->loop, &ssl->bk_io);
		close(ssl->bk_fd);
	}
	ev_timer_stop(ssl->loop, &ssl->tm);
	free(ssl->hostname);
	free(ssl->saved_buf);
	ringbuf_destroy(ssl->bk2cl);
	ringbuf_destroy(ssl->cl2bk);
	free(ssl);
}

static void
alert_cb(EV_P_ ev_io *w, int revents)
{
	struct ssl_alert alert;
	struct ssl_session *ssl = w->data;

	if (ssl->state == ssl_state_alert) {
		alert.type = tls_alert;
		memcpy (alert.version, ssl->ssl_version, 2);
		alert.len[1] = 2;
		alert.level = tls_alert_level;
		alert.description = tls_alert_description;
		ssl->state = ssl_state_alert_sent;

		write(ssl->fd, &alert, sizeof(alert));
	}
	else {
		terminate_session(ssl);
	}
}

void
send_alert(struct ssl_session *ssl)
{
	ssl->state = ssl_state_alert;
	ev_io_init(&ssl->io, alert_cb, ssl->fd, EV_WRITE);
	ev_io_start(ssl->loop, &ssl->io);
}

static void
backend_connect_cb(EV_P_ ev_io *w, int revents)
{
	struct ssl_session *ssl = w->data;

	ev_io_stop(ssl->loop, &ssl->bk_io);
	//printf("connected to hostname: %s\n", ssl->hostname);
	ssl->cl2bk = ringbuf_create(buflen, ssl->saved_buf, ssl->buflen);
	ssl->bk2cl = ringbuf_create(buflen, NULL, 0);
	proxy_create(ssl);
}

static void
connect_backend(struct ssl_session *ssl, const struct addrinfo *ai)
{
	int sock, ofl;

	/* TODO: Need upstreams support here */
	sock = socket(ai->ai_family, SOCK_STREAM, 0);

	if (sock == -1) {
		goto err;
	}

	if (fcntl(sock, F_SETFD, FD_CLOEXEC) == -1) {
		close(sock);

		goto err;
	}

	ofl = fcntl(sock, F_GETFL, 0);

	if (fcntl(sock, F_SETFL, ofl | O_NONBLOCK) == -1) {
		close(sock);

		goto err;
	}

	while (connect (sock, ai->ai_addr, ai->ai_addrlen) == -1) {

		if (errno == EINTR) {
			continue;
		}

		if (errno != EINPROGRESS) {
			close(sock);

			goto err;
		}
		else {
			break;
		}
	}

	ssl->bk_fd = sock;
	ssl->state = ssl_state_backend_ready;

	ssl->bk_io.data = ssl;
	ev_io_init(&ssl->bk_io, backend_connect_cb, sock, EV_WRITE);
	ev_io_start(ssl->loop, &ssl->bk_io);

	return;

err:
	send_alert(ssl);
}

static int
parse_extension(struct ssl_session *ssl, const unsigned char *pos, int remain)
{
	unsigned int tlen, type, hlen;
	const struct sni_ext *sni;

	if (remain < 0) {
		return -1;
	}
	if (remain < 4) {
		return 0;
	}

	type = int_2byte_be(pos);
	tlen = int_2byte_be(pos + 2);

	if (tlen > remain) {
		return -1;
	}

	if (type == sni_type) {
		if (tlen < sizeof(*sni)) {
			return -1;
		}

		sni = (const struct sni_ext *)(pos + 4);
		if (int_2byte_be(sni->slen) != tlen - 2 ||
			sni->type != sni_host ||
			int_2byte_be(sni->hlen) != tlen - 5) {
			return -1;
		}

		hlen = int_2byte_be(sni->hlen);
		free(ssl->hostname);
		ssl->hostname = xmalloc(hlen + 1);
		memcpy(ssl->hostname, sni->host, hlen);
		ssl->hostlen = hlen;
		ssl->hostname[hlen] = '\0';
	}

	return tlen + 4;
}

static void
parse_ssl_greeting(struct ssl_session *ssl, const unsigned char *buf, int len)
{
	const unsigned char *p = buf;
	int remain = len, ret;
	unsigned int tlen;
	const struct ssl_header *sslh;
	const ucl_object_t *bk = NULL, *elt = NULL;
	const char *hostname = NULL;
	unsigned hostlen = 0;
	int port = default_backend_port;
	struct addrinfo ai, *res;

	memset(&ai, 0, sizeof(ai));

	ai.ai_family = AF_UNSPEC;
	ai.ai_socktype = SOCK_STREAM;
	ai.ai_flags = AI_NUMERICSERV;

	ev_io_stop(ssl->loop, &ssl->io);

	if (len <= sizeof(struct ssl_header)) {
		send_alert(ssl);
		return;
	}

	sslh = (const struct ssl_header *)p;
	memcpy (ssl->ssl_version, sslh->ssl_version, 2);

	/* Not an SSL packet */
	if (memcmp(&sslh->tls_type, tls_magic, sizeof(tls_magic)) != 0 ||
		sslh->type != tls_greeting ||
		int_2byte_be(sslh->len) != len - 5 ||
		int_3byte_be(sslh->greeting_len) != len - 5 - 4) {
		goto err;
	}

	p = p + sizeof(*sslh);
	remain -= sizeof(*sslh);

	/* Session id */
	tlen = *p;
	if (tlen >= remain + 4) {
		goto err;
	}
	p = p + tlen + 1;
	remain -= tlen + 1;

	/* Cipher suite */
	tlen = int_2byte_be(p);
	if (tlen >= remain + 4) {
		goto err;
	}
	p = p + tlen + 2;
	remain -= tlen + 2;

	/* Compression methods */
	tlen = *p;
	if (tlen >= remain + 4) {
		goto err;
	}
	p = p + tlen + 1;
	remain -= tlen + 1;

	/* Now extensions */
	tlen = int_2byte_be(p);
	if (tlen > remain) {
		goto err;
	}

	p += 2;
	remain -= 2;

	while ((ret = parse_extension(ssl, p, remain)) > 0) {
		p += ret;
		remain -= ret;
	}

	if (ret == 0) {
		/* Here we can select a backend */
		hostname = ssl->hostname;
		hostlen = ssl->hostlen;
		while (hostname && hostlen) {
			char *key = strndup(hostname, hostlen);
			if (!key) break;
			if (key[0] == '.') key[0] = '/';
			// fprintf(stderr, "try key %s for hostname: %s\n", key, hostname);
			bk = ucl_object_find_keyl(ssl->backends, key, hostlen);
			// if (bk) fprintf(stderr, "found key %s for hostname %s for ssl hostname %s\n", key, hostname, ssl->hostname);
			if (bk) break;
			// (.?)foo.example.org => .example.org;
			do {
				hostname++;
				hostlen--;
			} while ( hostlen && hostname[0] && hostname[0] != '.');
		}

		if (bk == NULL) {
			/* Try to select default backend */
			bk = ucl_object_find_key(ssl->backends, "default");
		}

		if (bk == NULL) {
			/* Cowardly give up */
			fprintf(stderr, "cannot found hostname: %s\n", ssl->hostname);
			send_alert(ssl);
			return;
		}
		else {
			elt = ucl_object_find_key(bk, "port");
			if (elt != NULL) {
				port = ucl_object_toint(elt);
			}

			hostname = ssl->hostname;
			elt = ucl_object_find_key(bk, "host");
			if (elt != NULL) {
				hostname = ucl_object_tostring(elt);
			}

			res = NULL;
			if ((ret = getaddrinfo(hostname, port_to_str(port), &ai, &res)) != 0) {
				fprintf(stderr, "bad backend: %s:%d: connect to %s %s\n", ssl->hostname, port, hostname, gai_strerror(ret));
				send_alert(ssl);
				return;
			}

			ssl->state = ssl_state_backend_selected;
			free(ssl->saved_buf); // avoid memory leak with malcious client
			ssl->saved_buf = xmalloc(len);
			memcpy(ssl->saved_buf, buf, len);
			ssl->buflen = len;
			connect_backend(ssl, res);

			return;
		}
	}
err:
	send_alert(ssl);
}

static void
greet_cb(EV_P_ ev_io *w, int revents)
{
	unsigned char buf[8192];
	int r;
	struct ssl_session *ssl = w->data;

	ev_timer_stop(loop, &ssl->tm);
	r = read(w->fd, buf, sizeof (buf));

	if (r <= 0) {
		terminate_session(ssl);
	}
	else {
		parse_ssl_greeting(ssl, buf, r);
	}
}

/*
 * Called if client failed to send SSL greeting in time
 */
static void
timer_cb(EV_P_ ev_timer *w, int revents)
{
	struct ssl_session *ssl = w->data;

	ev_timer_stop(loop, &ssl->tm);
	terminate_session(ssl);
}

static int
accept_from_socket(int sock)
{
	int nfd, serrno, ofl;

	if ((nfd = accept (sock, NULL, 0)) == -1) {
		if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK) {
			return 0;
		}

		return -1;
	}

	ofl = fcntl(sock, F_GETFL, 0);

	if (fcntl(sock, F_SETFL, ofl | O_NONBLOCK) == -1) {
		close(sock);

		goto out;
	}

	/* Set close on exec */
	if (fcntl (nfd, F_SETFD, FD_CLOEXEC) == -1) {
		fprintf(stderr, "fcntl failed: %d, '%s'\n", errno, strerror (errno));
		goto out;
	}

	return (nfd);

out:
	serrno = errno;
	close (nfd);
	errno = serrno;

	return (-1);

}

static void
accept_cb(EV_P_ ev_io *w, int revents)
{
	int nfd;
	struct ssl_session *ssl;

	if ((nfd = accept_from_socket(w->fd)) > 0) {
		ssl = xmalloc0(sizeof(*ssl));
		ssl->io.data = ssl;
		ssl->backends = w->data;
		ssl->loop = loop;
		ssl->fd = nfd;
		ssl->bk_fd = -1;
		/* TLS 1.0 (SSL 3.1) */
		ssl->ssl_version[0] = 0x3;
		ssl->ssl_version[0] = 0x1;
		ev_io_init(&ssl->io, greet_cb, nfd, EV_READ);
		ev_io_start(loop, &ssl->io);
		ssl->tm.data = ssl;
		ev_timer_init(&ssl->tm, timer_cb, 2.0, 1);
		ev_timer_start(loop, &ssl->tm);
	}
	else {
		fprintf(stderr, "accept failed: %d, '%s'\n", errno, strerror (errno));
	}
}

static int
listen_on(const struct sockaddr *sa, socklen_t slen)
{
	int sock, on = 1, ofl;

	sock = socket(sa->sa_family, SOCK_STREAM, 0);

	if (sock == -1) {
		return -1;
	}

	if (fcntl(sock, F_SETFD, FD_CLOEXEC) == -1) {
		close(sock);

		return -1;
	}

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const void *)&on, sizeof (int));
	ofl = fcntl(sock, F_GETFL, 0);

	if (fcntl(sock, F_SETFL, ofl | O_NONBLOCK) == -1) {
		close(sock);

		return -1;
	}

	if (bind(sock, sa, slen) == -1) {
		close(sock);

		return -1;
	}

	if (listen(sock, -1) == -1) {
		close(sock);

		return -1;
	}

	return sock;
}

bool
start_listen(struct ev_loop *loop, int port, const ucl_object_t *backends)
{
	struct addrinfo ai, *res, *cur_ai;
	int sock, r, i;
	ev_io *watcher;
	bool ret = false;

	memset(&ai, 0, sizeof(ai));
	ai.ai_family = AF_UNSPEC;
	ai.ai_flags = AI_PASSIVE|AI_NUMERICSERV;
	ai.ai_socktype = SOCK_STREAM;

	if ((r = getaddrinfo(NULL, port_to_str(port), &ai, &res)) != 0) {
		fprintf(stderr, "getaddrinfo: *:%d: %s\n", port, gai_strerror(r));
		return false;
	}

	for (i = 0; i < 2; i++) {
		for (cur_ai = res; cur_ai != NULL; cur_ai = cur_ai->ai_next) {
			/* try IPv6 first */
			if (i == 0 && cur_ai->ai_family != AF_INET6) continue;
			if (i == 1 && cur_ai->ai_family == AF_INET6) continue;

			sock = listen_on(cur_ai->ai_addr, cur_ai->ai_addrlen);
	
			if (sock == -1) {
				fprintf(stderr, "socket listen: %s\n", strerror(errno));
				continue;
			}
	
			watcher = xmalloc0(sizeof(*watcher));
			watcher->data = (void *)backends;
			ev_io_init(watcher, accept_cb, sock, EV_READ);
			ev_io_start(loop, watcher);
			ret = true;
		}

		if (ret) break;
	}

	return ret;
}
