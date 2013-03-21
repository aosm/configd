/*
 * Copyright (c) 2000-2009 Apple Inc. All rights reserved.
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
 * November 9, 2000		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>
#include <SystemConfiguration/SCPrivate.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <mach/mach.h>
#include <mach/notify.h>
#include <mach/mach_error.h>
#include <pthread.h>

#include <execinfo.h>
#include <libproc.h>
#include <unistd.h>

#define	N_QUICK	32


#pragma mark -
#pragma mark Miscellaneous


char *
_SC_cfstring_to_cstring(CFStringRef cfstr, char *buf, CFIndex bufLen, CFStringEncoding encoding)
{
	CFIndex	converted;
	CFIndex	last	= 0;
	CFIndex	len;

	if (cfstr == NULL) {
		cfstr = CFSTR("");
	}
	len = CFStringGetLength(cfstr);

	/* how much buffer space will we really need? */
	converted = CFStringGetBytes(cfstr,
				     CFRangeMake(0, len),
				     encoding,
				     0,
				     FALSE,
				     NULL,
				     0,
				     &last);
	if (converted < len) {
		/* if full string could not be converted */
		if (buf != NULL) {
			buf[0] = '\0';
		}
		return NULL;
	}

	if (buf != NULL) {
		if (bufLen < (last + 1)) {
			/* if the size of the provided buffer is too small */
			buf[0] = '\0';
			return NULL;
		}
	} else {
		/* allocate a buffer */
		bufLen = last + 1;
		buf = CFAllocatorAllocate(NULL, bufLen, 0);
		if (buf == NULL) {
			return NULL;
		}
	}

	(void)CFStringGetBytes(cfstr,
			       CFRangeMake(0, len),
			       encoding,
			       0,
			       FALSE,
			       (UInt8 *)buf,
			       bufLen,
			       &last);
	buf[last] = '\0';

	return buf;
}


void
_SC_sockaddr_to_string(const struct sockaddr *address, char *buf, size_t bufLen)
{
	bzero(buf, bufLen);
	switch (address->sa_family) {
		case AF_INET :
			(void)inet_ntop(((struct sockaddr_in *)address)->sin_family,
					&((struct sockaddr_in *)address)->sin_addr,
					buf,
					bufLen);
			break;
		case AF_INET6 : {
			(void)inet_ntop(((struct sockaddr_in6 *)address)->sin6_family,
					&((struct sockaddr_in6 *)address)->sin6_addr,
					buf,
					bufLen);
			if (((struct sockaddr_in6 *)address)->sin6_scope_id != 0) {
				int	n;

				n = strlen(buf);
				if ((n+IF_NAMESIZE+1) <= (int)bufLen) {
					buf[n++] = '%';
					if_indextoname(((struct sockaddr_in6 *)address)->sin6_scope_id, &buf[n]);
				}
			}
			break;
		}
		case AF_LINK :
			if (((struct sockaddr_dl *)address)->sdl_len < bufLen) {
				bufLen = ((struct sockaddr_dl *)address)->sdl_len;
			} else {
				bufLen = bufLen - 1;
			}

			bcopy(((struct sockaddr_dl *)address)->sdl_data, buf, bufLen);
			break;
		default :
			snprintf(buf, bufLen, "unexpected address family %d", address->sa_family);
			break;
	}

	return;
}


void
_SC_sendMachMessage(mach_port_t port, mach_msg_id_t msg_id)
{
	mach_msg_empty_send_t	msg;
	mach_msg_option_t	options;
	kern_return_t		status;

	msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.header.msgh_size = sizeof(msg);
	msg.header.msgh_remote_port = port;
	msg.header.msgh_local_port = MACH_PORT_NULL;
	msg.header.msgh_id = msg_id;
	options = MACH_SEND_TIMEOUT;
	status = mach_msg(&msg.header,			/* msg */
			  MACH_SEND_MSG|options,	/* options */
			  msg.header.msgh_size,		/* send_size */
			  0,				/* rcv_size */
			  MACH_PORT_NULL,		/* rcv_name */
			  0,				/* timeout */
			  MACH_PORT_NULL);		/* notify */
	if (status == MACH_SEND_TIMED_OUT) {
		mach_msg_destroy(&msg.header);
	}

	return;
}


#pragma mark -
#pragma mark Serialization


