/*
 * iSCSI I/O Library
 *
 * Copyright (C) 2002 Cisco Systems, Inc.
 * maintained by linux-iscsi-devel@lists.sourceforge.net
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/param.h>

#include "iscsi_proto.h"
#include "initiator.h"
#include "log.h"

#define LOG_CONN_CLOSED(conn) \
	log_error("Connection to Discovery Address %u.%u.%u.%u closed", conn->ip_address[0], conn->ip_address[1], conn->ip_address[2], conn->ip_address[3])
#define LOG_CONN_FAIL(conn) \
	log_error("Connection to Discovery Address %u.%u.%u.%u failed", conn->ip_address[0], conn->ip_address[1], conn->ip_address[2], conn->ip_address[3])

static int timedout;

static void
sigalarm_handler(int unused)
{
	timedout = 1;
}

static void
set_non_blocking(int fd)
{
	int res = fcntl(fd, F_GETFL);

	if (res != -1) {
		res = fcntl(fd, F_SETFL, res | O_NONBLOCK);
		if (res)
			log_warning("unable to set fd flags (%s)!",
				    strerror(errno));
	} else
		log_warning("unable to get fd flags (%s)!", strerror(errno));

}

int
iscsi_tcp_connect(iscsi_conn_t *conn, int non_blocking)
{
	int rc, onearg;

	/* create a socket */
	conn->socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (conn->socket_fd < 0) {
		log_error("cannot create TCP socket");
		return -1;
	}

	onearg = 1;
	rc = setsockopt(conn->socket_fd, IPPROTO_TCP, TCP_NODELAY, &onearg,
			sizeof (onearg));
	if (rc < 0) {
		log_error("cannot set TCP_NODELAY option on socket");
		close(conn->socket_fd);
		conn->socket_fd = -1;
		return rc;
	}

	/* optionally set the window sizes */
	if (conn->tcp_window_size) {
		int window_size = conn->tcp_window_size;
		socklen_t arglen = sizeof (window_size);

		if (setsockopt(conn->socket_fd, SOL_SOCKET, SO_RCVBUF,
		       (char *) &window_size, sizeof (window_size)) < 0) {
			log_warning("failed to set TCP recv window size "
				    "to %u", window_size);
		} else {
			if (getsockopt(conn->socket_fd, SOL_SOCKET, SO_RCVBUF,
				       (char *) &window_size, &arglen) >= 0) {
				log_debug(4, "set TCP recv window size to %u, "
					  "actually got %u",
					  conn->tcp_window_size, window_size);
			}
		}

		window_size = conn->tcp_window_size;
		arglen = sizeof (window_size);

		if (setsockopt(conn->socket_fd, SOL_SOCKET, SO_SNDBUF,
		       (char *) &window_size, sizeof (window_size)) < 0) {
			log_warning("failed to set TCP send window size "
				    "to %u", window_size);
		} else {
			if (getsockopt(conn->socket_fd, SOL_SOCKET, SO_SNDBUF,
				       (char *) &window_size, &arglen) >= 0) {
				log_debug(4, "set TCP send window size to %u, "
					  "actually got %u",
					  conn->tcp_window_size, window_size);
			}
		}
	}

	/*
	 * Build a TCP connection to the target
	 */
	memset(&conn->addr, 0, sizeof (conn->addr));
	conn->addr.sin_family = AF_INET;
	conn->addr.sin_port = htons(conn->port);
	memcpy(&conn->addr.sin_addr.s_addr, conn->ip_address,
	       MIN(sizeof (conn->addr.sin_addr.s_addr), conn->ip_length));
	log_debug(1, "connecting to %s:%d ip_length %d",
		  inet_ntoa(conn->addr.sin_addr), conn->port, conn->ip_length);
	if (non_blocking)
		set_non_blocking(conn->socket_fd);
	rc = connect(conn->socket_fd, (struct sockaddr *) &conn->addr,
		     sizeof (conn->addr));
	return rc;
}

int
iscsi_tcp_poll(iscsi_conn_t *conn)
{
	int rc;
	struct pollfd pdesc;

	pdesc.fd = conn->socket_fd;
	pdesc.events = POLLOUT;
	rc = poll(&pdesc, 1, 1);
	if (rc < 0) {
		log_error("cannot make connection to %s:%d (%d)",
			 inet_ntoa(conn->addr.sin_addr), conn->port, errno);
		close(conn->socket_fd);
		conn->socket_fd = -1;
	} else if (rc > 0 && log_level > 0) {
		struct sockaddr_in local;
		socklen_t len = sizeof (local);

		if (getsockname(conn->socket_fd, (struct sockaddr *) &local,
				&len) >= 0) {
			log_debug(1, "connected local port %d to %s:%d",
				 ntohs(local.sin_port),
				 inet_ntoa(conn->addr.sin_addr), conn->port);
		}
	}
	return rc;
}

