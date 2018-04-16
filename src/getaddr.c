#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>

#include "util.h"
#include "ev.h"
#include "getaddr.h"

struct request {
	struct gaicb gaicb;
	reqCb cb;
	void *cbctx;
	struct addrinfo hints;
	struct gaicb *list[1];
	char bufs[0];
};

static ev_io io;

int send_request(const char *node, const char *service, int socktype, int protocol, int flags, reqCb cb, void *cbctx)
{
	size_t node_len = strlen(node) + 1, service_len = service != NULL ? strlen(service) + 1 : 0;
	struct request *req = xmalloc0(sizeof(*req) + node_len + service_len);

	req->list[0] = &req->gaicb;
	req->gaicb = (struct gaicb){};

	req->gaicb.ar_name = req->bufs;
	memcpy((char *)req->gaicb.ar_name, node, node_len);
	if (service != NULL) {
		req->gaicb.ar_service = req->bufs + node_len;
		memcpy((char *)req->gaicb.ar_service, service, service_len);
	}

	req->gaicb.ar_request = &req->hints;
	req->hints = (struct addrinfo){};
	req->hints.ai_socktype = socktype;
	req->hints.ai_protocol = protocol;
	req->hints.ai_flags = flags;

	req->cb = cb;
	req->cbctx = cbctx;

	struct sigevent sev = {};
	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = SIGRTMIN;
	sev.sigev_value.sival_ptr = req;

	int ret = getaddrinfo_a(GAI_NOWAIT, req->list, 1, &sev);
	return ret; // zero on success
}

static void handle_response(struct request *query)
{
	int ret;
	struct gaicb *req = &query->gaicb;

	if ((ret = gai_error(req)) != 0) {
		if (ret == EAI_INPROGRESS)
			return;
		query->cb(query->cbctx, NULL, ret);
		goto Done;
	}

	// printf("%s:", req->ar_name); // the name queried for
	struct addrinfo *ai = req->ar_result;
	query->cb(query->cbctx, ai, ret);

	freeaddrinfo(req->ar_result);
Done:
	free(query);
}

static int setup_signalfd(void)
{
	sigset_t sigs;

	sigemptyset(&sigs);
	sigaddset(&sigs, SIGRTMIN);
	sigprocmask(SIG_BLOCK, &sigs, NULL);

	return signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);
}

static void handle_sigs_via_fd(int sfd)
{
	struct signalfd_siginfo ssi;
	ssize_t rret;

	do {
		rret = read(sfd, &ssi, sizeof(ssi));
		if (rret == -1 || rret != sizeof(ssi))
			break;

		if (ssi.ssi_code == SI_ASYNCNL) {
			/* received response from getaddrinfo_a */
			struct request *req = (void *)ssi.ssi_ptr;
			handle_response(req);
		} else {
			fprintf(stderr, "received signal with unexpected si_code:%" PRId32 "\n", ssi.ssi_code);
		}
	} while (1);
}

static void
signalfd_cb(EV_P_ ev_io *w, int revents)
{
	handle_sigs_via_fd(w->fd);
}

bool start_signalfd(struct ev_loop *loop)
{
	int fd = setup_signalfd();
	if (fd < 0) return 0;

	ev_io_init(&io, signalfd_cb, fd, EV_READ);
	ev_io_start(loop, &io);

	return 1;
}

