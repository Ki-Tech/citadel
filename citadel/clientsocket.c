/*
 * $Id$
 *
 * This module handles client-side sockets opened by the Citadel server (for
 * the client side of Internet protocols, etc.)   It does _not_ handle client
 * sockets for the Citadel client; for that you must look in ipc_c_tcp.c
 * (which, uncoincidentally, bears a striking similarity to this file).
 *
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <pwd.h>
#include <errno.h>
#include <stdarg.h>
#include "citadel.h"
#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif
#include "sysdep_decls.h"

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

int sock_connect(char *host, char *service, char *protocol)
{
	struct hostent *phe;
	struct servent *pse;
	struct protoent *ppe;
	struct sockaddr_in sin;
	int s, type;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;

	pse = getservbyname(service, protocol);
	if (pse) {
		sin.sin_port = pse->s_port;
	} else if ((sin.sin_port = htons((u_short) atoi(service))) == 0) {
		lprintf(3, "Can't get %s service entry: %s\n",
			service, strerror(errno));
		return(-1);
	}
	phe = gethostbyname(host);
	if (phe) {
		memcpy(&sin.sin_addr, phe->h_addr, phe->h_length);
	} else if ((sin.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE) {
		lprintf(3, "Can't get %s host entry: %s\n",
			host, strerror(errno));
		return(-1);
	}
	if ((ppe = getprotobyname(protocol)) == 0) {
		lprintf(3, "Can't get %s protocol entry: %s\n",
			protocol, strerror(errno));
		return(-1);
	}
	if (!strcmp(protocol, "udp")) {
		type = SOCK_DGRAM;
	} else {
		type = SOCK_STREAM;
	}

	s = socket(PF_INET, type, ppe->p_proto);
	if (s < 0) {
		lprintf(3, "Can't create socket: %s\n", strerror(errno));
		return(-1);
	}

	if (connect(s, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		lprintf(3, "can't connect to %s.%s: %s\n",
			host, service, strerror(errno));
		return(-1);
	}

	return (s);
}

/*
 * sock_read() - input binary data from socket.
 * Returns the number of bytes read, or -1 for error.
 */
int sock_read(int sock, char *buf, int bytes)
{
	int len, rlen;

	len = 0;
	while (len < bytes) {
		rlen = read(sock, &buf[len], bytes - len);
		if (rlen < 1) {
			return (-1);
		}
		len = len + rlen;
	}
	return (len);
}


/*
 * sock_write() - send binary to server.
 * Returns the number of bytes written, or -1 for error.
 */
int sock_write(int sock, char *buf, int nbytes)
{
	int bytes_written = 0;
	int retval;
	while (bytes_written < nbytes) {
		retval = write(sock, &buf[bytes_written],
			       nbytes - bytes_written);
		if (retval < 1) {
			return (-1);
		}
		bytes_written = bytes_written + retval;
	}
	return (bytes_written);
}



/*
 * Input string from socket - implemented in terms of sock_read()
 * 
 */
int sock_gets(int sock, char *buf)
{
	int i;

	/* Read one character at a time.
	 */
	for (i = 0;; i++) {
		if (sock_read(sock, &buf[i], 1) < 0) return(-1);
		if (buf[i] == '\n' || i == 255)
			break;
	}

	/* If we got a long line, discard characters until the newline.
	 */
	if (i == 255)
		while (buf[i] != '\n')
			if (sock_read(sock, &buf[i], 1) < 0) return(-1);

	/* Strip any trailing CR and LF characters.
	 */
	buf[i] = 0;
	while ( (strlen(buf)>0)
	      && ((buf[strlen(buf)-1]==13)
	      || (buf[strlen(buf)-1]==10)) ) {
		buf[strlen(buf)-1] = 0;
	}
	return(strlen(buf));
}

/*
 * Multiline version of sock_gets() ... this is a convenience function for
 * client side protocol implementations.  It only returns the first line of
 * a multiline response, discarding the rest.
 */
int ml_sock_gets(int sock, char *buf) {
	char bigbuf[1024];
	int g;

	g = sock_gets(sock, buf);
	if (g < 0) return(g);
	if ( (g < 4) || (buf[3] != '-')) return(g);

	do {
		g = sock_gets(sock, bigbuf);
		if (g < 0) return(g);
	} while ( (g >= 4) && (bigbuf[3] == '-') );

	return(strlen(buf));
}


/*
 * sock_puts() - send line to server - implemented in terms of serv_write()
 * Returns the number of bytes written, or -1 for error.
 */
int sock_puts(int sock, char *buf)
{
	int i, j;

	i = sock_write(sock, buf, strlen(buf));
	if (i<0) return(i);
	j = sock_write(sock, "\n", 1);
	if (j<0) return(j);
	return(i+j);
}
