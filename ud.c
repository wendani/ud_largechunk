/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdint.h>

#include "pingpong.h"

#define GRH_SIZE    40

//#define DEBUG_DUMP_RECV
//#define DEBUG_DATA_POP_VARIATION
//#define DEBUG_SEND_DATA_CHANGE

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

static int page_size;

struct pingpong_context {
	struct ibv_context	*context;
	struct ibv_comp_channel *channel;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr;
	struct ibv_cq		*cq;
	struct ibv_qp		*qp;
	struct ibv_ah		*ah;
	void            *grh_buf;
	void			*buf;
	unsigned long long			 size;
	int			 send_flags;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo;
	int             mtu;
	struct ibv_mr   *mr_grh;
	void            *buf_curr;
	int             rx_refill_thrshld;
	int             tx_depth;
	int             tx_refill_thrshld;
};

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};


static void pp_dump_grh(struct ibv_grh *grh)
{
    char sgid[40] = {0};
    char dgid[40] = {0};
    uint32_t vtf = ntohl(grh->version_tclass_flow);
    inet_ntop(AF_INET6, &grh->sgid, sgid, sizeof(sgid));
    inet_ntop(AF_INET6, &grh->dgid, dgid, sizeof(dgid));
    printf("GRH: IPVer 0x%01x  TClass 0x%02x  FlowLabel 0x%05x  PayLen %u  NxtHdr 0x%02x  HopLmt 0x%02x  SGID %016lx:%016lx  SGID %s  DGID %016lx:%016lx  DGID %s\n",
            vtf >> 28, (vtf & 0xFF00000) >> 20, vtf & 0xFFFFF, ntohs(grh->paylen), grh->next_hdr, grh->hop_limit,
            grh->sgid.global.subnet_prefix, grh->sgid.global.interface_id, sgid, grh->dgid.global.subnet_prefix, grh->dgid.global.interface_id, dgid);
}

static inline void pp_sdump_data(void *buf, int size)
{
    printf("%.*s\n", size, (char *)buf);
}


static void pp_pop_data(void *buf, unsigned long long size, int mtu)
{

    int val = 65;
#ifdef DEBUG_DATA_POP_VARIATION
    val = val + rand() % 26;
    printf("pp_pop_data(): val = %d\n", val);
#endif

    if (size > mtu) {
        int i = 0;
        int cnt = size / mtu;

        for (i = 0; i < cnt; ++i) {
            memset(buf, val, mtu);

            buf = buf + mtu;
            val++;
            if (val == 91)
                val = 65;
        }
        if ((cnt = size % mtu))
            memset(buf, val, cnt);
    }
    else
        memset(buf, val, size);
}


static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  int sl, struct pingpong_dest *dest, int sgid_idx)
{
	struct ibv_ah_attr ah_attr = {
		.is_global     = 0,
		.dlid          = dest->lid,
		.sl            = sl,
		.src_path_bits = 0,
		.port_num      = port
	};
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR
	};

	if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.sq_psn	    = my_psn;

	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_SQ_PSN)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	if (dest->gid.global.interface_id) {
		ah_attr.is_global = 1;
		ah_attr.grh.hop_limit = 1;
		ah_attr.grh.dgid = dest->gid;
		ah_attr.grh.sgid_index = sgid_idx;
	}

	ctx->ah = ibv_create_ah(ctx->pd, &ah_attr);
	if (!ctx->ah) {
		fprintf(stderr, "Failed to create AH\n");
		return 1;
	}

	return 0;
}

static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client read");
		fprintf(stderr, "Couldn't read remote address\n");
		goto out;
	}

	write(sockfd, "done", sizeof "done");

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

out:
	close(sockfd);
	return rem_dest;
}

static struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
						 int ib_port, int port, int sl,
						 const struct pingpong_dest *my_dest,
						 int sgid_idx)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1, connfd;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return NULL;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);
	close(sockfd);
	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return NULL;
	}

	n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

	if (pp_connect_ctx(ctx, ib_port, my_dest->psn, sl, rem_dest,
								sgid_idx)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(connfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	read(connfd, msg, sizeof msg);

out:
	close(connfd);
	return rem_dest;
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, unsigned long long size,
					    int rx_depth, int port,
					    int use_event, int tx_depth)
{
	struct pingpong_context *ctx;

	ctx = malloc(sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size       = size;
	ctx->send_flags = IBV_SEND_SIGNALED;
	ctx->rx_depth   = rx_depth;
	ctx->rx_refill_thrshld = 3 * rx_depth / 4;
	ctx->tx_depth   = tx_depth;
	ctx->tx_refill_thrshld = tx_depth >> 2;

	ctx->grh_buf = memalign(page_size, GRH_SIZE);
    if (!ctx->grh_buf) {
        fprintf(stderr, "Couldn't allocate grh buf.\n");
        goto clean_ctx;
    }
	ctx->buf = memalign(page_size, size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_grh_buffer;
	}

	/* FIXME memset(ctx->buf, 0, size + 40); */
//	memset(ctx->buf, 0x7b, size + 40);
	memset(ctx->grh_buf, 0, GRH_SIZE);
	memset(ctx->buf, 0, size);
	ctx->buf_curr = ctx->buf;

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		goto clean_buffer;
	}

	{
		struct ibv_port_attr port_info = { 0 };
		int mtu;

		if (ibv_query_port(ctx->context, port, &port_info)) {
			fprintf(stderr, "Unable to query port info for port %d\n", port);
			goto clean_device;
		}
		mtu = 1 << (port_info.active_mtu + 7);
//		if (size > mtu) {
//			fprintf(stderr, "Requested size larger than port MTU (%d)\n", mtu);
//			goto clean_device;
//		}
		printf("Use mtu size %d\n", mtu);
		ctx->mtu = mtu;
	}

	if (use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			goto clean_device;
		}
	} else
		ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}

	ctx->mr_grh = ibv_reg_mr(ctx->pd, ctx->grh_buf, GRH_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr_grh) {
        fprintf(stderr, "Couldn't register MR for GRH\n");
        goto clean_pd;
    }
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clean_mr_grh;
	}

	ctx->cq = ibv_create_cq(ctx->context, rx_depth + tx_depth, NULL,
				ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_mr;
	}

	{
		struct ibv_qp_attr attr;
		struct ibv_qp_init_attr init_attr = {
			.send_cq = ctx->cq,
			.recv_cq = ctx->cq,
			.cap     = {
				.max_send_wr  = tx_depth,
				.max_recv_wr  = rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 2
			},
			.qp_type = IBV_QPT_UD,
		};

		ctx->qp = ibv_create_qp(ctx->pd, &init_attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
		}

		ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
		if (init_attr.cap.max_inline_data >= size) {
			ctx->send_flags |= IBV_SEND_INLINE;
		}
	}

	{
		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qkey            = 0x11111111
		};

		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_QKEY)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}
	}

	return ctx;

clean_qp:
	ibv_destroy_qp(ctx->qp);

clean_cq:
	ibv_destroy_cq(ctx->cq);

clean_mr:
	ibv_dereg_mr(ctx->mr);

clean_mr_grh:
    ibv_dereg_mr(ctx->mr_grh);

clean_pd:
	ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);

clean_device:
	ibv_close_device(ctx->context);

clean_buffer:
	free(ctx->buf);

clean_grh_buffer:
    free(ctx->grh_buf);

clean_ctx:
	free(ctx);

	return NULL;
}

