/*
 * Copyright (c) 2000-2006 Apple Computer, Inc. All rights reserved.
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
 * March 24, 2000		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */

#include "configd.h"
#include "configd_server.h"
#include "session.h"

#include <bsm/libbsm.h>
#include <sys/types.h>
#include <unistd.h>

__private_extern__
int
__SCDynamicStoreOpen(SCDynamicStoreRef *store, CFStringRef name)
{
	/*
	 * allocate and initialize a new session
	 */
	*store = (SCDynamicStoreRef)__SCDynamicStoreCreatePrivate(NULL, name, NULL, NULL);

	/*
	 * If necessary, initialize the store and session data dictionaries
	 */
	if (storeData == NULL) {
		sessionData        = CFDictionaryCreateMutable(NULL,
							       0,
							       &kCFTypeDictionaryKeyCallBacks,
							       &kCFTypeDictionaryValueCallBacks);
		storeData          = CFDictionaryCreateMutable(NULL,
							       0,
							       &kCFTypeDictionaryKeyCallBacks,
							       &kCFTypeDictionaryValueCallBacks);
		patternData        = CFDictionaryCreateMutable(NULL,
							       0,
							       &kCFTypeDictionaryKeyCallBacks,
							       &kCFTypeDictionaryValueCallBacks);
		changedKeys        = CFSetCreateMutable(NULL,
							0,
							&kCFTypeSetCallBacks);
		deferredRemovals   = CFSetCreateMutable(NULL,
							0,
							&kCFTypeSetCallBacks);
		removedSessionKeys = CFSetCreateMutable(NULL,
							0,
							&kCFTypeSetCallBacks);
	}

	return kSCStatusOK;
}


static CFStringRef
openMPCopyDescription(const void *info)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("<SCDynamicStore MP>"));
}