Boolean
_SCSerialize(CFPropertyListRef obj, CFDataRef *xml, void **dataRef, CFIndex *dataLen)
{
	CFDataRef		myXml;
	CFWriteStreamRef	stream;

	if ((xml == NULL) && ((dataRef == NULL) || (dataLen == NULL))) {
		/* if not keeping track of allocated space */
		return FALSE;
	}

	stream = CFWriteStreamCreateWithAllocatedBuffers(NULL, NULL);
	CFWriteStreamOpen(stream);
	CFPropertyListWriteToStream(obj, stream, kCFPropertyListBinaryFormat_v1_0, NULL);
	CFWriteStreamClose(stream);
	myXml = CFWriteStreamCopyProperty(stream, kCFStreamPropertyDataWritten);
	CFRelease(stream);
	if (myXml == NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("_SCSerialize() failed"));
		if (xml != NULL) {
			*xml = NULL;
		}
		if ((dataRef != NULL) && (dataLen != NULL)) {
			*dataLen = 0;
			*dataRef = NULL;
		}
		return FALSE;
	}

	if (xml != NULL) {
		*xml = myXml;
		if ((dataRef != NULL) && (dataLen != NULL)) {
			*dataRef = (void *)CFDataGetBytePtr(myXml);
			*dataLen = CFDataGetLength(myXml);
		}
	} else {
		mach_msg_type_number_t	len;
		kern_return_t		status;

		status = vm_read(mach_task_self(),
				 (vm_address_t)CFDataGetBytePtr(myXml),	// address
				 (vm_size_t)   CFDataGetLength(myXml),	// size
				 (void *)dataRef,
				 &len);
		if (status != KERN_SUCCESS) {
			SCLog(TRUE, LOG_ERR, CFSTR("_SCSerialize(): %s"), mach_error_string(status));
			CFRelease(myXml);
			*dataRef = NULL;
			*dataLen = 0;
			return FALSE;
		}

		*dataLen = len;
		CFRelease(myXml);
	}

	return TRUE;
}


Boolean
_SCUnserialize(CFPropertyListRef *obj, CFDataRef xml, void *dataRef, CFIndex dataLen)
{
	CFStringRef	xmlError;

	if (xml == NULL) {
		kern_return_t	status;

		xml = CFDataCreateWithBytesNoCopy(NULL, (void *)dataRef, dataLen, kCFAllocatorNull);
		*obj = CFPropertyListCreateFromXMLData(NULL,
						       xml,
						       kCFPropertyListImmutable,
						       &xmlError);
		CFRelease(xml);

		status = vm_deallocate(mach_task_self(), (vm_address_t)dataRef, dataLen);
		if (status != KERN_SUCCESS) {
			SCLog(_sc_verbose, LOG_DEBUG, CFSTR("_SCUnserialize(): %s"), mach_error_string(status));
			/* non-fatal???, proceed */
		}
	} else {
		*obj = CFPropertyListCreateFromXMLData(NULL,
						       xml,
						       kCFPropertyListImmutable,
						       &xmlError);
	}

	if (*obj == NULL) {
		if (xmlError != NULL) {
			SCLog(TRUE, LOG_ERR, CFSTR("_SCUnserialize(): %@"), xmlError);
			CFRelease(xmlError);
		}
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	return TRUE;
}


Boolean
_SCSerializeString(CFStringRef str, CFDataRef *data, void **dataRef, CFIndex *dataLen)
{
	CFDataRef	myData;

	if (!isA_CFString(str)) {
		/* if not a CFString */
		return FALSE;
	}

	if ((data == NULL) && ((dataRef == NULL) || (dataLen == NULL))) {
		/* if not keeping track of allocated space */
		return FALSE;
	}

	myData = CFStringCreateExternalRepresentation(NULL, str, kCFStringEncodingUTF8, 0);
	if (myData == NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("_SCSerializeString() failed"));
		if (data != NULL) {
			*data = NULL;
		}
		if ((dataRef != NULL) && (dataLen != NULL)) {
			*dataRef = NULL;
			*dataLen = 0;
		}
		return FALSE;
	}

	if (data != NULL) {
		*data = myData;
		if ((dataRef != NULL) && (dataLen != NULL)) {
			*dataRef = (void *)CFDataGetBytePtr(myData);
			*dataLen = CFDataGetLength(myData);
		}
	} else {
		mach_msg_type_number_t	len;
		kern_return_t		status;

		*dataLen = CFDataGetLength(myData);
		status = vm_read(mach_task_self(),
				 (vm_address_t)CFDataGetBytePtr(myData),	// address
				 *dataLen,					// size
				 (void *)dataRef,
				 &len);
		if (status != KERN_SUCCESS) {
			SCLog(TRUE, LOG_ERR, CFSTR("_SCSerializeString(): %s"), mach_error_string(status));
			CFRelease(myData);
			*dataRef = NULL;
			*dataLen = 0;
			return FALSE;
		}

		*dataLen = len;
		CFRelease(myData);
	}

	return TRUE;
}


