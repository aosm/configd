/*
 * Copyright (c) 2000-2005, 2009 Apple Inc. All rights reserved.
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

#include <mach/mach.h>
#include <mach/mach_error.h>

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCPrivate.h>
#include "SCDynamicStoreInternal.h"
#include "config.h"		/* MiG generated file */

Boolean
SCDynamicStoreAddTemporaryValue(SCDynamicStoreRef store, CFStringRef key, CFPropertyListRef value)
{
	SCDynamicStorePrivateRef	storePrivate = (SCDynamicStorePrivateRef)store;
	kern_return_t			status;
	CFDataRef			utfKey;		/* serialized key */
	xmlData_t			myKeyRef;
	CFIndex				myKeyLen;
	CFDataRef			xmlData;	/* serialized data */
	xmlData_t			myDataRef;
	CFIndex				myDataLen;
	int				newInstance;
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

	/* serialize the key */
	if (!_SCSerializeString(key, &utfKey, (void **)&myKeyRef, &myKeyLen)) {
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	/* serialize the data */
	if (!_SCSerialize(value, &xmlData, (void **)&myDataRef, &myDataLen)) {
		CFRelease(utfKey);
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	/* send the key & data to the server */
	status = configadd_s(storePrivate->server,
			     myKeyRef,
			     myKeyLen,
			     myDataRef,
			     myDataLen,
			     &newInstance,
			     (int *)&sc_status);

	/* clean up */
	CFRelease(utfKey);
	CFRelease(xmlData);

	if (status != KERN_SUCCESS) {
		if (status == MACH_SEND_INVALID_DEST) {
			/* the server's gone and our session port's dead, remove the dead name right */
			(void) mach_port_deallocate(mach_task_self(), storePrivate->server);
		} else {
			/* we got an unexpected error, leave the [session] port alone */
			SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreAddTemporaryValue configadd_s(): %s"), mach_error_string(status));
		}
		storePrivate->server = MACH_PORT_NULL;
		_SCErrorSet(status);
		return FALSE;
	}

	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return FALSE;
	}

	return TRUE;
}

Boolean
SCDynamicStoreAddValue(SCDynamicStoreRef store, CFStringRef key, CFPropertyListRef value)
{
	SCDynamicStorePrivateRef	storePrivate = (SCDynamicStorePrivateRef)store;
	kern_return_t			status;
	CFDataRef			utfKey;		/* serialized key */
	xmlData_t			myKeyRef;
	CFIndex				myKeyLen;
	CFDataRef			xmlData;	/* serialized data */
	xmlData_t			myDataRef;
	CFIndex				myDataLen;
	int				newInstance;
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

	/* serialize the key */
	if (!_SCSerializeString(key, &utfKey, (void **)&myKeyRef, &myKeyLen)) {
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	/* serialize the data */
	if (!_SCSerialize(value, &xmlData, (void **)&myDataRef, &myDataLen)) {
		CFRelease(utfKey);
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	/* send the key & data to the server */
	status = configadd(storePrivate->server,
			   myKeyRef,
			   myKeyLen,
			   myDataRef,
			   myDataLen,
			   &newInstance,
			   (int *)&sc_status);

	/* clean up */
	CFRelease(utfKey);
	CFRelease(xmlData);

	if (status != KERN_SUCCESS) {
		if (status == MACH_SEND_INVALID_DEST) {
			/* the server's gone and our session port's dead, remove the dead name right */
			(void) mach_port_deallocate(mach_task_self(), storePrivate->server);
		} else {
			/* we got an unexpected error, leave the [session] port alone */
			SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreAddValue configadd(): %s"), mach_error_string(status));
		}
		storePrivate->server = MACH_PORT_NULL;
		_SCErrorSet(status);
		return FALSE;
	}

	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return FALSE;
	}

	return TRUE;
}
