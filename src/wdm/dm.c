/*

Copyright 1988, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall
not be used in advertising or otherwise to promote the sale, use or
other dealings in this Software without prior written authorization
from The Open Group.

*/
/*
 * wdm - WINGs Display Manager
 * Author:  Keith Packard, MIT X Consortium
 *
 * display manager
 */

#include	<dm.h>
#include <wdm.h>
#include	<dm_auth.h>

#include	<stdio.h>
#include <signal.h>
#ifdef __NetBSD__
#include <sys/param.h>
#endif

#ifndef sigmask
#define sigmask(m)  (1 << ((m - 1)))
#endif

#include	<sys/stat.h>
#include	<errno.h>
#include	<X11/Xfuncproto.h>
#include	<stdarg.h>

#ifndef F_TLOCK
#include	<unistd.h>
#endif

#include <wdmlib.h>

static void StopAll(int n), RescanNotify(int n);
static void RescanServers(void);
static void RestartDisplay(struct display *d, int forceReserver);
static void ScanServers(void);
static void SetAccessFileTime(void);
static void SetConfigFileTime(void);
static void StartDisplays(void);
static void TerminateProcess(int pid, int signal);

int Rescan;
static long ServersModTime, ConfigModTime, AccessFileModTime;

#ifndef NOXDMTITLE
static char *Title;
static int TitleLen;
#endif

static void ChildNotify(int n);

static int StorePid(void);

static int parent_pid = -1;		/* PID of parent wdm process */

extern int wdmSequentialXServerLaunch;

int main(int argc, char **argv)
{
	int oldpid, oldumask;
	char cmdbuf[1024];
	int debugMode = 0;

	/* make sure at least world write access is disabled */
	if (((oldumask = umask(022)) & 002) == 002)
		(void)umask(oldumask);
#ifndef NOXDMTITLE
	Title = argv[0];
	TitleLen = (argv[argc - 1] + strlen(argv[argc - 1])) - Title;
#endif

	/*
	 * Step 1 - load configuration parameters
	 */
	InitResources(argc, argv);
	SetConfigFileTime();
	LoadDMResources();
	/*
	 * Only allow root to run in non-debug mode to avoid problems
	 */
	debugMode = (debugLevel.i > WDM_LEVEL_WARNING);
	if (!debugMode && getuid() != 0) {
		fprintf(stderr, "Only root wants to run %s\n", argv[0]);
		exit(1);
	}
	if (!debugMode && daemonMode.i) {
		BecomeOrphan();
		BecomeDaemon();
	}
	/* SUPPRESS 560 */
	if ((oldpid = StorePid())) {
		if (oldpid == -1)
			WDMError("Can't create/lock pid file %s\n", pidFile);
		else
			WDMError("Can't lock pid file %s, another wdm is running " "(pid %d)\n", pidFile, oldpid);
		exit(1);
	}
	WDMLogLevel(debugLevel.i);
	if (useSyslog.i) {
		WDMUseSysLog("wdm", WDMStringToFacility(syslogFacility));
	} else if (errorLogFile && *errorLogFile) {
		int f;
		if ((f = open(errorLogFile, O_CREAT | O_WRONLY | O_APPEND, 0600)) == -1)
			WDMError("cannot open errorLogFile %s\n", errorLogFile);
		else
			WDMLogStream(fdopen(f, "w"));
	}
	/* redirect any messages for stderr into standard logging functions. */
	WDMRedirectStderr(WDM_LEVEL_ERROR);

	/* Clean up any old Authorization files */
	sprintf(cmdbuf, "/bin/rm -f %s/authdir/authfiles/A*", authDir);
	system(cmdbuf);

#ifdef XDMCP
	init_session_id();
	CreateWellKnownSockets();
#else
	WDMDebug("wdm: not compiled for XDMCP\n");
#endif
	parent_pid = getpid();
	(void)Signal(SIGTERM, StopAll);
	(void)Signal(SIGINT, StopAll);
	/*
	 * Step 2 - Read /etc/Xservers and set up
	 *      the socket.
	 *
	 *      Keep a sub-daemon running
	 *      for each entry
	 */
	SetAccessFileTime();
#ifdef XDMCP
	ScanAccessDatabase();
#endif
	ScanServers();
	StartDisplays();
	(void)Signal(SIGHUP, RescanNotify);
	(void)Signal(SIGCHLD, ChildNotify);
	while (
#ifdef XDMCP
			  AnyWellKnownSockets() ||
#endif
			  AnyDisplaysLeft()) {
		if (Rescan) {
			RescanServers();
			Rescan = 0;
		}
		WaitForSomething();
	}
	WDMDebug("Nothing left to do, exiting\n");
	exit(0);
 /*NOTREACHED*/}

