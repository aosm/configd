/*
 * Copyright (c) 2001-2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * Modification History
 *
 * October 12, 2001		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */

#include <fcntl.h>
#include <paths.h>
#include <pthread.h>
#include <unistd.h>
#include <sysexits.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <mach/mach.h>
#include <mach/mach_error.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDPlugin.h>



typedef struct childInfo *childInfoRef;

struct childInfo {
	pid_t			pid;
	SCDPluginExecCallBack	callout;
	void			*context;
	int			status;
	struct rusage		rusage;
	childInfoRef		next;
};


/*
 * Mach port used to notify runloop when a child process
 * has been reaped.
 */
static CFMachPortRef	childReaped	= NULL;

/*
 * The following dictionaries contain information about child
 * processes, reaped processes, and any associated callback
 * information.
 *
 * Important: Access to these dictionaries should only be
 *            made when in a SIGCHLD handler (or when the
 *            childLock mutex is held *AND* the signal
 *            has been blocked).
 */
static childInfoRef	activeChildren	= NULL;
static pthread_mutex_t	lock		= PTHREAD_MUTEX_INITIALIZER;


static inline void
blockSignal()
{
	sigset_t	mask	= sigmask(SIGCHLD);

	// block SIGCHLD
	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		perror("sigprocmask(SIG_BLOCK)");
	}

	return;
}


static inline void
unblockSignal()
{
	sigset_t	mask	= sigmask(SIGCHLD);

	// unblock SIGCHLD
	if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1) {
		perror("sigprocmask(SIG_UNBLOCK)");
	}

	return;
}


static void
reaper(int sigraised)
{
	mach_msg_empty_send_t	msg;
	mach_msg_option_t	options;
	kern_return_t		status;

	/*
	 * block additional SIGCHLD's until current children have
	 * been reaped.
	 */
	blockSignal();

	/*
	 * send message to indicate that at least one child is ready
	 * to be reaped.
	 */
	msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.header.msgh_size = sizeof(msg);
	msg.header.msgh_remote_port = CFMachPortGetPort(childReaped);
	msg.header.msgh_local_port = MACH_PORT_NULL;
	msg.header.msgh_id = 0;
	options = MACH_SEND_TIMEOUT;
	status = mach_msg(&msg.header,			/* msg */
			  MACH_SEND_MSG|options,	/* options */
			  msg.header.msgh_size,		/* send_size */
			  0,				/* rcv_size */
			  MACH_PORT_NULL,		/* rcv_name */
			  0,				/* timeout */
			  MACH_PORT_NULL);		/* notify */

	return;
}


static void
childrenReaped(CFMachPortRef port, void *msg, CFIndex size, void *info)
{
	pid_t		pid		= 0;
	childInfoRef	reapedChildren	= NULL;

	do {
		struct rusage	rusage;
		int		status;

		pid = wait4(-1, &status, WNOHANG, &rusage);
		switch (pid) {
			case -1 :	// if error
				if (errno != ECHILD) {
					perror("wait4");
				}
				break;

			case  0 :	// if no more children
				break;

			default : {
				childInfoRef	last;
				childInfoRef	this;

				// grab the activeChildren mutex
				pthread_mutex_lock(&lock);

				last = NULL;
				this = activeChildren;
				while (this) {
					if (this->pid == pid) {
						/* save exit status & usage */
						this->status = status;
						this->rusage = rusage;

						/* remove from activeChildren */
						if (last) {
							last->next = this->next;
						} else {
							activeChildren = this->next;
						}

						/* add to reapedChildren */
						this->next = reapedChildren;
						reapedChildren = this;

						break;
					} else {
						/* if not this child */
						last = this;
						this = this->next;
					}
				}

				// release the activeChildren mutex
				pthread_mutex_unlock(&lock);

				break;
			}
		}
	} while (pid > 0);

	/*
	 * we need to know about any new children waiting to be reaped so
	 * re-enable the SIGCHLD handler.

	 */
	unblockSignal();

	while (reapedChildren) {
		childInfoRef	child = reapedChildren;

		reapedChildren = reapedChildren->next;
		(*child->callout)(child->pid,
				  child->status,
				  &child->rusage,
				  child->context);
		CFAllocatorDeallocate(NULL, child);
	}

	return;
}


