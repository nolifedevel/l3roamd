#include "error.h"
#include "ipmgr.h"
#include "timespec.h"
#include "if.h"
#include "intercom.h"
#include "l3roamd.h"
#include "util.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/if_tun.h>

static bool tun_open(ipmgr_ctx *ctx, const char *ifname, uint16_t mtu, const char *dev_name);
static void ipmgr_ns_task(void *d);
static void seek_task(void *d);

/* open l3roamd's tun device that is used to obtain packets for unknown clients */
bool tun_open(ipmgr_ctx *ctx, const char *ifname, uint16_t mtu, const char *dev_name) {
	int ctl_sock = -1;
	struct ifreq ifr = {};

	ctx->fd = open(dev_name, O_RDWR|O_NONBLOCK);
	if (ctx->fd < 0)
		exit_errno("could not open TUN/TAP device file");

	if (ifname)
		strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

	if (ioctl(ctx->fd, TUNSETIFF, &ifr) < 0) {
		puts("unable to open TUN/TAP interface: TUNSETIFF ioctl failed");
		goto error;
	}

	ctx->ifname = strndup(ifr.ifr_name, IFNAMSIZ-1);

	ctl_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (ctl_sock < 0)
		exit_errno("socket");

	if (ioctl(ctl_sock, SIOCGIFMTU, &ifr) < 0)
		exit_errno("SIOCGIFMTU ioctl failed");

	if (ifr.ifr_mtu != mtu) {
		ifr.ifr_mtu = mtu;
		if (ioctl(ctl_sock, SIOCSIFMTU, &ifr) < 0) {
			puts("unable to set TUN/TAP interface MTU: SIOCSIFMTU ioctl failed");
			goto error;
		}
	}

	ifr.ifr_flags = IFF_UP | IFF_RUNNING| IFF_MULTICAST | IFF_NOARP | IFF_POINTOPOINT;
	if (ioctl(ctl_sock, SIOCSIFFLAGS, &ifr) < 0 ) {
		puts("unable to set TUN/TAP interface UP: SIOCSIFFLAGS ioctl failed");
		goto error;
	}

	if (close(ctl_sock))
		puts("close");

	return true;

error:
	if (ctl_sock >= 0) {
		if (close(ctl_sock))
			puts("close");
	}
	free(ctx->ifname);

	close(ctx->fd);
	ctx->fd = -1;
	return false;
}

/* find an entry in the ipmgr's unknown-clients list*/
struct entry *find_entry(ipmgr_ctx *ctx, const struct in6_addr *k) {
	// TODO: make use of VECTOR_BSEARCH here.
	for (int i = 0; i < VECTOR_LEN(ctx->addrs); i++) {
		struct entry *e = &VECTOR_INDEX(ctx->addrs, i);
		if (memcmp(k, &(e->address), sizeof(struct in6_addr)) == 0) {
			if (l3ctx.debug) {
				print_ip(k, " is on the unknown-clients list\n");
			}
			return e;
		}
	}

	if (l3ctx.debug)
		print_ip(k, " is not on the unknown-clients list\n");

	return NULL;
}

/** This will remove an entry from the ipmgr unknown-clients list */
void delete_entry(const struct in6_addr *k) {
	for (int i = 0; i < VECTOR_LEN((&l3ctx.ipmgr_ctx)->addrs); i++) {
		struct entry *e = &VECTOR_INDEX((&l3ctx.ipmgr_ctx)->addrs, i);

		if (memcmp(k, &(e->address), sizeof(struct in6_addr)) == 0) {
			VECTOR_DELETE((&l3ctx.ipmgr_ctx)->addrs, i);
			break;
		}
	}
}


/** This will seek an address by checking locally and if needed querying the network by scheduling a task */
void ipmgr_seek_address(ipmgr_ctx *ctx, struct in6_addr *addr) {
	struct ip_task *ns_data = calloc(1, sizeof(struct ip_task));

	ns_data->ctx = ctx;
	memcpy(&ns_data->address, addr, sizeof(struct in6_addr));
	post_task(CTX(taskqueue), 0, 0, ipmgr_ns_task, free, ns_data);

	// schedule an intercom-seek operation that in turn will only be executed if there is no local client known
	struct ip_task *data = calloc(1, sizeof(struct ip_task));

	data->ctx = ctx;
	memcpy(&data->address, addr, sizeof(struct in6_addr));
	data->check_task = post_task(CTX(taskqueue), 0, 300, seek_task, free, data);
}

