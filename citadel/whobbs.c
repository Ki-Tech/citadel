/*
 * $Id$
 * 
 * Command-line "who is online?" utility
 *
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "citadel.h"
#include "citadel_ipc.h"
#include "tools.h"

void logoff(int code)
{
	exit(code);
	}

static void escapize(char *buf, size_t n) {
	char hold[512];
	int i;

	strcpy(hold, buf);
	strcpy(buf, "");

	for (i=0; i<strlen(hold); ++i) {
		size_t tmp = strlen(buf);

		if (hold[i]=='<')
			snprintf(&buf[tmp], n - tmp, "&lt;");
		else if (hold[i]=='>')
			snprintf(&buf[tmp], n - tmp, "&gt;");
		else if (hold[i]==34)
			snprintf(&buf[tmp], n - tmp, "&quot;");
		else
			snprintf(&buf[tmp], n - tmp, "%c", hold[i]);
	}
}




int main(int argc, char **argv)
{
	char buf[512];
	char nodetitle[SIZ];
	int www = 0;
	int s_pid = 0;
	int my_pid = 0;
	char hostbuf[SIZ];
	char portbuf[SIZ];
	char s_user[SIZ];
	char s_room[SIZ];
	char s_host[SIZ];
	char s_client[SIZ];
	int r;			/* IPC response code */
	time_t timenow;
	char *listing = NULL;
	CtdlIPC *ipc = NULL;

	/* If this environment variable is set, we assume that the program
	 * is being called as a cgi-bin from a webserver and will output
	 * everything as HTML.
	 */	
	if (getenv("REQUEST_METHOD") != NULL) www = 1;

	ipc = CtdlIPC_new(argc, argv, hostbuf, portbuf);
	if (!ipc) {
		fprintf(stderr, "Server not available: %s\n", strerror(errno));
		logoff(errno);
	}
	CtdlIPC_chat_recv(ipc, buf);
	if ((buf[0]!='2')&&(strncmp(buf,"551",3))) {
		fprintf(stderr,"%s: %s\n",argv[0],&buf[4]);
		logoff(atoi(buf));
		}
	strcpy(nodetitle, "this BBS");
	r = CtdlIPCServerInfo(ipc, buf);
	if (r / 100 == 1) {
		my_pid = ipc->ServInfo.pid;
		strcpy(nodetitle, ipc->ServInfo.humannode);
	}
	
	if (www) {
		printf(	"Content-type: text/html\n"
			"\n"
			"<HTML><HEAD>"
			"<META HTTP-EQUIV=\"Refresh\" CONTENT=\"60\">\n"
			"<TITLE>");
		printf("%s: who is online", nodetitle);
		printf(	"</TITLE></HEAD><BODY><H1>");
	} else {
		printf("            ");
	}

	if (www) {
		printf("<CENTER><H1>");
	}

	printf("Users currently logged on to %s\n", nodetitle);

	if (www) {
		printf("</H1>\n");
	}

	r = CtdlIPCOnlineUsers(ipc, &listing, &timenow, buf);
	if (r / 100 != 1) {
		fprintf(stderr,"%s: %s\n",argv[0], buf);
		logoff(atoi(buf));
	}

	if (www) {
		printf(	"<TABLE BORDER=1 WIDTH=100%%>"
			"<TR><TH>Session</TH><TH>User name</TH>"
			"<TH>Room</TH><TH>From host</TH>"
			"<TH>Client software</TH></TR>\n");
	} else {

		printf(	"Session         User name               "
			"Room                  From host\n");
		printf(	"------- ------------------------- "
			"------------------- ------------------------\n");
	}


	while (strlen(listing) > 0) {
		extract_token(buf, listing, 0, '\n');
		remove_token(listing, 0, '\n');

		/* Escape some stuff if we're using www mode */
		if (www) escapize(buf, sizeof buf);

		s_pid = extract_int(buf,0);
		extract(s_user,buf,1);
		extract(s_room,buf,2);
		extract(s_host,buf,3);
		extract(s_client,buf,4);
		if (s_pid != my_pid) {

			if (www) printf("<TR><TD>");
			printf("%-7d", s_pid);
			printf("%c", 
				((s_pid == my_pid) ? '*' : ' '));
			if (www) printf("</TD><TD>");
			printf("%-26s", s_user);
			if (www) printf("</TD><TD>");
			printf("%-19s ", s_room);
			if (www) printf("</TD><TD>");
			printf("%-24s\n", s_host);
			if (www) printf("</TD><TD>%s</TD></TR>\n", s_client);
			}
		}
	free(listing);

	if (www) printf("</TABLE></CENTER>\n"
			"<FONT SIZE=-1>"
			"(This display will automatically refresh "
			"once per minute)</FONT>\n"
			"</BODY></HTML>\n");

	r = CtdlIPCQuit(ipc);
	return (r / 100 == 2) ? 0 : r;
}


#ifndef HAVE_STRERROR
/*
 * replacement strerror() for systems that don't have it
 */
char *strerror(int e)
{
	static char buf[32];

	snprintf(buf, sizeof buf, "errno = %d",e);
	return(buf);
	}
#endif