void
_SCDPluginExecInit()
{
	struct sigaction	act;
	CFRunLoopSourceRef	rls;

	// create the "a child has been reaped" notification port
	childReaped = CFMachPortCreate(NULL, childrenReaped, NULL, NULL);

	// set queue limit
	{
		mach_port_limits_t	limits;
		kern_return_t		status;

		limits.mpl_qlimit = 1;
		status = mach_port_set_attributes(mach_task_self(),
						  CFMachPortGetPort(childReaped),
						  MACH_PORT_LIMITS_INFO,
						  (mach_port_info_t)&limits,
						  MACH_PORT_LIMITS_INFO_COUNT);
		if (status != KERN_SUCCESS) {
			perror("mach_port_set_attributes");
		}
	}

	// add to our runloop
	rls = CFMachPortCreateRunLoopSource(NULL, childReaped, 0);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);
	CFRelease(rls);

	// enable signal handler
	act.sa_handler = reaper;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART|SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &act, NULL) == -1) {
		perror("sigaction");
	}

	return;
}


pid_t
_SCDPluginExecCommand2(SCDPluginExecCallBack	callout,
		       void			*context,
		       uid_t			uid,
		       gid_t			gid,
		       const char		*path,
		       char * const 		argv[],
		       SCDPluginExecSetup	setup,
		       void			*setupContext
		       )
{
	pid_t	pid;

	// grab the activeChildren mutex
	pthread_mutex_lock(&lock);

	pid = fork();

	switch (pid) {
		case -1 : {	/* if error */

			int	status;

			status = errno;
			printf("fork() failed: %s\n", strerror(status));
			errno  = status;
			break;
		}

		case 0 : {	/* if child */

			uid_t	curUID	= geteuid();
			gid_t	curGID	= getegid();
			int	i;
			int	status;

			if (curUID != uid) {
				(void) setuid(uid);
			}

			if (curGID != gid) {
				(void) setgid(gid);
			}

			if (setup) {
				(setup)(pid, setupContext);
			} else {
				/* close any open FDs */
				for (i = getdtablesize()-1; i>=0; i--) close(i);
				open(_PATH_DEVNULL, O_RDWR, 0);
				dup(0);
				dup(0);
			}

			/* ensure that our PATH environment variable is somewhat reasonable */
			if (setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 0) == -1) {
				printf("setenv() failed: %s\n", strerror(errno));
				exit(EX_OSERR);
			}

			/* execute requested command */
			(void) execv(path, argv);

			/* if the execv failed */
			status = W_EXITCODE(errno, 0);
			_exit (WEXITSTATUS(status));
		}

		default : {	/* if parent */
			if (setup) {
				(setup)(pid, setupContext);
			}

			if (callout) {
				childInfoRef	child;

				// create child process info
				child = CFAllocatorAllocate(NULL, sizeof(struct childInfo), 0);
				bzero(child, sizeof(struct childInfo));
				child->pid     = pid;
				child->callout = callout;
				child->context = context;

				// add the new child to the activeChildren list
				child->next = activeChildren;
				activeChildren = child;
			}
			break;
		}
	}

	// release the activeChildren mutex
	pthread_mutex_unlock(&lock);

	return pid;
}


pid_t
_SCDPluginExecCommand(SCDPluginExecCallBack	callout,
		     void			*context,
		     uid_t			uid,
		     gid_t			gid,
		     const char			*path,
		     char * const		argv[])
{
	return _SCDPluginExecCommand2(callout, context, uid, gid, path, argv, NULL, NULL);
}