static void RescanNotify(int n)
{
	int olderrno = errno;

	WDMDebug("Caught SIGHUP\n");
	Rescan = 1;
	errno = olderrno;
}

static void ScanServers(void)
{
	char lineBuf[10240];
	int len;
	FILE *serversFile;
	struct stat statb;
	static DisplayType acceptableTypes[] = { {Local, Permanent, FromFile},
	{Foreign, Permanent, FromFile},
	};

#define NumTypes    (sizeof (acceptableTypes) / sizeof (acceptableTypes[0]))

	if (servers[0] == '/') {
		serversFile = fopen(servers, "r");
		if (serversFile == NULL) {
			WDMError("cannot access servers file %s\n", servers);
			return;
		}
		if (ServersModTime == 0) {
			fstat(fileno(serversFile), &statb);
			ServersModTime = statb.st_mtime;
		}
		while (fgets(lineBuf, sizeof(lineBuf) - 1, serversFile)) {
			len = strlen(lineBuf);
			if (lineBuf[len - 1] == '\n')
				lineBuf[len - 1] = '\0';
			ParseDisplay(lineBuf, acceptableTypes, NumTypes);
		}
		fclose(serversFile);
	} else {
		ParseDisplay(servers, acceptableTypes, NumTypes);
	}
}

static void MarkDisplay(struct display *d)
{
	d->state = MissingEntry;
}

static void RescanServers(void)
{
	WDMDebug("rescanning servers\n");
	WDMInfo("Rescanning both config and servers files\n");
	ForEachDisplay(MarkDisplay);
	SetConfigFileTime();
	ReinitResources();
	LoadDMResources();
	ScanServers();
	SetAccessFileTime();
#ifdef XDMCP
	ScanAccessDatabase();
#endif
	StartDisplays();
}

static void SetConfigFileTime(void)
{
	struct stat statb;

	if (stat(config, &statb) != -1)
		ConfigModTime = statb.st_mtime;
}

static void SetAccessFileTime(void)
{
	struct stat statb;

	if (stat(accessFile, &statb) != -1)
		AccessFileModTime = statb.st_mtime;
}

static void RescanIfMod(void)
{
	struct stat statb;

	if (config && stat(config, &statb) != -1) {
		if (statb.st_mtime != ConfigModTime) {
			WDMDebug("Config file %s has changed, rereading\n", config);
			WDMInfo("Rereading configuration file %s\n", config);
			ConfigModTime = statb.st_mtime;
			ReinitResources();
			LoadDMResources();
		}
	}
	if (servers[0] == '/' && stat(servers, &statb) != -1) {
		if (statb.st_mtime != ServersModTime) {
			WDMDebug("Servers file %s has changed, rescanning\n", servers);
			WDMInfo("Rereading servers file %s\n", servers);
			ServersModTime = statb.st_mtime;
			ForEachDisplay(MarkDisplay);
			ScanServers();
		}
	}
#ifdef XDMCP
	if (accessFile && accessFile[0] && stat(accessFile, &statb) != -1) {
		if (statb.st_mtime != AccessFileModTime) {
			WDMDebug("Access file %s has changed, rereading\n", accessFile);
			WDMInfo("Rereading access file %s\n", accessFile);
			AccessFileModTime = statb.st_mtime;
			ScanAccessDatabase();
		}
	}
#endif
}

/*
 * catch a SIGTERM, kill all displays and exit
 */

