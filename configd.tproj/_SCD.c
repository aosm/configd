/*
 * Copyright (c) 2000, 2001, 2003-2005 Apple Computer, Inc. All rights reserved.
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
 * June 2, 2000			Allan Nathanson <ajn@apple.com>
 * - initial revision
 */


#include <unistd.h>

#include "configd.h"
#include "configd_server.h"
#include "session.h"


__private_extern__ CFMutableDictionaryRef	sessionData		= NULL;

__private_extern__ CFMutableDictionaryRef	storeData		= NULL;
__private_extern__ CFMutableDictionaryRef	storeData_s		= NULL;

__private_extern__ CFMutableDictionaryRef	patternData		= NULL;
__private_extern__ CFMutableDictionaryRef	patternData_s		= NULL;

__private_extern__ CFMutableSetRef		changedKeys		= NULL;
__private_extern__ CFMutableSetRef		changedKeys_s		= NULL;

__private_extern__ CFMutableSetRef		deferredRemovals	= NULL;
__private_extern__ CFMutableSetRef		deferredRemovals_s	= NULL;

__private_extern__ CFMutableSetRef		removedSessionKeys	= NULL;
__private_extern__ CFMutableSetRef		removedSessionKeys_s	= NULL;

__private_extern__ CFMutableSetRef		needsNotification	= NULL;

__private_extern__ int				storeLocked		= 0;		/* > 0 if dynamic store locked */


__private_extern__
void
_swapLockedStoreData()
{
	void	*temp;

	temp                 = storeData;
	storeData            = storeData_s;
	storeData_s          = temp;

	temp                 = patternData;
	patternData          = patternData_s;
	patternData_s        = temp;

	temp                 = changedKeys;
	changedKeys          = changedKeys_s;
	changedKeys_s        = temp;

	temp                 = deferredRemovals;
	deferredRemovals     = deferredRemovals_s;
	deferredRemovals_s   = temp;

	temp                 = removedSessionKeys;
	removedSessionKeys   = removedSessionKeys_s;
	removedSessionKeys_s = temp;

	return;
}


__private_extern__
void
_addWatcher(CFNumberRef sessionNum, CFStringRef watchedKey)
{
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFArrayRef		watchers;
	CFMutableArrayRef	newWatchers;
	CFArrayRef		watcherRefs;
	CFMutableArrayRef	newWatcherRefs;
	CFIndex			i;
	int			refCnt;
	CFNumberRef		refNum;

	/*
	 * Get the dictionary associated with this key out of the store
	 */
	dict = CFDictionaryGetValue(storeData, watchedKey);
	if (dict) {
		newDict = CFDictionaryCreateMutableCopy(NULL, 0, dict);
	} else {
		newDict = CFDictionaryCreateMutable(NULL,
						    0,
						    &kCFTypeDictionaryKeyCallBacks,
						    &kCFTypeDictionaryValueCallBacks);
	}

	/*
	 * Get the set of watchers out of the keys dictionary
	 */
	watchers    = CFDictionaryGetValue(newDict, kSCDWatchers);
	watcherRefs = CFDictionaryGetValue(newDict, kSCDWatcherRefs);
	if (watchers) {
		newWatchers    = CFArrayCreateMutableCopy(NULL, 0, watchers);
		newWatcherRefs = CFArrayCreateMutableCopy(NULL, 0, watcherRefs);
	} else {
		newWatchers    = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		newWatcherRefs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	}

	/*
	 * Add my session to the set of watchers
	 */
	i = CFArrayGetFirstIndexOfValue(newWatchers,
					CFRangeMake(0, CFArrayGetCount(newWatchers)),
					sessionNum);
	if (i == kCFNotFound) {
		/* if this is the first instance of this session watching this key */
		CFArrayAppendValue(newWatchers, sessionNum);
		refCnt = 1;
		refNum = CFNumberCreate(NULL, kCFNumberIntType, &refCnt);
		CFArrayAppendValue(newWatcherRefs, refNum);
		CFRelease(refNum);
	} else {
		/* if this is another instance of this session watching this key */
		refNum = CFArrayGetValueAtIndex(newWatcherRefs, i);
		CFNumberGetValue(refNum, kCFNumberIntType, &refCnt);
		refCnt++;
		refNum = CFNumberCreate(NULL, kCFNumberIntType, &refCnt);
		CFArraySetValueAtIndex(newWatcherRefs, i, refNum);
		CFRelease(refNum);
	}

	/*
	 * Update the keys dictionary
	 */
	CFDictionarySetValue(newDict, kSCDWatchers, newWatchers);
	CFRelease(newWatchers);
	CFDictionarySetValue(newDict, kSCDWatcherRefs, newWatcherRefs);
	CFRelease(newWatcherRefs);

	/*
	 * Update the store for this key
	 */
	CFDictionarySetValue(storeData, watchedKey, newDict);
	CFRelease(newDict);

#ifdef	DEBUG
	SCLog(_configd_verbose, LOG_DEBUG, CFSTR("  _addWatcher: %@, %@"), sessionNum, watchedKey);
#endif	/* DEBUG */

	return;
}


