/*
 * $Id$
 *
 * This program attempts to act like a local MDA if you're using sendmail or
 * some other non-Citadel MTA.  It basically just contacts the Citadel LMTP
 * listener on a unix domain socket and transmits the message.
 *
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <string.h>
#include <pwd.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include "citadel.h"
#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif
#include "config.h"

/* #define DEBUG  */	/* uncomment to get protocol traces */

int serv_sock;


void strip_trailing_nonprint(char *buf)
{
        while ( (strlen(buf)>0) && (!isprint(buf[strlen(buf) - 1])) )
                buf[strlen(buf) - 1] = 0;
}


void timeout(int signum)
{
	exit(signum);
}


int uds_connectsock(char *sockpath)
{
	int s;
	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sockpath, sizeof addr.sun_path);

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0) {
		fprintf(stderr, "Can't create socket: %s\n",
			strerror(errno));
		exit(3);
	}

	if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		fprintf(stderr, "can't connect: %s\n",
			strerror(errno));
		close(s);
		exit(3);
	}

	return s;
}


/*
 * input binary data from socket
 */
void serv_read(char *buf, int bytes)
{
	int len, rlen;

	len = 0;
	while (len < bytes) {
		rlen = read(serv_sock, &buf[len], bytes - len);
		if (rlen < 1) {
			return;
		}
		len = len + rlen;
	}
}


/*
 * send binary to server
 */
void serv_write(char *buf, int nbytes)
{
	int bytes_written = 0;
	int retval;
	while (bytes_written < nbytes) {
		retval = write(serv_sock, &buf[bytes_written],
			       nbytes - bytes_written);
		if (retval < 1) {
			return;
		}
		bytes_written = bytes_written + retval;
	}
}



/*
 * input string from socket - implemented in terms of serv_read()
 */
void serv_gets(char *buf)
{
	int i;

	/* Read one character at a time.
	 */
	for (i = 0;; i++) {
		serv_read(&buf[i], 1);
		if (buf[i] == '\n' || i == (SIZ-1))
			break;
	}

	/* If we got a long line, discard characters until the newline.
	 */
	if (i == (SIZ-1))
		while (buf[i] != '\n')
			serv_read(&buf[i], 1);

	/* Strip all trailing nonprintables (crlf)
	 */
	buf[i] = 0;
	strip_trailing_nonprint(buf);
#ifdef DEBUG
	printf("> %s\n", buf);
#endif
}


/*
 * send line to server - implemented in terms of serv_write()
 */
void serv_puts(char *buf)
{
#ifdef DEBUG
	printf("< %s\n", buf);
#endif
	serv_write(buf, strlen(buf));
	serv_write("\n", 1);
}





void cleanup(int exitcode) {
	char buf[1024];

	serv_puts("QUIT");
	serv_gets(buf);
	exit(exitcode);
}



int main(int argc, char **argv) {
	char buf[1024];
	char fromline[1024];
	FILE *fp;

	get_config();

	fp = tmpfile();
	if (fp == NULL) return(errno);
	snprintf(fromline, sizeof fromline, "From: someone@somewhere.org");
	while (fgets(buf, 1024, stdin) != NULL) {
		fprintf(fp, "%s", buf);
		if (!strncasecmp(buf, "From:", 5)) strcpy(fromline, buf);
	}
	strip_trailing_nonprint(fromline);

	serv_sock = uds_connectsock("lmtp.socket");
	serv_gets(buf);
	if (buf[0]!='2') cleanup(1);

	serv_puts("LHLO x");
	do {
		serv_gets(buf);
		strcat(buf, "    ");
	} while (buf[3] == '-');
	if (buf[0] != '2') cleanup(1);

	snprintf(buf, sizeof buf, "MAIL %s", fromline);
	serv_puts(buf);
	serv_gets(buf);
	if (buf[0]!='2') cleanup(1);

	snprintf(buf, sizeof buf, "RCPT To: %s", argv[1]);
	serv_puts(buf);
	serv_gets(buf);
	if (buf[0]!='2') cleanup(1);

	serv_puts("DATA");
	serv_gets(buf);
	if (buf[0]!='3') cleanup(1);

	rewind(fp);
	while (fgets(buf, sizeof buf, fp) != NULL) {
		strip_trailing_nonprint(buf);
		serv_puts(buf);
	}
	serv_puts(".");
	serv_gets(buf);
	if (buf[0]!='2') cleanup(1);
	else cleanup(0);
	return(0);
}