static void StopAll(int n)
{
	int olderrno = errno;

	if (parent_pid != getpid()) {
		/* 
		 * We are a child wdm process that was killed by the
		 * master wdm before we were able to return from fork()
		 * and remove this signal handler.
		 *
		 * See defect XWSog08655 for more information.
		 */
		WDMDebug("Child wdm caught SIGTERM before it remove that signal.\n");
		(void)Signal(n, SIG_DFL);
		TerminateProcess(getpid(), SIGTERM);
		errno = olderrno;
		return;
	}
	WDMDebug("Shutting down entire manager\n");
#ifdef XDMCP
	DestroyWellKnownSockets();
#endif
	ForEachDisplay(StopDisplay);
	errno = olderrno;
}

/*
 * notice that a child has died and may need another
 * sub-daemon started
 */

int ChildReady;

static void ChildNotify(int n)
{
	int olderrno = errno;

	ChildReady = 1;
	errno = olderrno;
}

void WaitForChild(void)
{
	int pid;
	struct display *d;
	waitType status;
	sigset_t mask, omask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, &omask);
	WDMDebug("signals blocked\n");
	if (!ChildReady && !Rescan)
		sigsuspend(&omask);
	ChildReady = 0;
	sigprocmask(SIG_SETMASK, &omask, (sigset_t *) NULL);
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
		WDMDebug("Manager wait returns pid: %d sig %d core %d code %d\n", pid, waitSig(status), waitCore(status), waitCode(status));
		if (autoRescan.i)
			RescanIfMod();
		/* SUPPRESS 560 */
		if ((d = FindDisplayByPid(pid))) {
			d->pid = -1;
			switch (waitVal(status)) {
			case UNMANAGE_DISPLAY:
				WDMDebug("Display exited with UNMANAGE_DISPLAY\n");
				StopDisplay(d);
				break;
			case OBEYSESS_DISPLAY:
				d->startTries = 0;
				WDMDebug("Display exited with OBEYSESS_DISPLAY\n");
				if (d->displayType.lifetime != Permanent || d->status == zombie)
					StopDisplay(d);
				else
					RestartDisplay(d, FALSE);
				break;
			default:
				WDMDebug("Display exited with unknown status %d\n", waitVal(status));
				WDMError("Unknown session exit code %d from process %d\n", waitVal(status), pid);
				StopDisplay(d);
				break;
			case OPENFAILED_DISPLAY:
				WDMDebug("Display exited with OPENFAILED_DISPLAY, try %d of %d\n", d->startTries, d->startAttempts);
				WDMError("Display %s cannot be opened\n", d->name);
				/*
				 * no display connection was ever made, tell the
				 * terminal that the open attempt failed
				 */
#ifdef XDMCP
				if (d->displayType.origin == FromXDMCP)
					SendFailed(d, "Cannot open display");
#endif
				if (d->displayType.origin == FromXDMCP || d->status == zombie || ++d->startTries >= d->startAttempts) {
					WDMError("Display %s is being disabled\n", d->name);
					StopDisplay(d);
				} else {
					RestartDisplay(d, TRUE);
				}
				break;
			case RESERVER_DISPLAY:
				d->startTries = 0;
				WDMDebug("Display exited with RESERVER_DISPLAY\n");
				if (d->displayType.origin == FromXDMCP || d->status == zombie)
					StopDisplay(d);
				else
					RestartDisplay(d, TRUE);
				{
					Time_t Time;
					time(&Time);
					WDMDebug("time %i %i\n", (int)Time, (int)d->lastCrash);
					if (d->lastCrash && ((Time - d->lastCrash) < XDM_BROKEN_INTERVAL)) {
						WDMDebug("Server crash frequency too high:" " removing display %s\n", d->name);
						WDMError("Server crash rate too high:" " removing display %s\n", d->name);
						RemoveDisplay(d);
					} else
						d->lastCrash = Time;
				}
				break;
			case waitCompose(SIGTERM, 0, 0):
				WDMDebug("Display exited on SIGTERM, try %d of %d\n", d->startTries, d->startAttempts);
				if (d->displayType.origin == FromXDMCP || d->status == zombie || ++d->startTries >= d->startAttempts) {
					WDMError("Display %s is being disabled\n", d->name);
					StopDisplay(d);
				} else
					RestartDisplay(d, TRUE);
				break;
			case REMANAGE_DISPLAY:
				d->startTries = 0;
				WDMDebug("Display exited with REMANAGE_DISPLAY\n");
				/*
				 * XDMCP will restart the session if the display
				 * requests it
				 */
				if (d->displayType.origin == FromXDMCP || d->status == zombie)
					StopDisplay(d);
				else
					RestartDisplay(d, FALSE);
				break;
			}
		}
		/* SUPPRESS 560 */
		else if ((d = FindDisplayByServerPid(pid))) {
			d->serverPid = -1;
			switch (d->status) {
			case zombie:
				WDMDebug("Zombie server reaped, removing display %s\n", d->name);
				RemoveDisplay(d);
				break;
			case phoenix:
				WDMDebug("Phoenix server arises, restarting display %s\n", d->name);
				d->status = notRunning;
				break;
			case running:
				WDMDebug("Server for display %s terminated unexpectedly, status %d %d\n", d->name, waitVal(status), status);
				WDMError("Server for display %s terminated unexpectedly: %d\n", d->name, waitVal(status));
				d->status = notRunning;
				if (d->pid != -1) {
					WDMDebug("Terminating session pid %d\n", d->pid);
					TerminateProcess(d->pid, SIGTERM);
				}
				break;
			case notRunning:
				WDMDebug("Server exited for notRunning session on display %s\n", d->name);
				break;
			}
		} else {
			WDMDebug("Unknown child termination, status %d\n", waitVal(status));
		}
	}
	StartDisplays();
}