int pp_close_ctx(struct pingpong_context *ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

    if (ibv_dereg_mr(ctx->mr_grh)) {
        fprintf(stderr, "Couldn't deregister MR for GRH\n");
        return 1;
    }

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ibv_destroy_ah(ctx->ah)) {
		fprintf(stderr, "Couldn't destroy AH\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

static int pp_post_recv(struct pingpong_context *ctx, int data_size)
{
//    if (data_size < 0) {
//        fprintf(stderr, "post recv data size < 0\n");
//        return 1;
//    }
//
//    if (data_size > ctx->mtu) {
//        fprintf(stderr, "post recv data size larger than port MTU (%d)\n", ctx->mtu);
//        return 1;
//    }

	struct ibv_sge list[2] = {
	        {
	                .addr   = (uintptr_t) ctx->grh_buf,
	                .length = GRH_SIZE,
	                .lkey   = ctx->mr_grh->lkey
	        },
	        {
	                .addr	= (uintptr_t) ctx->buf_curr,
	                .length = data_size,
	                .lkey	= ctx->mr->lkey
	        }
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = list,
		.num_sge    = 2,
	};
	struct ibv_recv_wr *bad_wr;
//	int i;

//	for (i = 0; i < n; ++i)
//		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
//			break;

//	return i;

	int ret;
	if ((ret = ibv_post_recv(ctx->qp, &wr, &bad_wr)))
	    return ret;

	ctx->buf_curr += data_size;
	return 0;
}

static int pp_post_send(struct pingpong_context *ctx, uint32_t qpn, int data_size)
{
//    if (data_size < 0) {
//        fprintf(stderr, "post send data size < 0\n");
//        return 1;
//    }
//
//    if (data_size > ctx->mtu) {
//        fprintf(stderr, "post send data size larger than port MTU (%d)\n", ctx->mtu);
//        return 1;
//    }

	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf_curr,
		.length = data_size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_SEND,
		.send_flags = ctx->send_flags,
		.wr         = {
			.ud = {
				 .ah          = ctx->ah,
				 .remote_qpn  = qpn,
				 .remote_qkey = 0x11111111
			 }
		}
	};
	struct ibv_send_wr *bad_wr;
	int ret;

	if ((ret = ibv_post_send(ctx->qp, &wr, &bad_wr)))
	    return ret;

	ctx->buf_curr += data_size;
	return 0;
}

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>         listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>        use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>      use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>         size of message to exchange (default 2048)\n");
	printf("  -r, --rx-depth=<dep>      number of receives to post at a time (default 500)\n");
	printf("  -n, --iters=<iters>       number of exchanges (default 1)\n");
	printf("  -l, --sl=<sl>             service level value\n");
	printf("  -e, --events              sleep on CQ events (default poll)\n");
	printf("  -g, --gid-idx=<gid index> local port gid index\n");
	printf("  -t, --tx_depth=<dep>      number of sends to post at a time (default 50)\n");
}

