

#include "util.h"
#include <arpa/inet.h>
#include <stdio.h>
#include "l3roamd.h"
#include "error.h"

/* print a human-readable representation of an in6_addr struct to stdout
** */
const char *print_ip(const struct in6_addr *addr) {
	static char a1[INET6_ADDRSTRLEN+1];
	return inet_ntop(AF_INET6, &(addr->s6_addr), a1, INET6_ADDRSTRLEN);
}

void log_debug(const char *format, ...) {
	if (!l3ctx.debug)
		return;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

void log_verbose(const char *format, ...) {
	if (!l3ctx.verbose)
		return;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