static void CheckDisplayStatus(struct display *d)
{
	if (d->displayType.origin == FromFile) {
		switch (d->state) {
		case MissingEntry:
			StopDisplay(d);
			break;
		case NewEntry:
			d->state = OldEntry;
		case OldEntry:
			if (d->status == notRunning)
				StartDisplay(d);
			break;
		}
	}
}

static void StartDisplays(void)
{
	ForEachDisplay(CheckDisplayStatus);
}

void StartDisplay(struct display *d)
{
	int pid;

	WDMDebug("StartDisplay %s\n", d->name);
	LoadServerResources(d);
	if (d->displayType.location == Local) {
		/* don't bother pinging local displays; we'll
		 * certainly notice when they exit
		 */
		d->pingInterval = 0;
		if (d->authorize) {
			WDMDebug("SetLocalAuthorization %s, auth %s\n", d->name, d->authNames[0]);
			SetLocalAuthorization(d);
			/*
			 * reset the server after writing the authorization information
			 * to make it read the file (for compatibility with old
			 * servers which read auth file only on reset instead of
			 * at first connection)
			 */
			if (d->serverPid != -1 && d->resetForAuth && d->resetSignal)
				kill(d->serverPid, d->resetSignal);
		}
		if (d->serverPid == -1 && !StartServer(d)) {
			WDMError("Server for display %s can't be started, session disabled\n", d->name);
			RemoveDisplay(d);
			return;
		}
	} else {
		/* this will only happen when using XDMCP */
		if (d->authorizations)
			SaveServerAuthorizations(d, d->authorizations, d->authNum);
	}
	pid = fork();
	switch (pid) {
	case 0:
		CleanUpChild();
		LoadSessionResources(d);
		SetAuthorization(d);
		(void)Signal(SIGPIPE, SIG_IGN);
		(void)Signal(SIGHUP, SIG_IGN);
		if (!WaitForServer(d))
			exit(OPENFAILED_DISPLAY);
#ifdef XDMCP
		if (d->useChooser)
			RunChooser(d);
		else
#endif
			ManageSession(d);
		exit(REMANAGE_DISPLAY);
	case -1:
		break;
	default:
		WDMDebug("pid: %d\n", pid);
		d->pid = pid;
		d->status = running;
		/* checking a predeclared X resource DisplayManager*wdmSequentialXServerLaunch here */
		if (wdmSequentialXServerLaunch)
			WaitForServer(d);
		break;
	}
}

static void TerminateProcess(int pid, int signal)
{
	kill(pid, signal);
#ifdef SIGCONT
	kill(pid, SIGCONT);
#endif
}

/*
 * transition from running to zombie or deleted
 */

