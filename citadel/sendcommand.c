/*
 * $Id$
 *
 * Command-line utility to transmit a server command.
 *
 */


#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#include <signal.h>
#include <errno.h>
#include <limits.h>
#include "citadel.h"
#include "tools.h"
#include "citadel_ipc.h"
#include "server.h"
#include "serv_extensions.h"
#include "config.h"

#define LOCKFILE "/tmp/LCK.sendcommand"

static CtdlIPC *ipc = NULL;

/*
 * make sure only one copy of sendcommand runs at a time, using lock files
 */
int set_lockfile(void)
{
	FILE *lfp;
	int onppid;

	if ((lfp = fopen(LOCKFILE, "r")) != NULL) {
		fscanf(lfp, "%d", &onppid);
		fclose(lfp);
		if (!kill(onppid, 0) || errno == EPERM)
			return 1;
	}
	lfp = fopen(LOCKFILE, "w");
	fprintf(lfp, "%ld\n", (long) getpid());
	fclose(lfp);
	return (0);
}

void remove_lockfile(void)
{
	unlink(LOCKFILE);
}

/*
 * Why both cleanup() and nq_cleanup() ?  Notice the alarm() call in
 * cleanup() .  If for some reason sendcommand hangs waiting for the server
 * to clean up, the alarm clock goes off and the program exits anyway.
 * The cleanup() routine makes a check to ensure it's not reentering, in
 * case the ipc module looped it somehow.
 */
void nq_cleanup(int e)
{
	remove_lockfile();
	exit(e);
}

void cleanup(int e)
{
	static int nested = 0;

	alarm(30);
	signal(SIGALRM, nq_cleanup);
	if (nested++ < 1)
		CtdlIPCQuit(ipc);
	nq_cleanup(e);
}

/*
 * This is implemented as a function rather than as a macro because the
 * client-side IPC modules expect logoff() to be defined.  They call logoff()
 * when a problem connecting or staying connected to the server occurs.
 */
void logoff(int e)
{
	cleanup(e);
}

/*
 * Connect sendcommand to the Citadel server running on this computer.
 */
void np_attach_to_server(char *host, char *port)
{
	char buf[SIZ];
	char hostbuf[256], portbuf[256];
	char *args[] =
	{"sendcommand", NULL};
	int r;

	fprintf(stderr, "Attaching to server...\n");
	strcpy(hostbuf, host);
	strcpy(portbuf, port);
	ipc = CtdlIPC_new(1, args, hostbuf, portbuf);
	if (!ipc) {
		fprintf(stderr, "Can't connect: %s\n", strerror(errno));
		exit(3);
	}
	CtdlIPC_chat_recv(ipc, buf);
	fprintf(stderr, "%s\n", &buf[4]);
	snprintf(buf, sizeof buf, "IPGM %d", config.c_ipgm_secret);
	r = CtdlIPCInternalProgram(ipc, config.c_ipgm_secret, buf);
	fprintf(stderr, "%s\n", buf);
	if (r / 100 != 2) {
		cleanup(2);
	}
}


void sendcommand_die(void) {
	exit(0);
}


/*
 * main
 */
int main(int argc, char **argv)
{
	int a;
	char cmd[SIZ];
	char buf[SIZ];

	strcpy(ctdl_home_directory, DEFAULT_PORT);

	strcpy(cmd, "");
	/*
	 * Change directories if specified
	 */
	for (a = 1; a < argc; ++a) {
		if (!strncmp(argv[a], "-h", 2)) {
			strcpy(ctdl_home_directory, argv[a]);
			strcpy(ctdl_home_directory, &ctdl_home_directory[2]);
			home_specified = 1;
		} else {
			if (strlen(cmd) > 0)
				strcat(cmd, " ");
			strcat(cmd, argv[a]);
		}
	}

	get_config();

	signal(SIGINT, cleanup);
	signal(SIGQUIT, cleanup);
	signal(SIGHUP, cleanup);
	signal(SIGTERM, cleanup);

	fprintf(stderr, "sendcommand: started (pid=%d) "
			"running in %s\n",
			(int) getpid(),
			ctdl_home_directory);
	fflush(stderr);
	np_attach_to_server(UDS, ctdl_home_directory);
	fflush(stderr);
	setIPCDeathHook(sendcommand_die);

	fprintf(stderr, "%s\n", cmd);
	CtdlIPC_chat_send(ipc, cmd);
	CtdlIPC_chat_recv(ipc, buf);
	fprintf(stderr, "%s\n", buf);

	if (buf[0] == '1') {
		while (CtdlIPC_chat_recv(ipc, buf), strcmp(buf, "000")) {
			printf("%s\n", buf);
		}
	} else if (buf[0] == '4') {
		do {
			if (fgets(buf, sizeof buf, stdin) == NULL)
				strcpy(buf, "000");
			if (strlen(buf) > 0)
				if (buf[strlen(buf) - 1] == '\n')
					buf[strlen(buf) - 1] = 0;
			if (strlen(buf) > 0)
				if (buf[strlen(buf) - 1] == '\r')
					buf[strlen(buf) - 1] = 0;
			if (strcmp(buf, "000"))
				CtdlIPC_chat_send(ipc, buf);
		} while (strcmp(buf, "000"));
		CtdlIPC_chat_send(ipc, "000");
	}
	fprintf(stderr, "sendcommand: processing ended.\n");
	cleanup(0);
	return 0;
}