void handle_packet(ipmgr_ctx *ctx, uint8_t packet[], ssize_t packet_len) {
	struct in6_addr dst;
	struct in6_addr src;

	memcpy(&src, packet + 8, 16);
	memcpy(&dst, packet + 24, 16);

	uint8_t a0 = dst.s6_addr[0];

	// Ignore multicast
	if (a0 == 0xff)
		return;


	printf("Got packet from ");
	print_ip(&src, " destined to ");
	print_ip(&dst, "\n");


	if (!clientmgr_valid_address(CTX(clientmgr), &dst)) {
		char str[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &dst, str, sizeof str);
		fprintf(stderr, "The destination of the packet (%s) is not within the client prefixes. Ignoring packet\n", str);
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	struct entry *e = find_entry(ctx, &dst);

	bool new_unknown_dst = !e;

	if (new_unknown_dst) {
		struct entry entry = {
			.address = dst,
			.timestamp = now,
		};

		VECTOR_ADD(ctx->addrs, entry);
		e = &VECTOR_INDEX(ctx->addrs, VECTOR_LEN(ctx->addrs) - 1);

	}

	struct packet *p = malloc(sizeof(struct packet));

	p->timestamp = now;
	p->len = packet_len;
	p->data = malloc(packet_len);

	memcpy(p->data, packet, packet_len);

	VECTOR_ADD(e->packets, p);

	if (new_unknown_dst)
		ipmgr_seek_address(ctx, &dst);

//	schedule_ipcheck(ctx, e);
}

bool should_we_really_seek(struct in6_addr *destination) {
	struct entry *e = find_entry(&l3ctx.ipmgr_ctx, destination);
	// if a route to this client appeared, the queue will be emptied -- no seek necessary
	if (!e) {
		if (l3ctx.debug) {
			printf("INFO: seek task was scheduled but packets to be delivered to host: ");
			print_ip(destination, "\n");
		}
		return false;
	}

	if (clientmgr_is_known_address(&l3ctx.clientmgr_ctx, destination, NULL)) {
		printf("=================================================\n");
		printf("================= FAT WARNING ===================\n");
		printf("=================================================\n");
		printf("seek task was scheduled but there are no packets to be delivered to the host: ");
		print_ip(destination, ". This should not happen because it means the host is known but the packet queue is not empty.\n");
		return false;
	}

	// TODO: we could check if a route to this destination is known. If the route_appeared logic is ok, this however should not be necessary. Make the decision if we want the check or not and remove this message.

	return true;
}

void purge_old_packets(struct in6_addr *destination) {
	//TODO: check implementation. it seems to work but READ THIS 
	struct entry *e = find_entry(&l3ctx.ipmgr_ctx, destination);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	struct timespec then = now;
	then.tv_sec -= PACKET_TIMEOUT;

	for (int i = 0; i < VECTOR_LEN(e->packets); i++) {
		struct packet *p = VECTOR_INDEX(e->packets, i);

		if (timespec_cmp(p->timestamp, then) <= 0) {
			if (l3ctx.debug) {
				printf("deleting old packet with destination ");
				print_ip(&e->address, "\n");
			}

			free(p->data);
			free(p);
			VECTOR_DELETE(e->packets, i);
			i--;
		}
	}

	then = now;
	then.tv_sec -= SEEK_INTERVAL;

	if (VECTOR_LEN(e->packets) == 0 && timespec_cmp(e->timestamp, then) <= 0) {
		VECTOR_FREE(e->packets);
		delete_entry(&e->address);
	}
}


void ipmgr_ns_task(void *d) {
	struct ip_task *data = d;
	char str[INET6_ADDRSTRLEN];

	purge_old_packets(&data->address);

	if (should_we_really_seek(&data->address)) {
		inet_ntop(AF_INET6, &data->address, str, sizeof str);
		printf("\x1b[36mLooking for %s locally\x1b[0m\n", str);

		if (clientmgr_is_ipv4(&l3ctx.clientmgr_ctx, &data->address))
			arp_send_request(&l3ctx.arp_ctx, &data->address);
		else
			icmp6_send_solicitation(&l3ctx.icmp6_ctx, &data->address);

		struct ip_task *ns_data = calloc(1, sizeof(struct ip_task));

		ns_data->ctx = data->ctx;
		memcpy(&ns_data->address, &data->address, sizeof(struct in6_addr));
		post_task(&l3ctx.taskqueue_ctx, SEEK_INTERVAL, 0, ipmgr_ns_task, free, ns_data);
	}
}

void seek_task(void *d) {
	struct ip_task *data = d;

	if (should_we_really_seek(&data->address)) {
		if (l3ctx.debug) {
			printf("\x1b[36mseeking on intercom for client with the address ");
			print_ip(&data->address, "\x1b[0m\n");
		}
		intercom_seek(&l3ctx.intercom_ctx, (const struct in6_addr*) &(data->address));

/* // WHY do we need to set the client ip to inactive? we are only seeking if
** there is no route to this ip known.
		struct client *client = NULL;
		if (clientmgr_is_known_address(&l3ctx.clientmgr_ctx, &data->address, &client)) {
			struct client_ip *ip = get_client_ip(client, &data->address);
			if (ip)
				client_ip_set_state(&l3ctx.clientmgr_ctx, client, ip, IP_INACTIVE);
		}
		*/
		struct ip_task *_data = calloc(1, sizeof(struct ip_task));

		_data->ctx = data->ctx;
		memcpy(&_data->address, &data->address, sizeof(struct in6_addr));
		post_task(&l3ctx.taskqueue_ctx, SEEK_INTERVAL, 0, seek_task, free, _data);
	}
}

void ipmgr_handle_in(ipmgr_ctx *ctx, int fd) {
	ssize_t count;
	uint8_t buf[1500];

	while (1) {
		count = read(fd, buf, sizeof buf);

		if (count == -1) {
			/* If errno == EAGAIN, that means we have read all
			   data. So go back to the main loop. */
			if (errno != EAGAIN) {
				perror("read");
			}
			break;
		} else if (count == 0) {
			/* End of file. The remote has closed the
			   connection. */
			break;
		}

		// we are only interested in IPv6 containing a full header.
		if (count < 40)
			continue;

		// We're only interested in ip6 packets
		if ((buf[0] & 0xf0) != 0x60)
			continue;

		handle_packet(ctx, buf, count);
	}
}

void ipmgr_handle_out(ipmgr_ctx *ctx, int fd) {
	ssize_t count;

	while (1) {
		if (VECTOR_LEN(ctx->output_queue) == 0)
			break;

		struct packet *packet = &VECTOR_INDEX(ctx->output_queue, 0);
		count = write(fd, packet->data, packet->len);

		// TODO refactor to use epoll.

		if (count == -1) {
			if (errno != EAGAIN)
				perror("Could not send packet to newly visible client, requeueing and trying again.");
			else {
				// we received eagain, so let's requeue this packet 
				VECTOR_ADD(ctx->output_queue, *packet);
			}

			break;
		}
		else {
			// write was successful, free data structures
			free(packet->data);
		}
		VECTOR_DELETE(ctx->output_queue, 0);
	}
}

void ipmgr_route_appeared(ipmgr_ctx *ctx, const struct in6_addr *destination) {
	struct entry *e = find_entry(ctx, destination);

	if (!e)
		return;

	for (int i = 0; i < VECTOR_LEN(e->packets); i++) {
		struct packet *p = VECTOR_INDEX(e->packets, i);
		VECTOR_ADD(ctx->output_queue, *p);
	}

	VECTOR_FREE(e->packets);

	delete_entry(destination);

	ipmgr_handle_out(ctx, ctx->fd);
}


bool ipmgr_init(ipmgr_ctx *ctx, char *tun_name, unsigned int mtu) {
	return tun_open(ctx, tun_name, mtu, "/dev/net/tun");
}