Boolean
_SCUnserializeString(CFStringRef *str, CFDataRef utf8, void *dataRef, CFIndex dataLen)
{
	if (utf8 == NULL) {
		kern_return_t	status;

		utf8 = CFDataCreateWithBytesNoCopy(NULL, dataRef, dataLen, kCFAllocatorNull);
		*str = CFStringCreateFromExternalRepresentation(NULL, utf8, kCFStringEncodingUTF8);
		CFRelease(utf8);

		status = vm_deallocate(mach_task_self(), (vm_address_t)dataRef, dataLen);
		if (status != KERN_SUCCESS) {
			SCLog(_sc_verbose, LOG_DEBUG, CFSTR("_SCUnserializeString(): %s"), mach_error_string(status));
			/* non-fatal???, proceed */
		}
	} else {
		*str = CFStringCreateFromExternalRepresentation(NULL, utf8, kCFStringEncodingUTF8);
	}

	if (*str == NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("_SCUnserializeString() failed"));
		return FALSE;
	}

	return TRUE;
}


Boolean
_SCSerializeData(CFDataRef data, void **dataRef, CFIndex *dataLen)
{
	mach_msg_type_number_t	len;
	kern_return_t		status;

	if (!isA_CFData(data)) {
		/* if not a CFData */
		return FALSE;
	}

	*dataLen = CFDataGetLength(data);
	status = vm_read(mach_task_self(),
			 (vm_address_t)CFDataGetBytePtr(data),	// address
			 *dataLen,				// size
			 (void *)dataRef,
			 &len);
	if (status != KERN_SUCCESS) {
		SCLog(TRUE, LOG_ERR, CFSTR("_SCSerializeData(): %s"), mach_error_string(status));
		*dataRef = NULL;
		*dataLen = 0;
		return FALSE;
	}

	*dataLen = len;

	return TRUE;
}