__private_extern__
void
_removeWatcher(CFNumberRef sessionNum, CFStringRef watchedKey)
{
	CFDictionaryRef		dict;
	CFMutableDictionaryRef	newDict;
	CFArrayRef		watchers;
	CFMutableArrayRef	newWatchers;
	CFArrayRef		watcherRefs;
	CFMutableArrayRef	newWatcherRefs;
	CFIndex			i;
	int			refCnt;
	CFNumberRef		refNum;

	/*
	 * Get the dictionary associated with this key out of the store
	 */
	dict = CFDictionaryGetValue(storeData, watchedKey);
	if ((dict == NULL) || (CFDictionaryContainsKey(dict, kSCDWatchers) == FALSE)) {
		/* key doesn't exist (isn't this really fatal?) */
#ifdef	DEBUG
		SCLog(_configd_verbose, LOG_DEBUG, CFSTR("  _removeWatcher: %@, %@, key not watched"), sessionNum, watchedKey);
#endif	/* DEBUG */
		return;
	}
	newDict = CFDictionaryCreateMutableCopy(NULL, 0, dict);

	/*
	 * Get the set of watchers out of the keys dictionary and
	 * remove this session from the list.
	 */
	watchers       = CFDictionaryGetValue(newDict, kSCDWatchers);
	newWatchers    = CFArrayCreateMutableCopy(NULL, 0, watchers);

	watcherRefs    = CFDictionaryGetValue(newDict, kSCDWatcherRefs);
	newWatcherRefs = CFArrayCreateMutableCopy(NULL, 0, watcherRefs);

	/* locate the session reference */
	i = CFArrayGetFirstIndexOfValue(newWatchers,
					CFRangeMake(0, CFArrayGetCount(newWatchers)),
					sessionNum);
	if (i == kCFNotFound) {
#ifdef	DEBUG
		SCLog(_configd_verbose, LOG_DEBUG, CFSTR("  _removeWatcher: %@, %@, session not watching"), sessionNum, watchedKey);
#endif	/* DEBUG */
		CFRelease(newDict);
		CFRelease(newWatchers);
		CFRelease(newWatcherRefs);
		return;
	}

	/* remove one session reference */
	refNum = CFArrayGetValueAtIndex(newWatcherRefs, i);
	CFNumberGetValue(refNum, kCFNumberIntType, &refCnt);
	if (--refCnt > 0) {
		refNum = CFNumberCreate(NULL, kCFNumberIntType, &refCnt);
		CFArraySetValueAtIndex(newWatcherRefs, i, refNum);
		CFRelease(refNum);
	} else {
		/* if this was the last reference */
		CFArrayRemoveValueAtIndex(newWatchers, i);
		CFArrayRemoveValueAtIndex(newWatcherRefs, i);
	}

	if (CFArrayGetCount(newWatchers) > 0) {
		/* if this key is still being "watched" */
		CFDictionarySetValue(newDict, kSCDWatchers, newWatchers);
		CFDictionarySetValue(newDict, kSCDWatcherRefs, newWatcherRefs);
	} else {
		/* no watchers left, remove the empty set */
		CFDictionaryRemoveValue(newDict, kSCDWatchers);
		CFDictionaryRemoveValue(newDict, kSCDWatcherRefs);
	}
	CFRelease(newWatchers);
	CFRelease(newWatcherRefs);

	if (CFDictionaryGetCount(newDict) > 0) {
		/* if this key is still active */
		CFDictionarySetValue(storeData, watchedKey, newDict);
	} else {
		/* no information left, remove the empty dictionary */
		CFDictionaryRemoveValue(storeData, watchedKey);
	}
	CFRelease(newDict);

#ifdef	DEBUG
	SCLog(_configd_verbose, LOG_DEBUG, CFSTR("  _removeWatcher: %@, %@"), sessionNum, watchedKey);
#endif	/* DEBUG */

	return;
}