int
iscsi_connect(iscsi_conn_t *conn)
{
	int rc, ret;
	struct sigaction action;
	struct sigaction old;

	/* set a timeout, since the socket calls may take a long time to
	 * timeout on their own
	 */
	memset(&action, 0, sizeof (struct sigaction));
	memset(&old, 0, sizeof (struct sigaction));
	action.sa_sigaction = NULL;
	action.sa_flags = 0;
	action.sa_handler = sigalarm_handler;
	sigaction(SIGALRM, &action, &old);
	timedout = 0;
	alarm(conn->login_timeout);

	/* perform blocking TCP connect operation when no async request
	 * associated. SendTargets Discovery know to work in such a mode.
	 */
	rc = iscsi_tcp_connect(conn, 0);
	if (timedout) {
		log_debug(1, "socket %d connect timed out", conn->socket_fd);
		ret = 0;
		goto done;
	} else if (rc < 0) {
		log_error("cannot make connection to %s:%d (%d)",
			 inet_ntoa(conn->addr.sin_addr), conn->port, errno);
		close(conn->socket_fd);
		ret = 0;
		goto done;
	} else if (log_level > 0) {
		struct sockaddr_in local;
		socklen_t len = sizeof (local);

		if (getsockname(conn->socket_fd, (struct sockaddr *) &local,
				&len) >= 0) {
			log_debug(1, "connected local port %d to %s:%d",
				 ntohs(local.sin_port),
				 inet_ntoa(conn->addr.sin_addr), conn->port);
		}
	}

	ret = 1;

done:
	alarm(0);
	sigaction(SIGALRM, &old, NULL);
	return ret;
}

void
iscsi_disconnect(iscsi_conn_t *conn)
{
	if (conn->socket_fd >= 0) {
		log_debug(1, "disconnecting conn %p, fd %d", conn,
			 conn->socket_fd);
		close(conn->socket_fd);
		conn->socket_fd = -1;
	}
}

static void
iscsi_log_text(iscsi_hdr_t *pdu, char *data)
{
	int dlength = ntoh24(pdu->dlength);
	char *text = data;
	char *end = text + dlength;

	while (text && (text < end)) {
		log_debug(4, ">    %s", text);
		text += strlen(text);
		while ((text < end) && (*text == '\0'))
			text++;
	}
}