Boolean
_SCUnserializeData(CFDataRef *data, void *dataRef, CFIndex dataLen)
{
	kern_return_t		status;

	*data = CFDataCreate(NULL, dataRef, dataLen);
	status = vm_deallocate(mach_task_self(), (vm_address_t)dataRef, dataLen);
	if (status != KERN_SUCCESS) {
		SCLog(_sc_verbose, LOG_DEBUG, CFSTR("_SCUnserializeData(): %s"), mach_error_string(status));
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	return TRUE;
}


CFDictionaryRef
_SCSerializeMultiple(CFDictionaryRef dict)
{
	const void *		keys_q[N_QUICK];
	const void **		keys		= keys_q;
	CFIndex			nElements;
	CFDictionaryRef		newDict		= NULL;
	const void *		pLists_q[N_QUICK];
	const void **		pLists		= pLists_q;
	const void *		values_q[N_QUICK];
	const void **		values		= values_q;

	nElements = CFDictionaryGetCount(dict);
	if (nElements > 0) {
		CFIndex	i;

		if (nElements > (CFIndex)(sizeof(keys_q) / sizeof(CFTypeRef))) {
			keys   = CFAllocatorAllocate(NULL, nElements * sizeof(CFTypeRef), 0);
			values = CFAllocatorAllocate(NULL, nElements * sizeof(CFTypeRef), 0);
			pLists = CFAllocatorAllocate(NULL, nElements * sizeof(CFTypeRef), 0);
		}
		bzero(pLists, nElements * sizeof(CFTypeRef));

		CFDictionaryGetKeysAndValues(dict, keys, values);
		for (i = 0; i < nElements; i++) {
			if (!_SCSerialize((CFPropertyListRef)values[i], (CFDataRef *)&pLists[i], NULL, NULL)) {
				goto done;
			}
		}
	}

	newDict = CFDictionaryCreate(NULL,
				     keys,
				     pLists,
				     nElements,
				     &kCFTypeDictionaryKeyCallBacks,
				     &kCFTypeDictionaryValueCallBacks);

    done :

	if (nElements > 0) {
		CFIndex	i;

		for (i = 0; i < nElements; i++) {
			if (pLists[i])	CFRelease(pLists[i]);
		}

		if (keys != keys_q) {
			CFAllocatorDeallocate(NULL, keys);
			CFAllocatorDeallocate(NULL, values);
			CFAllocatorDeallocate(NULL, pLists);
		}
	}

	return newDict;
}


CFDictionaryRef
_SCUnserializeMultiple(CFDictionaryRef dict)
{
	const void *		keys_q[N_QUICK];
	const void **		keys		= keys_q;
	CFIndex			nElements;
	CFDictionaryRef		newDict		= NULL;
	const void *		pLists_q[N_QUICK];
	const void **		pLists		= pLists_q;
	const void *		values_q[N_QUICK];
	const void **		values		= values_q;

	nElements = CFDictionaryGetCount(dict);
	if (nElements > 0) {
		CFIndex	i;

		if (nElements > (CFIndex)(sizeof(keys_q) / sizeof(CFTypeRef))) {
			keys   = CFAllocatorAllocate(NULL, nElements * sizeof(CFTypeRef), 0);
			values = CFAllocatorAllocate(NULL, nElements * sizeof(CFTypeRef), 0);
			pLists = CFAllocatorAllocate(NULL, nElements * sizeof(CFTypeRef), 0);
		}
		bzero(pLists, nElements * sizeof(CFTypeRef));

		CFDictionaryGetKeysAndValues(dict, keys, values);
		for (i = 0; i < nElements; i++) {
			if (!_SCUnserialize((CFPropertyListRef *)&pLists[i], values[i], NULL, 0)) {
				goto done;
			}
		}
	}

	newDict = CFDictionaryCreate(NULL,
				     keys,
				     pLists,
				     nElements,
				     &kCFTypeDictionaryKeyCallBacks,
				     &kCFTypeDictionaryValueCallBacks);

    done :

	if (nElements > 0) {
		CFIndex	i;

		for (i = 0; i < nElements; i++) {
			if (pLists[i])	CFRelease(pLists[i]);
		}

		if (keys != keys_q) {
			CFAllocatorDeallocate(NULL, keys);
			CFAllocatorDeallocate(NULL, values);
			CFAllocatorDeallocate(NULL, pLists);
		}
	}

	return newDict;
}


#pragma mark -
#pragma mark CFRunLoop scheduling


__private_extern__ void
_SC_signalRunLoop(CFTypeRef obj, CFRunLoopSourceRef rls, CFArrayRef rlList)
{
	CFRunLoopRef	rl	= NULL;
	CFRunLoopRef	rl1	= NULL;
	CFIndex		i;
	CFIndex		n	= CFArrayGetCount(rlList);

	if (n == 0) {
		return;
	}

	/* get first runLoop for this object */
	for (i = 0; i < n; i += 3) {
		if (!CFEqual(obj, CFArrayGetValueAtIndex(rlList, i))) {
			continue;
		}

		rl1 = (CFRunLoopRef)CFArrayGetValueAtIndex(rlList, i+1);
		break;
	}

	if (rl1 == NULL) {
		/* if not scheduled */
		return;
	}

	/* check if we have another runLoop for this object */
	rl = rl1;
	for (i = i+3; i < n; i += 3) {
		CFRunLoopRef	rl2;

		if (!CFEqual(obj, CFArrayGetValueAtIndex(rlList, i))) {
			continue;
		}

		rl2 = (CFRunLoopRef)CFArrayGetValueAtIndex(rlList, i+1);
		if (!CFEqual(rl1, rl2)) {
			/* we've got more than one runLoop */
			rl = NULL;
			break;
		}
	}

	if (rl != NULL) {
		/* if we only have one runLoop */
		CFRunLoopWakeUp(rl);
		return;
	}

	/* more than one different runLoop, so we must pick one */
	for (i = 0; i < n; i+=3) {
		CFStringRef	rlMode;

		if (!CFEqual(obj, CFArrayGetValueAtIndex(rlList, i))) {
			continue;
		}

		rl     = (CFRunLoopRef)CFArrayGetValueAtIndex(rlList, i+1);
		rlMode = CFRunLoopCopyCurrentMode(rl);
		if (rlMode != NULL) {
			Boolean	waiting;

			waiting = (CFRunLoopIsWaiting(rl) && CFRunLoopContainsSource(rl, rls, rlMode));
			CFRelease(rlMode);
			if (waiting) {
				/* we've found a runLoop that's "ready" */
				CFRunLoopWakeUp(rl);
				return;
			}
		}
	}

	/* didn't choose one above, so choose first */
	CFRunLoopWakeUp(rl1);
	return;
}


__private_extern__ Boolean
_SC_isScheduled(CFTypeRef obj, CFRunLoopRef runLoop, CFStringRef runLoopMode, CFMutableArrayRef rlList)
{
	CFIndex	i;
	CFIndex	n	= CFArrayGetCount(rlList);

	for (i = 0; i < n; i += 3) {
		if ((obj != NULL)         && !CFEqual(obj,         CFArrayGetValueAtIndex(rlList, i))) {
			continue;
		}
		if ((runLoop != NULL)     && !CFEqual(runLoop,     CFArrayGetValueAtIndex(rlList, i+1))) {
			continue;
		}
		if ((runLoopMode != NULL) && !CFEqual(runLoopMode, CFArrayGetValueAtIndex(rlList, i+2))) {
			continue;
		}
		return TRUE;
	}

	return FALSE;
}


__private_extern__ void
_SC_schedule(CFTypeRef obj, CFRunLoopRef runLoop, CFStringRef runLoopMode, CFMutableArrayRef rlList)
{
	CFArrayAppendValue(rlList, obj);
	CFArrayAppendValue(rlList, runLoop);
	CFArrayAppendValue(rlList, runLoopMode);

	return;
}


__private_extern__ Boolean
_SC_unschedule(CFTypeRef obj, CFRunLoopRef runLoop, CFStringRef runLoopMode, CFMutableArrayRef rlList, Boolean all)
{
	CFIndex	i	= 0;
	Boolean	found	= FALSE;
	CFIndex	n	= CFArrayGetCount(rlList);

	while (i < n) {
		if ((obj != NULL)         && !CFEqual(obj,         CFArrayGetValueAtIndex(rlList, i))) {
			i += 3;
			continue;
		}
		if ((runLoop != NULL)     && !CFEqual(runLoop,     CFArrayGetValueAtIndex(rlList, i+1))) {
			i += 3;
			continue;
		}
		if ((runLoopMode != NULL) && !CFEqual(runLoopMode, CFArrayGetValueAtIndex(rlList, i+2))) {
			i += 3;
			continue;
		}

		found = TRUE;

		CFArrayRemoveValueAtIndex(rlList, i + 2);
		CFArrayRemoveValueAtIndex(rlList, i + 1);
		CFArrayRemoveValueAtIndex(rlList, i);

		if (!all) {
			return found;
		}

		n -= 3;
	}

	return found;
}


#pragma mark -
#pragma mark Bundle


#define SYSTEMCONFIGURATION_BUNDLE_ID		CFSTR("com.apple.SystemConfiguration")
#define	SYSTEMCONFIGURATION_FRAMEWORK_PATH	"/System/Library/Frameworks/SystemConfiguration.framework"
#define	SYSTEMCONFIGURATION_FRAMEWORK_PATH_LEN	(sizeof(SYSTEMCONFIGURATION_FRAMEWORK_PATH) - 1)

#define	SUFFIX_SYM				"~sym"
#define	SUFFIX_SYM_LEN				(sizeof(SUFFIX_SYM) - 1)

#define	SUFFIX_DST				"~dst"


CFBundleRef
_SC_CFBundleGet(void)
{
	static CFBundleRef	bundle	= NULL;
	char			*env;
	size_t			len;

	if (bundle != NULL) {
		return bundle;
	}

	bundle = CFBundleGetBundleWithIdentifier(SYSTEMCONFIGURATION_BUNDLE_ID);
	if (bundle != NULL) {
		CFRetain(bundle);	// we want to hold a reference to the bundle
		return bundle;
	}

	// if appropriate (e.g. when debugging), try a bit harder

	env = getenv("DYLD_FRAMEWORK_PATH");
	len = (env != NULL) ? strlen(env) : 0;

	// trim any trailing slashes
	while (len > 1) {
		if (env[len - 1] != '/') {
			break;
		}
		len--;
	}

	// if DYLD_FRAMEWORK_PATH is ".../xxx~sym" than try ".../xxx~dst"
	if ((len > SUFFIX_SYM_LEN) &&
	    (strncmp(&env[len - SUFFIX_SYM_LEN], SUFFIX_SYM, SUFFIX_SYM_LEN) == 0) &&
	    ((len + SYSTEMCONFIGURATION_FRAMEWORK_PATH_LEN) < MAXPATHLEN)) {
		char		path[MAXPATHLEN];
		CFURLRef	url;

		strlcpy(path, env, sizeof(path));
		strlcpy(&path[len - SUFFIX_SYM_LEN], SUFFIX_DST, sizeof(path) - (len - SUFFIX_SYM_LEN));
		strlcat(&path[len], SYSTEMCONFIGURATION_FRAMEWORK_PATH, sizeof(path) - len);

		url = CFURLCreateFromFileSystemRepresentation(NULL,
							      (UInt8 *)path,
							      len + SYSTEMCONFIGURATION_FRAMEWORK_PATH_LEN,
							      TRUE);
		bundle = CFBundleCreate(NULL, url);
		CFRelease(url);
	}

	if (bundle == NULL) {
		static	Boolean	warned	= FALSE;

		SCLog(!warned, LOG_WARNING,
		      CFSTR("_SC_CFBundleGet(), could not get CFBundle for \"%@\""),
		      SYSTEMCONFIGURATION_BUNDLE_ID);
		warned = TRUE;
	}

	return bundle;
}


CFStringRef
_SC_CFBundleCopyNonLocalizedString(CFBundleRef bundle, CFStringRef key, CFStringRef value, CFStringRef tableName)
{
	CFStringRef	str	= NULL;
	CFURLRef	url;

	if ((tableName == NULL) || CFEqual(tableName, CFSTR(""))) tableName = CFSTR("Localizable");

	url = CFBundleCopyResourceURLForLocalization(bundle,
						     tableName,
						     CFSTR("strings"),
						     NULL,
						     CFSTR("English"));
	if (url != NULL) {
		CFDataRef	data	= NULL;
		SInt32		errCode	= 0;

		if (CFURLCreateDataAndPropertiesFromResource(NULL,
							     url,
							     &data,
							     NULL,
							     NULL,
							     &errCode)) {
			CFDictionaryRef	table;

			table = (CFDictionaryRef)CFPropertyListCreateFromXMLData(NULL,
										 data,
										 kCFPropertyListImmutable,
										 NULL);
			if (table != NULL) {
				if (isA_CFDictionary(table)) {
					str = CFDictionaryGetValue(table, key);
					if (str != NULL) {
						CFRetain(str);
					}
				}

				CFRelease(table);
			}

			CFRelease(data);
		}

		CFRelease(url);
	}

	if (str == NULL) {
		str = CFRetain(value);
	}

	return str;
}


#pragma mark -
#pragma mark DOS encoding/codepage


#if	!TARGET_OS_IPHONE
void
_SC_dos_encoding_and_codepage(CFStringEncoding	macEncoding,
			      UInt32		macRegion,
			      CFStringEncoding	*dosEncoding,
			      UInt32		*dosCodepage)
{
	switch (macEncoding) {
	case kCFStringEncodingMacRoman:
		if (macRegion != 0) /* anything non-zero is not US */
		*dosEncoding = kCFStringEncodingDOSLatin1;
		else /* US region */
		*dosEncoding = kCFStringEncodingDOSLatinUS;
		break;

	case kCFStringEncodingMacJapanese:
		*dosEncoding = kCFStringEncodingDOSJapanese;
		break;

	case kCFStringEncodingMacChineseTrad:
		*dosEncoding = kCFStringEncodingDOSChineseTrad;
		break;

	case kCFStringEncodingMacKorean:
		*dosEncoding = kCFStringEncodingDOSKorean;
		break;

	case kCFStringEncodingMacArabic:
		*dosEncoding = kCFStringEncodingDOSArabic;
		break;

	case kCFStringEncodingMacHebrew:
		*dosEncoding = kCFStringEncodingDOSHebrew;
		break;

	case kCFStringEncodingMacGreek:
		*dosEncoding = kCFStringEncodingDOSGreek;
		break;

	case kCFStringEncodingMacCyrillic:
		*dosEncoding = kCFStringEncodingDOSCyrillic;
		break;

	case kCFStringEncodingMacThai:
		*dosEncoding = kCFStringEncodingDOSThai;
		break;

	case kCFStringEncodingMacChineseSimp:
		*dosEncoding = kCFStringEncodingDOSChineseSimplif;
		break;

	case kCFStringEncodingMacCentralEurRoman:
		*dosEncoding = kCFStringEncodingDOSLatin2;
		break;

	case kCFStringEncodingMacTurkish:
		*dosEncoding = kCFStringEncodingDOSTurkish;
		break;

	case kCFStringEncodingMacCroatian:
		*dosEncoding = kCFStringEncodingDOSLatin2;
		break;

	case kCFStringEncodingMacIcelandic:
		*dosEncoding = kCFStringEncodingDOSIcelandic;
		break;

	case kCFStringEncodingMacRomanian:
		*dosEncoding = kCFStringEncodingDOSLatin2;
		break;

	case kCFStringEncodingMacFarsi:
		*dosEncoding = kCFStringEncodingDOSArabic;
		break;

	case kCFStringEncodingMacUkrainian:
		*dosEncoding = kCFStringEncodingDOSCyrillic;
		break;

	default:
		*dosEncoding = kCFStringEncodingDOSLatin1;
		break;
	}

	*dosCodepage = CFStringConvertEncodingToWindowsCodepage(*dosEncoding);
	return;
}
#endif	// !TARGET_OS_IPHONE


#pragma mark -
#pragma mark Debugging


/*
 * print status of in-use mach ports
 */
void
_SC_logMachPortStatus(void)
{
	kern_return_t		status;
	mach_port_name_array_t	ports;
	mach_port_type_array_t	types;
	mach_msg_type_number_t	pi, pn, tn;
	CFMutableStringRef	str;

	SCLog(TRUE, LOG_DEBUG, CFSTR("----------"));

	/* report on ALL mach ports associated with this task */
	status = mach_port_names(mach_task_self(), &ports, &pn, &types, &tn);
	if (status == MACH_MSG_SUCCESS) {
		str = CFStringCreateMutable(NULL, 0);
		for (pi = 0; pi < pn; pi++) {
			char	rights[16], *rp = &rights[0];

			if (types[pi] != MACH_PORT_TYPE_NONE) {
				*rp++ = ' ';
				*rp++ = '(';
				if (types[pi] & MACH_PORT_TYPE_SEND)
					*rp++ = 'S';
				if (types[pi] & MACH_PORT_TYPE_RECEIVE)
					*rp++ = 'R';
				if (types[pi] & MACH_PORT_TYPE_SEND_ONCE)
					*rp++ = 'O';
				if (types[pi] & MACH_PORT_TYPE_PORT_SET)
					*rp++ = 'P';
				if (types[pi] & MACH_PORT_TYPE_DEAD_NAME)
					*rp++ = 'D';
				*rp++ = ')';
			}
			*rp = '\0';
			CFStringAppendFormat(str, NULL, CFSTR(" %d%s"), ports[pi], rights);
		}
		SCLog(TRUE, LOG_DEBUG, CFSTR("Task ports (n=%d):%@"), pn, str);
		CFRelease(str);
	}

	return;
}


void
_SC_logMachPortReferences(const char *str, mach_port_t port)
{
	const char		*blanks		= "                                                            ";
	char			buf[60];
	mach_port_type_t	pt;
	mach_port_status_t	recv_status	= { 0 };
	mach_port_urefs_t	refs_send	= 0;
	mach_port_urefs_t	refs_recv	= 0;
	mach_port_urefs_t	refs_once	= 0;
	mach_port_urefs_t	refs_pset	= 0;
	mach_port_urefs_t	refs_dead	= 0;
	kern_return_t		status;

	buf[0] = '\0';
	if (str != NULL) {
		static int	is_configd	= -1;

		if (is_configd == -1) {
			char	name[64]	= "";

			(void) proc_name(getpid(), name, sizeof(name));
			is_configd = (strncmp(name, "configd", sizeof(name)) == 0);
		}
		if (is_configd == 1) {
			// if "configd", add indication if this is the M[ain] or [P]lugin thread
			strlcpy(buf,
				(CFRunLoopGetMain() == CFRunLoopGetCurrent()) ? "M " : "P ",
				sizeof(buf));
		}

		// add provided string
		strlcat(buf, str, sizeof(buf));

		// fill
		strlcat(buf, blanks, sizeof(buf));
		if (strcmp(&buf[sizeof(buf) - 3], "  ") == 0) {
			buf[sizeof(buf) - 3] = ':';
		}
	}

	status = mach_port_type(mach_task_self(), port, &pt);
	if (status != KERN_SUCCESS) {
		SCLog(TRUE, LOG_DEBUG,
		      CFSTR("%smach_port_get_refs(..., %d, MACH_PORT_RIGHT_SEND): %s"),
		      buf,
		      port,
		      mach_error_string(status));
	}

	if ((pt & MACH_PORT_TYPE_SEND) != 0) {
		status = mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND,      &refs_send);
		if (status != KERN_SUCCESS) {
			SCLog(TRUE, LOG_DEBUG,
			      CFSTR("%smach_port_get_refs(..., %d, MACH_PORT_RIGHT_SEND): %s"),
			      buf,
			      port,
			      mach_error_string(status));
		}
	}

	if ((pt & MACH_PORT_TYPE_RECEIVE) != 0) {
		mach_msg_type_number_t	count;

		status = mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE,   &refs_recv);
		if (status != KERN_SUCCESS) {
			SCLog(TRUE, LOG_DEBUG,
			      CFSTR("%smach_port_get_refs(..., %d, MACH_PORT_RIGHT_RECEIVE): %s"),
			      buf,
			      port,
			      mach_error_string(status));
		}

		count = MACH_PORT_RECEIVE_STATUS_COUNT;
		status = mach_port_get_attributes(mach_task_self(),
					       port,
					       MACH_PORT_RECEIVE_STATUS,
					       (mach_port_info_t)&recv_status,
					       &count);
		if (status != KERN_SUCCESS) {
			SCLog(TRUE, LOG_DEBUG,
			      CFSTR("%mach_port_get_attributes(..., %d, MACH_PORT_RECEIVE_STATUS): %s"),
			      buf,
			      port,
			      mach_error_string(status));
		}
	}

	if ((pt & MACH_PORT_TYPE_SEND_ONCE) != 0) {
		status = mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND_ONCE, &refs_once);
		if (status != KERN_SUCCESS) {
			SCLog(TRUE, LOG_DEBUG,
			      CFSTR("%smach_port_get_refs(..., %d, MACH_PORT_RIGHT_SEND_ONCE): %s"),
			      buf,
			      port,
			      mach_error_string(status));
		}
	}

	if ((pt & MACH_PORT_TYPE_PORT_SET) != 0) {
		status = mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_PORT_SET,  &refs_pset);
		if (status != KERN_SUCCESS) {
			SCLog(TRUE, LOG_DEBUG,
			      CFSTR("%smach_port_get_refs(..., %d, MACH_PORT_RIGHT_PORT_SET): %s"),
			      buf,
			      port,
			      mach_error_string(status));
		}
	}

	if ((pt & MACH_PORT_TYPE_DEAD_NAME) != 0) {
		status = mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_DEAD_NAME, &refs_dead);
		if (status != KERN_SUCCESS) {
			SCLog(TRUE, LOG_DEBUG,
			      CFSTR("%smach_port_get_refs(..., %d, MACH_PORT_RIGHT_DEAD_NAME): %s"),
			      buf,
			      port,
			      mach_error_string(status));
		}
	}

	SCLog(TRUE, LOG_DEBUG,
	      CFSTR("%smach port 0x%x (%d): send=%d, receive=%d, send once=%d, port set=%d, dead name=%d%s%s"),
	      buf,
	      port,
	      port,
	      refs_send,
	      refs_recv,
	      refs_once,
	      refs_pset,
	      refs_dead,
	      recv_status.mps_nsrequest ? ", no more senders"   : "",
	      ((pt & MACH_PORT_TYPE_DEAD_NAME) != 0) ? ", dead name request" : "");

	return;
}


CFStringRef
_SC_copyBacktrace()
{
	int			n;
	void			*stack[64];
	char			**symbols;
	CFMutableStringRef	trace;

	n = backtrace(stack, sizeof(stack)/sizeof(stack[0]));
	if (n == -1) {
		SCLog(TRUE, LOG_ERR, CFSTR("backtrace() failed: %s"), strerror(errno));
		return NULL;
	}

	trace = CFStringCreateMutable(NULL, 0);

	symbols = backtrace_symbols(stack, n);
	if (symbols != NULL) {
		int	i;

		for (i = 0; i < n; i++) {
			CFStringAppendFormat(trace, NULL, CFSTR("%s\n"), symbols[i]);
		}

		free(symbols);
	}

	return trace;
}