__private_extern__
kern_return_t
_configopen(mach_port_t			server,
	    xmlData_t			nameRef,		/* raw XML bytes */
	    mach_msg_type_number_t	nameLen,
	    xmlData_t			optionsRef,		/* raw XML bytes */
	    mach_msg_type_number_t	optionsLen,
	    mach_port_t			*newServer,
	    int				*sc_status,
	    audit_token_t		audit_token)
{
	CFMachPortContext		context		= { 0
							  , (void *)1
							  , NULL
							  , NULL
							  , openMPCopyDescription
							  };
	CFDictionaryRef			info;
	CFMachPortRef			mp;
	serverSessionRef		mySession;
	CFStringRef			name		= NULL;	/* name (un-serialized) */
	CFMutableDictionaryRef		newInfo;
	serverSessionRef		newSession;
	mach_port_t			oldNotify;
	CFDictionaryRef			options		= NULL;	/* options (un-serialized) */
	CFStringRef			sessionKey;
	kern_return_t 			status;
	SCDynamicStorePrivateRef	storePrivate;
	CFBooleanRef			useSessionKeys	= NULL;

	*sc_status = kSCStatusOK;

	/* un-serialize the name */
	if (!_SCUnserializeString(&name, NULL, (void *)nameRef, nameLen)) {
		*sc_status = kSCStatusFailed;
	}

	if ((optionsRef != NULL) && (optionsLen > 0)) {
		/* un-serialize the [session] options */
		if (!_SCUnserialize((CFPropertyListRef *)&options, NULL, (void *)optionsRef, optionsLen)) {
			*sc_status = kSCStatusFailed;
		}
	}

	if (*sc_status != kSCStatusOK) {
		goto done;
	}

	if (!isA_CFString(name)) {
		*sc_status = kSCStatusInvalidArgument;
		goto done;
	}

	if (options != NULL) {
		if (!isA_CFDictionary(options)) {
			*sc_status = kSCStatusInvalidArgument;
			goto done;
		}

		/*
		 * [pre-]process any provided options
		 */
		useSessionKeys = CFDictionaryGetValue(options, kSCDynamicStoreUseSessionKeys);
		if (useSessionKeys != NULL) {
			if (!isA_CFBoolean(useSessionKeys)) {
				*sc_status = kSCStatusInvalidArgument;
				goto done;
			}
		}
	}

	mySession = getSession(server);
	if (mySession->store != NULL) {
#ifdef	DEBUG
		SCLog(TRUE, LOG_DEBUG, CFSTR("_configopen(): session is already open."));
#endif	/* DEBUG */
		*sc_status = kSCStatusFailed;	/* you can't re-open an "open" session */
		goto done;
	}

	/* Create the server port for this session */
	mp = CFMachPortCreate(NULL, configdCallback, &context, NULL);

	/* return the newly allocated port to be used for this session */
	*newServer = CFMachPortGetPort(mp);

	/*
	 * establish the new session
	 */
	newSession = addSession(mp);

	/*
	 * get the credentials associated with the caller.
	 */
	audit_token_to_au32(audit_token,
			    NULL,			// auidp
			    &newSession->callerEUID,	// euid
			    NULL,			// egid
			    NULL,			// ruid
			    NULL,			// rgid
			    NULL,			// pid
			    NULL,			// asid
			    NULL);			// tid

	/* Create and add a run loop source for the port */
	newSession->serverRunLoopSource = CFMachPortCreateRunLoopSource(NULL, mp, 0);
	CFRunLoopAddSource(CFRunLoopGetCurrent(),
			   newSession->serverRunLoopSource,
			   kCFRunLoopDefaultMode);

	if (_configd_trace) {
		SCTrace(TRUE, _configd_trace,
			CFSTR("open    : %5d : %@\n"),
			*newServer,
			name);
	}

	*sc_status = __SCDynamicStoreOpen(&newSession->store, name);
	storePrivate = (SCDynamicStorePrivateRef)newSession->store;

	/*
	 * Make the server port accessible to the framework routines.
	 */
	storePrivate->server = *newServer;

	/*
	 * Process any provided [session] options
	 */
	if (useSessionKeys != NULL) {
		storePrivate->useSessionKeys = CFBooleanGetValue(useSessionKeys);
	}

	/* Request a notification when/if the client dies */
	status = mach_port_request_notification(mach_task_self(),
						*newServer,
						MACH_NOTIFY_NO_SENDERS,
						1,
						*newServer,
						MACH_MSG_TYPE_MAKE_SEND_ONCE,
						&oldNotify);
	if (status != KERN_SUCCESS) {
		SCLog(TRUE, LOG_DEBUG, CFSTR("_configopen() mach_port_request_notification() failed: %s"), mach_error_string(status));
		cleanupSession(*newServer);
		*newServer = MACH_PORT_NULL;
		*sc_status = kSCStatusFailed;
		goto done;
	}

#ifdef	DEBUG
	if (oldNotify != MACH_PORT_NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("_configopen(): why is oldNotify != MACH_PORT_NULL?"));
	}
#endif	/* DEBUG */

	/*
	 * Save the name of the calling application / plug-in with the session data.
	 */
	sessionKey = CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), *newServer);
	info = CFDictionaryGetValue(sessionData, sessionKey);
	if (info != NULL) {
		newInfo = CFDictionaryCreateMutableCopy(NULL, 0, info);
	} else {
		newInfo = CFDictionaryCreateMutable(NULL,
						    0,
						    &kCFTypeDictionaryKeyCallBacks,
						    &kCFTypeDictionaryValueCallBacks);
	}
	CFDictionarySetValue(newInfo, kSCDName, name);
	CFDictionarySetValue(sessionData, sessionKey, newInfo);
	CFRelease(newInfo);
	CFRelease(sessionKey);

    done :

	if (name != NULL)	CFRelease(name);
	if (options != NULL)	CFRelease(options);
	return KERN_SUCCESS;
}