void StopDisplay(struct display *d)
{
	if (d->serverPid != -1)
		d->status = zombie;		/* be careful about race conditions */
	if (d->pid != -1)
		TerminateProcess(d->pid, SIGTERM);
	if (d->serverPid != -1)
		TerminateProcess(d->serverPid, d->termSignal);
	else
		RemoveDisplay(d);
}

/*
 * transition from running to phoenix or notRunning
 */

static void RestartDisplay(struct display *d, int forceReserver)
{
	if (d->serverPid != -1 && (forceReserver || d->terminateServer)) {
		TerminateProcess(d->serverPid, d->termSignal);
		d->status = phoenix;
	} else {
		d->status = notRunning;
	}
}

static FD_TYPE CloseMask;
static int max;

void RegisterCloseOnFork(int fd)
{
	FD_SET(fd, &CloseMask);
	if (fd > max)
		max = fd;
}

void ClearCloseOnFork(int fd)
{
	FD_CLR(fd, &CloseMask);
	if (fd == max) {
		while (--fd >= 0)
			if (FD_ISSET(fd, &CloseMask))
				break;
		max = fd;
	}
}

void CloseOnFork(void)
{
	int fd;

	for (fd = 0; fd <= max; fd++)
		if (FD_ISSET(fd, &CloseMask)) {
			close(fd);
		}
	FD_ZERO(&CloseMask);
	max = 0;
}

static int pidFd;
static FILE *pidFilePtr;

static int StorePid(void)
{
	int oldpid;

	if (pidFile[0] != '\0') {
		pidFd = open(pidFile, O_RDWR);
		if (pidFd == -1 && errno == ENOENT)
			pidFd = open(pidFile, O_RDWR | O_CREAT, 0666);
		if (pidFd == -1 || !(pidFilePtr = fdopen(pidFd, "r+"))) {
			WDMError("process-id file %s cannot be opened\n", pidFile);
			return -1;
		}
		if (fscanf(pidFilePtr, "%d\n", &oldpid) != 1)
			oldpid = -1;
		fseek(pidFilePtr, 0l, 0);
		if (lockPidFile.i) {
#ifdef F_SETLK
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
			struct flock lock_data;
			lock_data.l_type = F_WRLCK;
			lock_data.l_whence = SEEK_SET;
			lock_data.l_start = lock_data.l_len = 0;
			if (fcntl(pidFd, F_SETLK, &lock_data) == -1) {
				if (errno == EAGAIN)
					return oldpid;
				else
					return -1;
			}
#else
#ifdef LOCK_EX
			if (flock(pidFd, LOCK_EX | LOCK_NB) == -1) {
				if (errno == EWOULDBLOCK)
					return oldpid;
				else
					return -1;
			}
#else
			if (lockf(pidFd, F_TLOCK, 0) == -1) {
				if (errno == EACCES)
					return oldpid;
				else
					return -1;
			}
#endif
#endif
		}
		fprintf(pidFilePtr, "%5ld\n", (long)getpid());
		(void)fflush(pidFilePtr);
		RegisterCloseOnFork(pidFd);
	}
	return 0;
}

#if 0
void UnlockPidFile(void)
{
	if (lockPidFile)
#ifdef F_SETLK
	{
		struct flock lock_data;
		lock_data.l_type = F_UNLCK;
		lock_data.l_whence = SEEK_SET;
		lock_data.l_start = lock_data.l_len = 0;
		(void)fcntl(pidFd, F_SETLK, &lock_data);
	}
#else
#ifdef F_ULOCK
		lockf(pidFd, F_ULOCK, 0);
#else
		flock(pidFd, LOCK_UN);
#endif
#endif
	close(pidFd);
	fclose(pidFilePtr);
}
#endif

#ifndef HAS_SETPROCTITLE
void SetTitle(char *name, ...)
{
#ifndef NOXDMTITLE
	char *p = Title;
	int left = TitleLen;
	char *s;
	va_list args;

	va_start(args, name);
	*p++ = '-';
	--left;
	s = name;
	while (s) {
		while (*s && left > 0) {
			*p++ = *s++;
			left--;
		}
		s = va_arg(args, char *);
	}
	while (left > 0) {
		*p++ = ' ';
		--left;
	}
	va_end(args);
#endif
}
#endif
