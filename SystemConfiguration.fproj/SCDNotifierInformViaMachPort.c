/*
 * Copyright (c) 2000, 2001, 2004, 2005, 2009 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
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
 * June 1, 2001			Allan Nathanson <ajn@apple.com>
 * - public API conversion
 *
 * March 31, 2000		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */

#include <mach/mach.h>
#include <mach/mach_error.h>

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCPrivate.h>
#include "SCDynamicStoreInternal.h"
#include "config.h"		/* MiG generated file */

Boolean
SCDynamicStoreNotifyMachPort(SCDynamicStoreRef store, mach_msg_id_t identifier, mach_port_t *port)
{
	SCDynamicStorePrivateRef	storePrivate = (SCDynamicStorePrivateRef)store;
	kern_return_t			status;
	mach_port_t			oldNotify;
	int				sc_status;

	if (store == NULL) {
		/* sorry, you must provide a session */
		_SCErrorSet(kSCStatusNoStoreSession);
		return FALSE;
	}

	if (storePrivate->server == MACH_PORT_NULL) {
		/* sorry, you must have an open session to play */
		_SCErrorSet(kSCStatusNoStoreServer);
		return FALSE;
	}

	if (storePrivate->notifyStatus != NotifierNotRegistered) {
		/* sorry, you can only have one notification registered at once */
		_SCErrorSet(kSCStatusNotifierActive);
		return FALSE;
	}

	/* Allocating port (for server response) */
	status = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, port);
	if (status != KERN_SUCCESS) {
		SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreNotifyMachPort mach_port_allocate(): %s"), mach_error_string(status));
		_SCErrorSet(status);
		return FALSE;
	}

	status = mach_port_insert_right(mach_task_self(),
					*port,
					*port,
					MACH_MSG_TYPE_MAKE_SEND);
	if (status != KERN_SUCCESS) {
		/*
		 * We can't insert a send right into our own port!  This should
		 * only happen if someone stomped on OUR port (so let's leave
		 * the port alone).
		 */
		SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreNotifyMachPort mach_port_insert_right(): %s"), mach_error_string(status));
		*port = MACH_PORT_NULL;
		_SCErrorSet(status);
		return FALSE;
	}

	/* Request a notification when/if the server dies */
	status = mach_port_request_notification(mach_task_self(),
						*port,
						MACH_NOTIFY_NO_SENDERS,
						1,
						*port,
						MACH_MSG_TYPE_MAKE_SEND_ONCE,
						&oldNotify);
	if (status != KERN_SUCCESS) {
		/*
		 * We can't request a notification for our own port!  This should
		 * only happen if someone stomped on OUR port (so let's leave
		 * the port alone).
		 */
		SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreNotifyMachPort mach_port_request_notification(): %s"), mach_error_string(status));
		*port = MACH_PORT_NULL;
		_SCErrorSet(status);
		return FALSE;
	}

	if (oldNotify != MACH_PORT_NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreNotifyMachPort(): oldNotify != MACH_PORT_NULL"));
	}

	status = notifyviaport(storePrivate->server,
			       *port,
			       identifier,
			       (int *)&sc_status);

	if (status != KERN_SUCCESS) {
		if (status == MACH_SEND_INVALID_DEST) {
			/* the server's gone and our session port's dead, remove the dead name right */
			(void) mach_port_deallocate(mach_task_self(), storePrivate->server);
		} else {
			/* we got an unexpected error, leave the [session] port alone */
			SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreNotifyMachPort notifyviaport(): %s"), mach_error_string(status));
		}
		storePrivate->server = MACH_PORT_NULL;

		if (status == MACH_SEND_INVALID_DEST) {
			/* remove the send right that we tried (but failed) to pass to the server */
			(void) mach_port_deallocate(mach_task_self(), *port);
		}

		/* remove our receive right  */
		(void) mach_port_mod_refs(mach_task_self(), *port, MACH_PORT_RIGHT_RECEIVE, -1);
		*port = MACH_PORT_NULL;
		_SCErrorSet(status);
		return FALSE;
	}

	/* set notifier active */
	__MACH_PORT_DEBUG(TRUE, "*** SCDynamicStoreNotifyMachPort", *port);
	storePrivate->notifyStatus = Using_NotifierInformViaMachPort;

	return TRUE;
}
