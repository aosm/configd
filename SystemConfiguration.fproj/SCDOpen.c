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

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCPrivate.h>
#include "SCDynamicStoreInternal.h"
#include "config.h"		/* MiG generated file */


static int		_sc_active	= 0;
static CFStringRef	_sc_bundleID	= NULL;
static pthread_mutex_t	_sc_lock	= PTHREAD_MUTEX_INITIALIZER;
static mach_port_t	_sc_server	= MACH_PORT_NULL;


static CFStringRef
__SCDynamicStoreCopyDescription(CFTypeRef cf) {
	CFAllocatorRef			allocator	= CFGetAllocator(cf);
	CFMutableStringRef		result;
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)cf;

	result = CFStringCreateMutable(allocator, 0);
	CFStringAppendFormat(result, NULL, CFSTR("<SCDynamicStore %p [%p]> {"), cf, allocator);
	if (storePrivate->server != MACH_PORT_NULL) {
		CFStringAppendFormat(result, NULL, CFSTR("server port = %p"), storePrivate->server);
	} else {
		CFStringAppendFormat(result, NULL, CFSTR("server not (no longer) available"));
	}
	if (storePrivate->locked) {
		CFStringAppendFormat(result, NULL, CFSTR(", locked"));
	}
	switch (storePrivate->notifyStatus) {
		case Using_NotifierWait :
			CFStringAppendFormat(result, NULL, CFSTR(", waiting for a notification"));
			break;
		case Using_NotifierInformViaMachPort :
			CFStringAppendFormat(result, NULL, CFSTR(", mach port notifications"));
			break;
		case Using_NotifierInformViaFD :
			CFStringAppendFormat(result, NULL, CFSTR(", FD notifications"));
			break;
		case Using_NotifierInformViaSignal :
			CFStringAppendFormat(result, NULL, CFSTR(", BSD signal notifications"));
			break;
		case Using_NotifierInformViaRunLoop :
		case Using_NotifierInformViaCallback :
			if (storePrivate->notifyStatus == Using_NotifierInformViaRunLoop) {
				CFStringAppendFormat(result, NULL, CFSTR(", runloop notifications"));
				CFStringAppendFormat(result, NULL, CFSTR(" {callout = %p"), storePrivate->rlsFunction);
				CFStringAppendFormat(result, NULL, CFSTR(", info = %p"), storePrivate->rlsContext.info);
				CFStringAppendFormat(result, NULL, CFSTR(", rls = %p"), storePrivate->rls);
				CFStringAppendFormat(result, NULL, CFSTR(", refs = %d"), storePrivate->rlsRefs);
			} else {
				CFStringAppendFormat(result, NULL, CFSTR(", mach port/callback notifications"));
				CFStringAppendFormat(result, NULL, CFSTR(" {callout = %p"), storePrivate->callbackFunction);
				CFStringAppendFormat(result, NULL, CFSTR(", info = %p"), storePrivate->callbackArgument);
			}
			if (storePrivate->callbackRLS != NULL) {
				CFStringAppendFormat(result, NULL, CFSTR(", notify rls = %@" ), storePrivate->callbackRLS);
			}
			CFStringAppendFormat(result, NULL, CFSTR("}"));
			break;
		default :
			CFStringAppendFormat(result, NULL, CFSTR(", notification delivery not requested%s"),
					     storePrivate->rlsFunction ? " (yet)" : "");
			break;
	}
	CFStringAppendFormat(result, NULL, CFSTR("}"));

	return result;
}


static void
__SCDynamicStoreDeallocate(CFTypeRef cf)
{
	int				oldThreadState;
	int				sc_status;
	kern_return_t			status;
	SCDynamicStoreRef		store		= (SCDynamicStoreRef)cf;
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;

	(void) pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldThreadState);

	/* Remove/cancel any outstanding notification requests. */
	(void) SCDynamicStoreNotifyCancel(store);

	if ((storePrivate->server != MACH_PORT_NULL) && storePrivate->locked) {
		(void) SCDynamicStoreUnlock(store);	/* release the lock */
	}

	if (storePrivate->server != MACH_PORT_NULL) {
		status = configclose(storePrivate->server, (int *)&sc_status);
#ifdef	DEBUG
		if (status != KERN_SUCCESS) {
			if (status != MACH_SEND_INVALID_DEST)
				SCLog(_sc_verbose, LOG_DEBUG, CFSTR("__SCDynamicStoreDeallocate configclose(): %s"), mach_error_string(status));
		}
#endif	/* DEBUG */

		(void) mach_port_destroy(mach_task_self(), storePrivate->server);
		storePrivate->server = MACH_PORT_NULL;
	}

	(void) pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldThreadState);
	pthread_testcancel();

	/* release any callback context info */
	if (storePrivate->rlsContext.release != NULL) {
		(*storePrivate->rlsContext.release)(storePrivate->rlsContext.info);
	}

	/* release any keys being watched */
	CFRelease(storePrivate->keys);
	CFRelease(storePrivate->patterns);

	/* cleanup */
	pthread_mutex_lock(&_sc_lock);
	_sc_active--;			/* drop the number of active dynamic store sessions */
	if ((_sc_active == 0) && (_sc_server != MACH_PORT_NULL)) {
		/* release the [last] reference to the server */
		(void)mach_port_deallocate(mach_task_self(), _sc_server);
		_sc_server = MACH_PORT_NULL;
	}
	pthread_mutex_unlock(&_sc_lock);

	return;
}