int main(int argc, char *argv[])
{
	struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct pingpong_context *ctx;
	struct pingpong_dest     my_dest;
	struct pingpong_dest    *rem_dest;
	struct timeval           start, end;
	char                    *ib_devname = NULL;
	char                    *servername = NULL;
	unsigned int             port = 18515;
	int                      ib_port = 1;
	unsigned long long      size = 2048;
	unsigned int             rx_depth = 500;
	unsigned int             iters = 1;
	int                      use_event = 0;
	int                      routs;
	int                      rcnt, scnt;
	int                      num_cq_events = 0;
	int                      sl = 0;
	int			 gidx = -1;
	char			 gid[33];
	unsigned int            cnt;
	unsigned int            iter_cnt;
	int                     rmnder;
	unsigned int            tx_depth = 50;
	int                     touts;
	int                     touts_in_msg;
	int                     touts_in_mtu;
	unsigned int            size_in_mtu;
	int                     routs_in_msg;
	int                     routs_in_mtu;

	srand48(getpid() * time(NULL));

	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",     .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",   .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",  .has_arg = 1, .val = 'i' },
			{ .name = "size",     .has_arg = 1, .val = 's' },
			{ .name = "rx-depth", .has_arg = 1, .val = 'r' },
			{ .name = "iters",    .has_arg = 1, .val = 'n' },
			{ .name = "sl",       .has_arg = 1, .val = 'l' },
			{ .name = "events",   .has_arg = 0, .val = 'e' },
			{ .name = "gid-idx",  .has_arg = 1, .val = 'g' },
			{ .name = "tx-depth", .has_arg = 1, .val = 't' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:s:r:n:l:eg:t:",
							long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			port = strtol(optarg, NULL, 0);
			if (port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			ib_devname = strdupa(optarg);
			break;

		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 1) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 's':
			size = strtoul(optarg, NULL, 0);
			break;

		case 'r':
			rx_depth = strtoul(optarg, NULL, 0);
			break;

		case 'n':
			iters = strtoul(optarg, NULL, 0);
			break;

		case 'l':
			sl = strtol(optarg, NULL, 0);
			break;

		case 'e':
			++use_event;
			break;

		case 'g':
			gidx = strtol(optarg, NULL, 0);
			break;

		case 't':
		    tx_depth = strtoul(optarg, NULL, 0);
		    printf("tx_depth = %u\n", tx_depth);
		    break;

		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind == argc - 1)
		servername = strdupa(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	page_size = sysconf(_SC_PAGESIZE);

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return 1;
	}

	if (!ib_devname) {
		ib_dev = *dev_list;
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return 1;
		}
	} else {
		int i;
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}

	ctx = pp_init_ctx(ib_dev, size, rx_depth, ib_port, use_event, tx_depth);
	if (!ctx)
		return 1;


	size_in_mtu = size / ctx->mtu;
	if ((rmnder = size % ctx->mtu))
	    ++size_in_mtu;

	if (!servername) {
	    routs = 0;
	    routs_in_msg = 0;
	    routs_in_mtu = 0;
	    while (routs < ctx->rx_depth) {
	        ++routs;
	        ++routs_in_mtu;

	        if (routs_in_mtu < size_in_mtu) {
	            if (pp_post_recv(ctx, ctx->mtu)) {
	                fprintf(stderr, "Couldn't post receive (%d)\n", routs);
	                return 1;
	            }
	        }
	        else {
	            // routs_in_mtu == size_in_mtu
	            if (rmnder) {
	                if (pp_post_recv(ctx, rmnder)) {
	                    fprintf(stderr, "Couldn't post receive (%d)\n", routs);
	                    return 1;
	                }
	            }
	            else {
	                if (pp_post_recv(ctx, ctx->mtu)) {
	                    fprintf(stderr, "Couldn't post receive (%d)\n", routs);
	                    return 1;
	                }
	            }
	            ++routs_in_msg;
	            routs_in_mtu = 0;
	            ctx->buf_curr = ctx->buf;
	            if (routs_in_msg == iters) break;
	        }
	    }
	}

	if (use_event)
		if (ibv_req_notify_cq(ctx->cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		}

	if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return 1;
	}
	my_dest.lid = ctx->portinfo.lid;

	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;

	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
			fprintf(stderr, "Could not get local gid for gid index "
								"%d\n", gidx);
			return 1;
		}
	} else
		memset(&my_dest.gid, 0, sizeof my_dest.gid);

	printf("GID %016lx:%016lx\n", my_dest.gid.global.subnet_prefix, my_dest.gid.global.interface_id);
	inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x: GID %s\n",
	       my_dest.lid, my_dest.qpn, my_dest.psn, gid);

	if (servername)
		rem_dest = pp_client_exch_dest(servername, port, &my_dest);
	else
		rem_dest = pp_server_exch_dest(ctx, ib_port, port, sl,
							&my_dest, gidx);

	if (!rem_dest)
		return 1;

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

	if (servername) {
		if (pp_connect_ctx(ctx, ib_port, my_dest.psn, sl, rem_dest,
									gidx))
			return 1;

		// populate data payload
		pp_pop_data(ctx->buf, size, ctx->mtu);
	}

	if (gettimeofday(&start, NULL)) {
		perror("gettimeofday");
		return 1;
	}

	if (servername) {
	    touts = 0;
	    touts_in_msg = 0;
	    touts_in_mtu = 0;
	    while (touts < ctx->tx_depth) {
	        ++touts;
	        ++touts_in_mtu;

	        if (touts_in_mtu < size_in_mtu) {
	            if (pp_post_send(ctx, rem_dest->qpn, ctx->mtu)) {
	                fprintf(stderr, "Couldn't post send\n");
	                return 1;
	            }
	        }
	        else {
	            // touts_in_mtu == size_in_mtu
	            if (rmnder) {
	                if (pp_post_send(ctx, rem_dest->qpn, rmnder)) {
	                    fprintf(stderr, "Couldn't post send\n");
	                    return 1;
	                }
	            }
	            else {
	                if (pp_post_send(ctx, rem_dest->qpn, ctx->mtu)) {
	                    fprintf(stderr, "Couldn't post send\n");
	                    return 1;
	                }
	            }
	            ++touts_in_msg;
	            touts_in_mtu = 0;
	            ctx->buf_curr = ctx->buf;
	            if (touts_in_msg == iters) break;
	        }
	    }
	}

	// completion events processing
	cnt = 0;
	iter_cnt = 0;
	while (iter_cnt < iters) {
	    if (use_event) {
	        struct ibv_cq *ev_cq;
	        void          *ev_ctx;

	        if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
	            fprintf(stderr, "Failed to get cq_event\n");
	            return 1;
	        }

	        ++num_cq_events;

	        if (ev_cq != ctx->cq) {
	            fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
	            return 1;
	        }

	        if (ibv_req_notify_cq(ctx->cq, 0)) {
	            fprintf(stderr, "Couldn't request CQ notification\n");
	            return 1;
	        }
	    }

	    {
	        struct ibv_wc wc[2];
	        int ne, i;

	        do {
	            ne = ibv_poll_cq(ctx->cq, 2, wc);
	            if (ne < 0) {
	                fprintf(stderr, "poll CQ failed %d\n", ne);
	                return 1;
	            }
	        } while (!use_event && ne < 1);

	        for (i = 0; i < ne; ++i) {
	            if (wc[i].status != IBV_WC_SUCCESS) {
	                fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
	                        ibv_wc_status_str(wc[i].status),
	                        wc[i].status, (int) wc[i].wr_id);
	                return 1;
	            }

	            switch ((int) wc[i].wr_id) {
	            case PINGPONG_SEND_WRID:
	                --touts;
	                ++cnt;
	                if (!touts_in_msg) {
	                    --touts_in_mtu;
	                }
	                else {
	                    // touts_in_msg != 0
	                    if (cnt == size_in_mtu) {
	                        --touts_in_msg;
	                        ++iter_cnt;
	                        cnt = 0;
	                    }
	                }
	                if (touts <= ctx->tx_refill_thrshld) {
	                    unsigned int i = touts_in_msg ? touts_in_mtu : cnt + touts_in_mtu;
	                    // the second condition is placed in the case of shallow tx_depth that cannot hold a message
	                    // In such a case, the first condition is always true even when enough sends have been posted
	                    if ((touts_in_msg + iter_cnt < iters) && (i < size_in_mtu)) {
	                        while (touts < ctx->tx_depth) {
	                            ++touts;
	                            ++touts_in_mtu;
	                            ++i;
	                            if (i < size_in_mtu) {
	                                if (pp_post_send(ctx, rem_dest->qpn, ctx->mtu)) {
	                                    fprintf(stderr, "Couldn't post send\n");
	                                    return 1;
	                                }
	                            }
	                            else {
	                                // i == size_in_mtu
	                                if (rmnder) {
	                                    if (pp_post_send(ctx, rem_dest->qpn, rmnder)) {
	                                        fprintf(stderr, "Couldn't post send\n");
	                                        return 1;
	                                    }
	                                }
	                                else {
	                                    if (pp_post_send(ctx, rem_dest->qpn, ctx->mtu)) {
	                                        fprintf(stderr, "Couldn't post send\n");
	                                        return 1;
	                                    }
	                                }
	                                ++touts_in_msg;
	                                touts_in_mtu = 0;
	                                i = 0;
	                                ctx->buf_curr = ctx->buf;
#ifdef DEBUG_SEND_DATA_CHANGE
	                                pp_pop_data(ctx->buf, size, ctx->mtu);
#endif
	                            }
	                        }
	                    }
	                }
	                break;

	            case PINGPONG_RECV_WRID:
#ifdef DEBUG_DUMP_RECV
	                pp_dump_grh(ctx->grh_buf);
	                if (cnt < size_in_mtu)
	                    pp_sdump_data(ctx->buf + cnt * ctx->mtu, ctx->mtu);
	                else
	                    pp_sdump_data(ctx->buf + cnt * ctx->mtu, rmnder);
#endif
	                --routs;
	                ++cnt;
	                if (!routs_in_msg) {
	                    --routs_in_mtu;
	                }
	                else {
	                    // routs_in_msg != 0
	                    if (cnt == size_in_mtu) {
	                        --routs_in_msg;
	                        ++iter_cnt;
	                        cnt = 0;
	                    }
	                }
	                if (routs <= ctx->rx_refill_thrshld) {
	                    unsigned int i = routs_in_msg ? routs_in_mtu : cnt + routs_in_mtu;
	                    // the second condition is placed in the case of shallow rx_depth that cannot hold a message
	                    // In such a case, the first condition is always true even when enough receives have been posted
	                    if ((routs_in_msg + iter_cnt < iters) && (i < size_in_mtu)) {
	                        while (routs < ctx->rx_depth) {
	                            ++routs;
	                            ++routs_in_mtu;
	                            ++i;
	                            if (i < size_in_mtu) {
	                                if (pp_post_recv(ctx, ctx->mtu)) {
	                                    fprintf(stderr, "Couldn't post receive (%d)\n", routs);
	                                    return 1;
	                                }
	                            }
	                            else {
	                                // i == size_in_mtu
	                                if (rmnder) {
	                                    if (pp_post_recv(ctx, rmnder)) {
	                                        fprintf(stderr, "Couldn't post receive (%d)\n", routs);
	                                        return 1;
	                                    }
	                                }
	                                else {
	                                    if (pp_post_recv(ctx, ctx->mtu)) {
	                                        fprintf(stderr, "Couldn't post receive (%d)\n", routs);
	                                        return 1;
	                                    }
	                                }
	                                ++routs_in_msg;
	                                routs_in_mtu = 0;
	                                i = 0;
	                                ctx->buf_curr = ctx->buf;
	                            }
	                        }
	                    }
	                }
	                break;

	            default:
	                fprintf(stderr, "Completion for unknown wr_id %d\n",
	                        (int) wc[i].wr_id);
	                return 1;
	            }
	        }
	    }
	}

	if (gettimeofday(&end, NULL)) {
		perror("gettimeofday");
		return 1;
	}

	{
		float usec = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_usec - start.tv_usec);
//		printf("size_in_mtu = %u, mtu = %d, iter_cnt = %u\n ", size_in_mtu, ctx->mtu, iter_cnt);
		unsigned long long bytes = rmnder ? ((unsigned long long)(size_in_mtu - 1) * ctx->mtu + rmnder) * iter_cnt
		        : ((unsigned long long)size_in_mtu * ctx->mtu) * iter_cnt;

		printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
		       bytes, usec / 1000000., bytes * 8. / usec);
		printf("%d iters in %.2f seconds = %.2f usec/iter\n",
		       iters, usec / 1000000., usec / iters);
	}

	ibv_ack_cq_events(ctx->cq, num_cq_events);

	if (pp_close_ctx(ctx))
		return 1;

	ibv_free_device_list(dev_list);
	free(rem_dest);

	return 0;
}