__private_extern__
void
pushNotifications()
{
	const void			**sessionsToNotify;
	CFIndex				notifyCnt;
	int				server;
	serverSessionRef		theSession;
	SCDynamicStorePrivateRef	storePrivate;

	if (needsNotification == NULL)
		return;		/* if no sessions need to be kicked */

	notifyCnt = CFSetGetCount(needsNotification);
	sessionsToNotify = malloc(notifyCnt * sizeof(CFNumberRef));
	CFSetGetValues(needsNotification, sessionsToNotify);
	while (--notifyCnt >= 0) {
		(void) CFNumberGetValue(sessionsToNotify[notifyCnt],
					kCFNumberIntType,
					&server);
		theSession = getSession(server);
		storePrivate = (SCDynamicStorePrivateRef)theSession->store;

		/*
		 * deliver notifications to client sessions
		 */
		if ((storePrivate->notifyStatus == Using_NotifierInformViaMachPort) &&
		    (storePrivate->notifyPort != MACH_PORT_NULL)) {
			/*
			 * Post notification as mach message
			 */
#ifdef	DEBUG
			if (_configd_verbose) {
				SCLog(TRUE, LOG_DEBUG, CFSTR("sending mach message notification."));
				SCLog(TRUE, LOG_DEBUG, CFSTR("  port  = %d"), storePrivate->notifyPort);
				SCLog(TRUE, LOG_DEBUG, CFSTR("  msgid = %d"), storePrivate->notifyPortIdentifier);
			}
#endif	/* DEBUG */
			_SC_sendMachMessage(storePrivate->notifyPort, storePrivate->notifyPortIdentifier);
		}

		if ((storePrivate->notifyStatus == Using_NotifierInformViaFD) &&
		    (storePrivate->notifyFile >= 0)) {
			ssize_t		written;

#ifdef	DEBUG
			if (_configd_verbose) {
				SCLog(TRUE, LOG_DEBUG, CFSTR("sending (UNIX domain) socket notification"));
				SCLog(TRUE, LOG_DEBUG, CFSTR("  fd    = %d"), storePrivate->notifyFile);
				SCLog(TRUE, LOG_DEBUG, CFSTR("  msgid = %d"), storePrivate->notifyFileIdentifier);
			}
#endif	/* DEBUG */

			written = write(storePrivate->notifyFile,
					&storePrivate->notifyFileIdentifier,
					sizeof(storePrivate->notifyFileIdentifier));
			if (written == -1) {
				if (errno == EWOULDBLOCK) {
#ifdef	DEBUG
					SCLog(_configd_verbose, LOG_DEBUG,
					      CFSTR("sorry, only one outstanding notification per session."));
#endif	/* DEBUG */
				} else {
#ifdef	DEBUG
					SCLog(_configd_verbose, LOG_DEBUG,
					      CFSTR("could not send notification, write() failed: %s"),
					      strerror(errno));
#endif	/* DEBUG */
					storePrivate->notifyFile = -1;
				}
			} else if (written != sizeof(storePrivate->notifyFileIdentifier)) {
#ifdef	DEBUG
				SCLog(_configd_verbose, LOG_DEBUG,
				      CFSTR("could not send notification, incomplete write()"));
#endif	/* DEBUG */
				storePrivate->notifyFile = -1;
			}
		}

		if ((storePrivate->notifyStatus == Using_NotifierInformViaSignal) &&
		    (storePrivate->notifySignal > 0)) {
			kern_return_t	status;
			pid_t		pid;
			/*
			 * Post notification as signal
			 */
			status = pid_for_task(storePrivate->notifySignalTask, &pid);
			if (status == KERN_SUCCESS) {
#ifdef	DEBUG
				if (_configd_verbose) {
					SCLog(TRUE, LOG_DEBUG, CFSTR("sending signal notification"));
					SCLog(TRUE, LOG_DEBUG, CFSTR("  pid    = %d"), pid);
					SCLog(TRUE, LOG_DEBUG, CFSTR("  signal = %d"), storePrivate->notifySignal);
				}
#endif	/* DEBUG */
				if (kill(pid, storePrivate->notifySignal) != 0) {
#ifdef	DEBUG
					SCLog(_configd_verbose, LOG_DEBUG, CFSTR("could not send signal: %s"), strerror(errno));
#endif	/* DEBUG */
					status = KERN_FAILURE;
				}
			} else {
				mach_port_type_t	pt;

				if ((mach_port_type(mach_task_self(), storePrivate->notifySignalTask, &pt) == KERN_SUCCESS) &&
				    (pt & MACH_PORT_TYPE_DEAD_NAME)) {
					SCLog(_configd_verbose, LOG_DEBUG, CFSTR("could not send signal, process died"));
				} else {
					SCLog(_configd_verbose, LOG_DEBUG, CFSTR("could not send signal: %s"), mach_error_string(status));
				}
			}

			if (status != KERN_SUCCESS) {
				/* don't bother with any more attempts */
				(void) mach_port_destroy(mach_task_self(), storePrivate->notifySignalTask);
				storePrivate->notifySignal     = 0;
				storePrivate->notifySignalTask = TASK_NULL;
			}
	       }
	}
	free(sessionsToNotify);

	/*
	 * this list of notifications have been posted, wait for some more.
	 */
	CFRelease(needsNotification);
	needsNotification = NULL;

	return;
}