static CFTypeID __kSCDynamicStoreTypeID = _kCFRuntimeNotATypeID;


static const CFRuntimeClass __SCDynamicStoreClass = {
	0,				// version
	"SCDynamicStore",		// className
	NULL,				// init
	NULL,				// copy
	__SCDynamicStoreDeallocate,	// dealloc
	NULL,				// equal
	NULL,				// hash
	NULL,				// copyFormattingDesc
	__SCDynamicStoreCopyDescription	// copyDebugDesc
};


static void
childForkHandler()
{
	/* the process has forked (and we are the child process) */

	_sc_active = 0;
	_sc_server = MACH_PORT_NULL;

	return;
}


static pthread_once_t initialized	= PTHREAD_ONCE_INIT;

static void
__SCDynamicStoreInitialize(void) {
	CFBundleRef	bundle;

	/* register with CoreFoundation */
	__kSCDynamicStoreTypeID = _CFRuntimeRegisterClass(&__SCDynamicStoreClass);

	/* add handler to cleanup after fork() */
	(void) pthread_atfork(NULL, NULL, childForkHandler);

	/* get the application/executable/bundle name */
	bundle = CFBundleGetMainBundle();
	if (bundle != NULL) {
		_sc_bundleID = CFBundleGetIdentifier(bundle);
		if (_sc_bundleID != NULL) {
			CFRetain(_sc_bundleID);
		} else {
			CFURLRef	url;

			url = CFBundleCopyExecutableURL(bundle);
			if (url != NULL) {
				_sc_bundleID = CFURLCopyPath(url);
				CFRelease(url);
			}
		}

		if (_sc_bundleID != NULL) {
			if (CFEqual(_sc_bundleID, CFSTR("/"))) {
				CFRelease(_sc_bundleID);
				_sc_bundleID = CFStringCreateWithFormat(NULL, NULL, CFSTR("(%d)"), getpid());
			}
		}
	}

	return;
}


SCDynamicStorePrivateRef
__SCDynamicStoreCreatePrivate(CFAllocatorRef		allocator,
			     const CFStringRef		name,
			     SCDynamicStoreCallBack	callout,
			     SCDynamicStoreContext	*context)
{
	int				sc_status	= kSCStatusOK;
	uint32_t			size;
	SCDynamicStorePrivateRef	storePrivate;

	/* initialize runtime */
	pthread_once(&initialized, __SCDynamicStoreInitialize);


	/* allocate session */
	size  = sizeof(SCDynamicStorePrivate) - sizeof(CFRuntimeBase);
	storePrivate = (SCDynamicStorePrivateRef)_CFRuntimeCreateInstance(allocator,
									  __kSCDynamicStoreTypeID,
									  size,
									  NULL);
	if (storePrivate == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	/* server side of the "configd" session */
	storePrivate->server				= MACH_PORT_NULL;

	/* flags */
	storePrivate->locked				= FALSE;
	storePrivate->useSessionKeys			= FALSE;

	/* Notification status */
	storePrivate->notifyStatus			= NotifierNotRegistered;

	/* "client" information associated with SCDynamicStoreCreateRunLoopSource() */
	storePrivate->rlsRefs				= 0;
	storePrivate->rls				= NULL;
	storePrivate->rlsFunction			= callout;
	storePrivate->rlsContext.info			= NULL;
	storePrivate->rlsContext.retain			= NULL;
	storePrivate->rlsContext.release		= NULL;
	storePrivate->rlsContext.copyDescription	= NULL;
	if (context) {
		bcopy(context, &storePrivate->rlsContext, sizeof(SCDynamicStoreContext));
		if (context->retain != NULL) {
			storePrivate->rlsContext.info = (void *)(*context->retain)(context->info);
		}
	}

	/* "client" information associated with SCDynamicStoreNotifyCallback() */
	storePrivate->callbackFunction			= NULL;
	storePrivate->callbackArgument			= NULL;
	storePrivate->callbackPort			= NULL;
	storePrivate->callbackRLS			= NULL;

	/* "server" information associated with SCDynamicStoreSetNotificationKeys() */
	storePrivate->keys				= CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	storePrivate->patterns				= CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

	/* "server" information associated with SCDynamicStoreNotifyMachPort(); */
	storePrivate->notifyPort			= MACH_PORT_NULL;
	storePrivate->notifyPortIdentifier		= 0;

	/* "server" information associated with SCDynamicStoreNotifyFileDescriptor(); */
	storePrivate->notifyFile			= -1;
	storePrivate->notifyFileIdentifier		= 0;

	/* "server" information associated with SCDynamicStoreNotifySignal(); */
	storePrivate->notifySignal			= 0;
	storePrivate->notifySignalTask			= TASK_NULL;

	/* initialize global state */

	pthread_mutex_lock(&_sc_lock);

	/* get the server port */
	if (_sc_server == MACH_PORT_NULL) {
		char		*server_name;
		kern_return_t	status;

		server_name = getenv("SCD_SERVER");
		if (!server_name) {
			server_name = SCD_SERVER;
		}

		status = bootstrap_look_up(bootstrap_port, server_name, &_sc_server);
		switch (status) {
			case BOOTSTRAP_SUCCESS :
				/* service currently registered, "a good thing" (tm) */
				break;
			case BOOTSTRAP_UNKNOWN_SERVICE :
				/* service not currently registered, try again later */
				sc_status = status;
				goto done;
			default :
#ifdef	DEBUG
				SCLog(_sc_verbose, LOG_DEBUG, CFSTR("SCDynamicStoreCreate[WithOptions] bootstrap_look_up() failed: status=%d"), status);
#endif	/* DEBUG */
				sc_status = status;
				goto done;
		}
	}

	/* bump the number of active dynamic store sessions */
	_sc_active++;

    done :

	pthread_mutex_unlock(&_sc_lock);

	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		CFRelease(storePrivate);
		storePrivate = NULL;
	}

	return storePrivate;
}