int
iscsi_send_pdu(iscsi_conn_t *conn, iscsi_hdr_t *hdr,
	       int hdr_digest, char *data, int data_digest, int timeout)
{
	int rc, ret = 0;
	char *header = (char *) hdr;
	char *end;
	char pad[4];
	struct iovec vec[3];
	int pad_bytes;
	int pdu_length = sizeof (*hdr) + hdr->hlength + ntoh24(hdr->dlength);
	int remaining;
	struct sigaction action;
	struct sigaction old;

	/* set a timeout, since the socket calls may take a long time
	 * to timeout on their own
	 */
	if (!conn->kernel_io) {
		memset(&action, 0, sizeof (struct sigaction));
		memset(&old, 0, sizeof (struct sigaction));
		action.sa_sigaction = NULL;
		action.sa_flags = 0;
		action.sa_handler = sigalarm_handler;
		sigaction(SIGALRM, &action, &old);
		timedout = 0;
		alarm(timeout);
	}

	memset(&pad, 0, sizeof (pad));
	memset(&vec, 0, sizeof (vec));

	if (log_level > 0) {
		switch (hdr->opcode & ISCSI_OPCODE_MASK) {
		case ISCSI_OP_LOGIN:{
				struct iscsi_login *login_hdr =
				    (struct iscsi_login_hdr *) hdr;

				log_debug(4,
					 "sending login PDU with current stage "
					 "%d, next stage %d, transit 0x%x, isid"
					 " 0x%02x%02x%02x%02x%02x%02x",
					 ISCSI_LOGIN_CURRENT_STAGE(login_hdr->
								   flags),
					 ISCSI_LOGIN_NEXT_STAGE(login_hdr->
								flags),
					 login_hdr->
					 flags & ISCSI_FLAG_LOGIN_TRANSIT,
					 login_hdr->isid[0], login_hdr->isid[1],
					 login_hdr->isid[2], login_hdr->isid[3],
					 login_hdr->isid[4],
					 login_hdr->isid[5]);
				iscsi_log_text(hdr, data);
				break;
			}
		case ISCSI_OP_TEXT:{
				struct iscsi_text *text_hdr =
				    (struct iscsi_txt_hdr *) hdr;

				log_debug(4,
					 "sending text pdu with itt %u, "
					 "CmdSN %u:",
					 ntohl(text_hdr->itt),
					 ntohl(text_hdr->cmdsn));
				iscsi_log_text(hdr, data);
				break;
			}
		case ISCSI_OP_NOOP_OUT:{
				struct iscsi_nopout *nopout_hdr =
				    (struct iscsi_nop_out_hdr *) hdr;

				log_debug(4,
					 "sending Nop-out pdu with itt %u, "
					 "ttt %u, CmdSN %u:",
					 ntohl(nopout_hdr->itt),
					 ntohl(nopout_hdr->ttt),
					 ntohl(nopout_hdr->cmdsn));
				iscsi_log_text(hdr, data);
				break;
			}
		default:
			log_debug(4, "sending pdu opcode 0x%x:", hdr->opcode);
			break;
		}
	}

	/* send the PDU header */
	header = (char *) hdr;
	end = header + sizeof (*hdr) + hdr->hlength;

	/* send all the data and any padding */
	if (pdu_length % PAD_WORD_LEN)
		pad_bytes = PAD_WORD_LEN - (pdu_length % PAD_WORD_LEN);
	else
		pad_bytes = 0;

	if (conn->kernel_io) {
		if (conn->send_pdu_begin(conn->session, conn,
			end - header, ntoh24(hdr->dlength) + pad_bytes)) {
			ret = 0;
			goto done;
		}
	}

	while (header < end) {
		vec[0].iov_base = header;
		vec[0].iov_len = end - header;

		rc = writev(conn->kernel_io ? conn->ctrl_fd : conn->socket_fd,
			    vec, 1);
		if (timedout) {
			log_error("socket %d write timed out",
			       conn->socket_fd);
			ret = 0;
			goto done;
		} else if ((rc <= 0) && (errno != EAGAIN)) {
			LOG_CONN_FAIL(conn);
			ret = 0;
			goto done;
		} else if (rc > 0) {
			log_debug(4, "wrote %d bytes of PDU header", rc);
			header += rc;
		}
	}

	end = data + ntoh24(hdr->dlength);
	remaining = ntoh24(hdr->dlength) + pad_bytes;

	while (remaining > 0) {
		vec[0].iov_base = data;
		vec[0].iov_len = end - data;
		vec[1].iov_base = (void *) &pad;
		vec[1].iov_len = pad_bytes;

		rc = writev(conn->kernel_io ? conn->ctrl_fd : conn->socket_fd,
			    vec, 2);
		if (timedout) {
			log_error("socket %d write timed out",
			       conn->socket_fd);
			ret = 0;
			goto done;
		} else if ((rc <= 0) && (errno != EAGAIN)) {
			LOG_CONN_FAIL(conn);
			ret = 0;
			goto done;
		} else if (rc > 0) {
			log_debug(4, "wrote %d bytes of PDU data", rc);
			remaining -= rc;
			if (data < end) {
				data += rc;
				if (data > end)
					data = end;
			}
		}
	}

	if (conn->kernel_io) {
		if (conn->send_pdu_end(conn->session, conn)) {
			ret = 0;
			goto done;
		}
	}

	ret = 1;

      done:
	if (!conn->kernel_io) {
		alarm(0);
		sigaction(SIGALRM, &old, NULL);
		timedout = 0;
	}
	return ret;
}