const CFStringRef	kSCDynamicStoreUseSessionKeys	= CFSTR("UseSessionKeys");	/* CFBoolean */


SCDynamicStoreRef
SCDynamicStoreCreateWithOptions(CFAllocatorRef		allocator,
				CFStringRef		name,
				CFDictionaryRef		storeOptions,
				SCDynamicStoreCallBack	callout,
				SCDynamicStoreContext	*context)
{
	SCDynamicStorePrivateRef	storePrivate;
	kern_return_t			status;
	CFDataRef			utfName;		/* serialized name */
	xmlData_t			myNameRef;
	CFIndex				myNameLen;
	CFDataRef			xmlOptions	= NULL;	/* serialized options */
	xmlData_t			myOptionsRef	= NULL;
	CFIndex				myOptionsLen	= 0;
	int				sc_status	= kSCStatusFailed;

	/*
	 * allocate and initialize a new session
	 */
	storePrivate = __SCDynamicStoreCreatePrivate(allocator, name, callout, context);
	if (storePrivate == NULL) {
		return NULL;
	}

	if (_sc_bundleID != NULL) {
		CFStringRef	fullName;

		fullName = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@:%@"), _sc_bundleID, name);
		name = fullName;
	} else {
		CFRetain(name);
	}

	if (!_SCSerializeString(name, &utfName, (void **)&myNameRef, &myNameLen)) {
		CFRelease(name);
		goto done;
	}
	CFRelease(name);

	/* serialize the options */
	if (storeOptions != NULL) {
		if (!_SCSerialize(storeOptions, &xmlOptions, (void **)&myOptionsRef, &myOptionsLen)) {
			CFRelease(utfName);
			goto done;
		}
	}

	/* open a new session with the server */
	status = configopen(_sc_server,
			    myNameRef,
			    myNameLen,
			    myOptionsRef,
			    myOptionsLen,
			    &storePrivate->server,
			    (int *)&sc_status);

	/* clean up */
	CFRelease(utfName);
	if (xmlOptions)	CFRelease(xmlOptions);

	if (status != KERN_SUCCESS) {
#ifdef	DEBUG
		if (status != MACH_SEND_INVALID_DEST)
			SCLog(_sc_verbose, LOG_DEBUG, CFSTR("SCDynamicStoreCreate[WithOptions] configopen(): %s"), mach_error_string(status));
#endif	/* DEBUG */
		sc_status = status;
		goto done;
	}

    done :

	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		CFRelease(storePrivate);
		storePrivate = NULL;
	}

	return (SCDynamicStoreRef)storePrivate;
}


SCDynamicStoreRef
SCDynamicStoreCreate(CFAllocatorRef		allocator,
		     CFStringRef		name,
		     SCDynamicStoreCallBack	callout,
		     SCDynamicStoreContext	*context)
{
	return SCDynamicStoreCreateWithOptions(allocator, name, NULL, callout, context);
}


CFTypeID
SCDynamicStoreGetTypeID(void) {
	pthread_once(&initialized, __SCDynamicStoreInitialize);	/* initialize runtime */
	return __kSCDynamicStoreTypeID;
}