int
iscsi_recv_pdu(iscsi_conn_t *conn, iscsi_hdr_t *hdr,
	       int hdr_digest, char *data, int max_data_length, int data_digest,
	       int timeout)
{
	uint32_t h_bytes = 0;
	uint32_t ahs_bytes = 0;
	uint32_t d_bytes = 0;
	uint32_t ahslength = 0;
	uint32_t dlength = 0;
	uint32_t pad = 0;
	int rlen = 0;
	int failed = 0;
	char *header = (char *) hdr;
	char *end = data + max_data_length;
	struct sigaction action;
	struct sigaction old;

	/* set a timeout, since the socket calls may take a long
	 * time to timeout on their own
	 */
	memset(data, 0, max_data_length);
	memset(&action, 0, sizeof (struct sigaction));
	memset(&old, 0, sizeof (struct sigaction));
	action.sa_sigaction = NULL;
	action.sa_flags = 0;
	action.sa_handler = sigalarm_handler;
	sigaction(SIGALRM, &action, &old);
	timedout = 0;
	alarm(timeout);

	/* read a response header */
	do {
		rlen =
		    read(conn->socket_fd, header, sizeof (*hdr) - h_bytes);
		if (timedout) {
			log_error("socket %d header read timed out",
			       conn->socket_fd);
			failed = 1;
			goto done;
		} else if (rlen == 0) {
			LOG_CONN_CLOSED(conn);
			failed = 1;
			goto done;
		} else if ((rlen < 0) && (errno != EAGAIN)) {
			LOG_CONN_FAIL(conn);
			failed = 1;
			goto done;
		} else if (rlen > 0) {
			log_debug(4, "read %d bytes of PDU header", rlen);
			header += rlen;
			h_bytes += rlen;
		}
	} while (h_bytes < sizeof (*hdr));

	log_debug(4, "read %d PDU header bytes, opcode 0x%x, dlength %u, "
		 "data %p, max %u", h_bytes, hdr->opcode,
		 ntoh24(hdr->dlength), data, max_data_length);

	/* check for additional headers */
	ahslength = hdr->hlength;	/* already includes padding */
	if (ahslength) {
		log_warning("additional header segment length %u not supported",
		       ahslength);
		failed = 1;
		goto done;
	}

	/* read exactly what we expect, plus padding */
	dlength = hdr->dlength[0] << 16;
	dlength |= hdr->dlength[1] << 8;
	dlength |= hdr->dlength[2];

	/* if we only expected to receive a header, exit */
	if (dlength == 0)
		goto done;

	if (data + dlength >= end) {
		log_warning("buffer size %u too small for data length %u",
		       max_data_length, dlength);
		failed = 1;
		goto done;
	}

	/* read the rest into our buffer */
	d_bytes = 0;
	while (d_bytes < dlength) {
		rlen =
		    read(conn->socket_fd, data + d_bytes, dlength - d_bytes);
		if (timedout) {
			log_error("socket %d data read timed out",
			       conn->socket_fd);
			failed = 1;
			goto done;
		} else if (rlen == 0) {
			LOG_CONN_CLOSED(conn);
			failed = 1;
			goto done;
		} else if ((rlen < 0 && errno != EAGAIN)) {
			LOG_CONN_FAIL(conn);
			failed = 1;
			goto done;
		} else if (rlen > 0) {
			log_debug(4, "read %d bytes of PDU data", rlen);
			d_bytes += rlen;
		}
	}

	/* handle PDU data padding */
	pad = dlength % PAD_WORD_LEN;
	if (pad) {
		int pad_bytes = pad = PAD_WORD_LEN - pad;
		char bytes[PAD_WORD_LEN];

		while (pad_bytes > 0) {
			rlen = read(conn->socket_fd, &bytes, pad_bytes);
			if (timedout) {
				log_error("socket %d pad read timed out",
				       conn->socket_fd);
				failed = 1;
				goto done;
			} else if (rlen == 0) {
				LOG_CONN_CLOSED(conn);
				failed = 1;
				goto done;
			} else if ((rlen < 0 && errno != EAGAIN)) {
				LOG_CONN_FAIL(conn);
				failed = 1;
				goto done;
			} else if (rlen > 0) {
				log_debug(4, "read %d pad bytes", rlen);
				pad_bytes -= rlen;
			}
		}
	}

	if (log_level > 0) {
		switch (hdr->opcode) {
		case ISCSI_OP_TEXT_RSP:
			log_debug(4,
				 "finished reading text PDU, %u hdr, %u "
				 "ah, %u data, %u pad",
				 h_bytes, ahs_bytes, d_bytes, pad);
			iscsi_log_text(hdr, data);
			break;
		case ISCSI_OP_LOGIN_RSP:{
				struct iscsi_login_rsp *login_rsp =
				    (struct iscsi_login_rsp_hdr *) hdr;

				log_debug(4,
					 "finished reading login PDU, %u hdr, "
					 "%u ah, %u data, %u pad",
					 h_bytes, ahs_bytes, d_bytes, pad);
				log_debug(4,
					 "login current stage %d, next stage "
					 "%d, transit 0x%x",
					 ISCSI_LOGIN_CURRENT_STAGE(login_rsp->
								   flags),
					 ISCSI_LOGIN_NEXT_STAGE(login_rsp->
								flags),
					 login_rsp->
					 flags & ISCSI_FLAG_LOGIN_TRANSIT);
				iscsi_log_text(hdr, data);
				break;
			}
		case ISCSI_OP_ASYNC_EVENT:
			/* FIXME: log the event info */
			break;
		default:
			break;
		}
	}

      done:
	alarm(0);
	sigaction(SIGALRM, &old, NULL);
	if (timedout || failed) {
		timedout = 0;
		return 0;
	} else {
		return h_bytes + ahs_bytes + d_bytes;
	}
}