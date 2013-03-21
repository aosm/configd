/*
 * Copyright (c) 2003-2012 Apple Inc. All rights reserved.
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
 * April 12, 2011		Allan Nathanson <ajn@apple.com>
 * - add SCNetworkReachability "server"
 *
 * March 31, 2004		Allan Nathanson <ajn@apple.com>
 * - use [SC] DNS configuration information
 *
 * January 19, 2003		Allan Nathanson <ajn@apple.com>
 * - add advanced reachability APIs
 */

#include <Availability.h>
#include <TargetConditionals.h>
#include <sys/cdefs.h>
#include <dispatch/dispatch.h>
#include <dispatch/private.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRuntime.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>
#include <SystemConfiguration/SCPrivate.h>
#include <pthread.h>
#include <libkern/OSAtomic.h>

#if	!TARGET_OS_IPHONE
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#endif	// !TARGET_OS_IPHONE

#include <notify.h>
#include <dnsinfo.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netdb_async.h>
#include <resolv.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#define	KERNEL_PRIVATE
#include <net/route.h>
#undef	KERNEL_PRIVATE

#ifndef s6_addr16
#define s6_addr16 __u6_addr.__u6_addr16
#endif

#include "SCNetworkReachabilityInternal.h"

#include <ppp/ppp_msg.h>

#if	!TARGET_IPHONE_SIMULATOR
#include <ppp/PPPControllerPriv.h>
#endif	// !TARGET_IPHONE_SIMULATOR



#if	((__MAC_OS_X_VERSION_MIN_REQUIRED >= 1070) || (__IPHONE_OS_VERSION_MIN_REQUIRED >= 40000)) && !TARGET_IPHONE_SIMULATOR
#define	HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL
#endif	// ((__MAC_OS_X_VERSION_MIN_REQUIRED >= 1070) || (__IPHONE_OS_VERSION_MIN_REQUIRED >= 40000)) && !TARGET_IPHONE_SIMULATOR

#if	((__MAC_OS_X_VERSION_MIN_REQUIRED >= 1070) || (__IPHONE_OS_VERSION_MIN_REQUIRED >= 40000)) && !TARGET_IPHONE_SIMULATOR && !TARGET_OS_EMBEDDED_OTHER
#define	HAVE_IPSEC_STATUS
#define	HAVE_VPN_STATUS
#endif	// ((__MAC_OS_X_VERSION_MIN_REQUIRED >= 1070) || (__IPHONE_OS_VERSION_MIN_REQUIRED >= 40000)) && !TARGET_IPHONE_SIMULATOR && !TARGET_OS_EMBEDDED_OTHER




#define	DEBUG_REACHABILITY_TYPE_NAME			"create w/name"
#define	DEBUG_REACHABILITY_TYPE_NAME_OPTIONS		"    + options"

#define	DEBUG_REACHABILITY_TYPE_ADDRESS		"create w/address"
#define	DEBUG_REACHABILITY_TYPE_ADDRESS_OPTIONS	"       + options"

#define	DEBUG_REACHABILITY_TYPE_ADDRESSPAIR		"create w/address pair"
#define	DEBUG_REACHABILITY_TYPE_ADDRESSPAIR_OPTIONS	"            + options"


static pthread_mutexattr_t	lock_attr;

#define MUTEX_INIT(m) {							\
	int _lock_ = (pthread_mutex_init(m, &lock_attr) == 0);		\
	assert(_lock_);							\
}

#define	MUTEX_LOCK(m) {							\
	int _lock_ = (pthread_mutex_lock(m) == 0);			\
	assert(_lock_);							\
}

#define	MUTEX_UNLOCK(m) {						\
	int _unlock_ = (pthread_mutex_unlock(m) == 0);			\
	assert(_unlock_);						\
}

#define	MUTEX_ASSERT_HELD(m) {						\
	int _locked_ = (pthread_mutex_lock(m) == EDEADLK);		\
	assert(_locked_);						\
}


#ifdef	HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL
/* Libinfo SPI */
mach_port_t
_getaddrinfo_interface_async_call(const char			*nodename,
				  const char			*servname,
				  const struct addrinfo		*hints,
				  const char			*interface,
				  getaddrinfo_async_callback	callback,
				  void				*context);
#endif	/* HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL */


#define SCNETWORKREACHABILITY_TRIGGER_KEY	CFSTR("com.apple.SCNetworkReachability:FORCE-CHANGE")


// how long (minimum time, us) to wait before retrying DNS query after EAI_NONAME
#define EAI_NONAME_RETRY_DELAY_USEC	250000			// 250ms

// how long (maximum time, us) after DNS configuration change we accept EAI_NONAME
// without question.
#define EAI_NONAME_RETRY_LIMIT_USEC	2500000			// 2.5s


// how long (maximum time, ns) to wait for a long-lived-query callback before
// we assume EAI_NONAME.
#define LLQ_TIMEOUT_NSEC		30 * NSEC_PER_SEC	// 30s


#define	N_QUICK	64


static CFStringRef	__SCNetworkReachabilityCopyDescription	(CFTypeRef cf);
static void		__SCNetworkReachabilityDeallocate	(CFTypeRef cf);
static void		reachPerform				(void *info);


static Boolean
__SCNetworkReachabilityScheduleWithRunLoop	(SCNetworkReachabilityRef	target,
						 CFRunLoopRef			runLoop,
						 CFStringRef			runLoopMode,
						 dispatch_queue_t		queue,
						 Boolean			onDemand);

static Boolean
__SCNetworkReachabilityUnscheduleFromRunLoop	(SCNetworkReachabilityRef	target,
						 CFRunLoopRef			runLoop,
						 CFStringRef			runLoopMode,
						 Boolean			onDemand);


static CFTypeID __kSCNetworkReachabilityTypeID	= _kCFRuntimeNotATypeID;


static const CFRuntimeClass __SCNetworkReachabilityClass = {
	0,					// version
	"SCNetworkReachability",		// className
	NULL,					// init
	NULL,					// copy
	__SCNetworkReachabilityDeallocate,	// dealloc
	NULL,					// equal
	NULL,					// hash
	NULL,					// copyFormattingDesc
	__SCNetworkReachabilityCopyDescription	// copyDebugDesc
};


static pthread_once_t		initialized	= PTHREAD_ONCE_INIT;
static const ReachabilityInfo	NOT_REACHABLE	= { 0, 0,		0, { 0 }, FALSE };
static const ReachabilityInfo	NOT_REPORTED	= { 0, 0xFFFFFFFF,	0, { 0 }, FALSE };
static int			rtm_seq		= 0;


static const struct addrinfo	HINTS_DEFAULT	= {
#ifdef	AI_PARALLEL
	.ai_flags	= AI_ADDRCONFIG | AI_PARALLEL,
#else	// AI_PARALLEL
	.ai_flags	= AI_ADDRCONFIG,
#endif	// AI_PARALLEL
};


static const struct timeval	TIME_ZERO	= { 0, 0 };


static Boolean			D_llqBypass	= FALSE;
static int			llqCount	= 0;
static DNSServiceRef		llqMain		= NULL;
static CFMutableSetRef		llqUpdated	= NULL;


#ifdef	HAVE_REACHABILITY_SERVER
static Boolean			D_serverBypass	= FALSE;
#endif	// HAVE_REACHABILITY_SERVER


#if	!TARGET_OS_IPHONE
/*
 * Power capabilities (sleep/wake)
 */
static IOPMSystemPowerStateCapabilities	power_capabilities	= kIOPMSytemPowerStateCapabilitiesMask;
#endif	// !TARGET_OS_IPHONE


/*
 * host "something has changed" notifications
 */

static SCDynamicStoreRef	hn_store	= NULL;
static dispatch_queue_t		hn_dispatchQueue = NULL;
static CFMutableSetRef		hn_targets	= NULL;


/*
 * DNS configuration
 */

typedef struct {
	dns_config_t	*config;
	int		refs;
} dns_configuration_t;


static pthread_mutex_t		dns_lock		= PTHREAD_MUTEX_INITIALIZER;
static dns_configuration_t	*dns_configuration	= NULL;
static int			dns_token;
static Boolean			dns_token_valid		= FALSE;


typedef enum {
	dns_query_sync,
	dns_query_async,
	dns_query_llq
} dns_query_type;


static void
__dns_query_start(struct timeval	*dnsQueryStart,
		  struct timeval	*dnsQueryEnd)
{
	(void) gettimeofday(dnsQueryStart, NULL);
	*dnsQueryEnd = TIME_ZERO;

	return;
}


static void
__dns_query_end(SCNetworkReachabilityRef	target,
		Boolean				found,
		dns_query_type			query_type,
		struct timeval			*dnsQueryStart,
		struct timeval			*dnsQueryEnd)
{
	struct timeval			dnsQueryElapsed;
	Boolean				firstQuery;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	// report initial or updated query time
	firstQuery = !timerisset(dnsQueryEnd);

	(void) gettimeofday(dnsQueryEnd, NULL);

	if (!_sc_debug) {
		return;
	}

	if (!timerisset(dnsQueryStart)) {
		return;
	}

	timersub(dnsQueryEnd, dnsQueryStart, &dnsQueryElapsed);
	switch (query_type) {

//		#define	QUERY_TIME__FMT	"%d.%3.3d"
//		#define	QUERY_TIME__DIV	1000

		#define	QUERY_TIME__FMT	"%d.%6.6d"
		#define	QUERY_TIME__DIV	1

		case dns_query_sync :
			SCLog(TRUE, LOG_INFO,
			      CFSTR("%ssync DNS complete%s (query time = " QUERY_TIME__FMT ")"),
			      targetPrivate->log_prefix,
			      found ? "" : ", host not found",
			      dnsQueryElapsed.tv_sec,
			      dnsQueryElapsed.tv_usec / QUERY_TIME__DIV);
			break;
		case dns_query_async :
			SCLog(TRUE, LOG_INFO,
			      CFSTR("%sasync DNS complete%s (query time = " QUERY_TIME__FMT ")"),
			      targetPrivate->log_prefix,
			      found ? "" : ", host not found",
			      dnsQueryElapsed.tv_sec,
			      dnsQueryElapsed.tv_usec / QUERY_TIME__DIV);
			break;
		case dns_query_llq :
			SCLog(TRUE, LOG_INFO,
			      CFSTR("%sDNS updated%s (%s = " QUERY_TIME__FMT ")"),
			      targetPrivate->log_prefix,
			      found ? "" : ", host not found",
			      firstQuery ? "query time" : "updated after",
			      dnsQueryElapsed.tv_sec,
			      dnsQueryElapsed.tv_usec / QUERY_TIME__DIV);
			break;
	}

	return;
}


static __inline__ Boolean
__reach_changed(ReachabilityInfo *r1, ReachabilityInfo *r2)
{
	if (r1->flags != r2->flags) {
		// if the reachability flags changed
		return TRUE;
	}

	if (r1->if_index != r2->if_index) {
		// if the target interface changed
		return TRUE;
	}

	if ((r1->sleeping != r2->sleeping) && !r2->sleeping) {
		// if our sleep/wake status changed and if we
		// are no longer sleeping
		return TRUE;
	}

	return FALSE;
}


static __inline__ void
_reach_set(ReachabilityInfo *dst, const ReachabilityInfo *src, uint64_t cycle)
{
	memcpy(dst, src, sizeof(ReachabilityInfo));
	dst->cycle = cycle;

	return;
}


#pragma mark -
#pragma mark SCDynamicStore info


typedef struct {
	SCDynamicStoreRef	store;
	CFStringRef		entity;
	CFDictionaryRef		dict;
	CFIndex			n;
	const void **		keys;
	const void *		keys_q[N_QUICK];
	const void **		values;
	const void *		values_q[N_QUICK];
} ReachabilityStoreInfo, *ReachabilityStoreInfoRef;


static ReachabilityStoreInfo	S_storeInfo		= { 0 };
static Boolean			S_storeInfoActive	= FALSE;


static dispatch_queue_t
_storeInfo_queue()
{
	static dispatch_once_t	once;
	static dispatch_queue_t	q;

	dispatch_once(&once, ^{
		q = dispatch_queue_create("SCNetworkReachabilty.storeInfo", NULL);
	});

	return q;
}


static void
ReachabilityStoreInfo_copy(ReachabilityStoreInfoRef	src,
			   ReachabilityStoreInfoRef	dst)
{
	if (src->dict != NULL) {
		dst->store = src->store;
		CFRetain(dst->store);

		dst->dict = src->dict;
		CFRetain(dst->dict);

		dst->n = src->n;
		if (dst->n > 0) {
			if (dst->n <= (CFIndex)(sizeof(dst->keys_q) / sizeof(CFTypeRef))) {
				dst->keys   = dst->keys_q;
				dst->values = dst->values_q;
			} else {
				dst->keys   = CFAllocatorAllocate(NULL, dst->n * sizeof(CFTypeRef), 0);
				dst->values = CFAllocatorAllocate(NULL, dst->n * sizeof(CFTypeRef), 0);
			}
			memcpy(dst->keys,   src->keys,   dst->n * sizeof(CFTypeRef));
			memcpy(dst->values, src->values, dst->n * sizeof(CFTypeRef));
		}
	}

	return;
}


static void
ReachabilityStoreInfo_enable(Boolean enable)
{
	dispatch_sync(_storeInfo_queue(), ^{
		S_storeInfoActive = enable;
	});

	return;
}


static void
ReachabilityStoreInfo_free(ReachabilityStoreInfoRef store_info)
{
	if ((store_info->n > 0) && (store_info->keys != store_info->keys_q)) {
		CFAllocatorDeallocate(NULL, store_info->keys);
		store_info->keys = NULL;

		CFAllocatorDeallocate(NULL, store_info->values);
		store_info->values = NULL;
	}
	store_info->n = 0;

	if (store_info->dict != NULL) {
		CFRelease(store_info->dict);
		store_info->dict = NULL;
	}

	if (store_info->store != NULL) {
		CFRelease(store_info->store);
		store_info->store = NULL;
	}

	return;
}


static void
ReachabilityStoreInfo_init(ReachabilityStoreInfoRef store_info)
{
	dispatch_sync(_storeInfo_queue(), ^{
		bzero(store_info, sizeof(ReachabilityStoreInfo));

		if (S_storeInfoActive && (S_storeInfo.dict != NULL)) {
			ReachabilityStoreInfo_copy(&S_storeInfo, store_info);
		}
	});

	return;
}


static void
ReachabilityStoreInfo_save(ReachabilityStoreInfoRef store_info)
{
	dispatch_sync(_storeInfo_queue(), ^{
		if ((store_info == NULL) ||
		    !_SC_CFEqual(store_info->dict, S_storeInfo.dict)) {
			// free any old info
			ReachabilityStoreInfo_free(&S_storeInfo);

			// save new info
			if (S_storeInfoActive &&
			    (store_info != NULL) &&
			    (store_info->dict != NULL)) {
				ReachabilityStoreInfo_copy(store_info, &S_storeInfo);
			}
		}
	});

	return;
}


static Boolean
ReachabilityStoreInfo_fill(ReachabilityStoreInfoRef store_info)
{
	CFStringRef		pattern;
	CFMutableArrayRef	patterns;

	patterns = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	// get info for IPv4 services
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetIPv4);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetIPv4);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

	// get info for IPv6 services
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetIPv6);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetIPv6);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

	// get info for PPP services
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetPPP);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetPPP);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

#if	!TARGET_IPHONE_SIMULATOR
	// get info for VPN services
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetVPN);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetVPN);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
#endif	// !TARGET_IPHONE_SIMULATOR

	// get info for IPSec services
//	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
//							      kSCDynamicStoreDomainSetup,
//							      kSCCompAnyRegex,
//							      kSCEntNetIPSec);
//	CFArrayAppendValue(patterns, pattern);
//	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetIPSec);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

	// get info to identify "available" services
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetInterface);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);


	// get the SCDynamicStore info
	store_info->dict = SCDynamicStoreCopyMultiple(store_info->store, NULL, patterns);
	CFRelease(patterns);
	if (store_info->dict == NULL) {
		return FALSE;
	}

	// and extract the keys/values for post-processing
	store_info->n = CFDictionaryGetCount(store_info->dict);
	if (store_info->n > 0) {
		if (store_info->n <= (CFIndex)(sizeof(store_info->keys_q) / sizeof(CFTypeRef))) {
			store_info->keys   = store_info->keys_q;
			store_info->values = store_info->values_q;
		} else {
			store_info->keys   = CFAllocatorAllocate(NULL, store_info->n * sizeof(CFTypeRef), 0);
			store_info->values = CFAllocatorAllocate(NULL, store_info->n * sizeof(CFTypeRef), 0);
		}
		CFDictionaryGetKeysAndValues(store_info->dict,
					     store_info->keys,
					     store_info->values);
	}

	return TRUE;
}


static Boolean
ReachabilityStoreInfo_update(ReachabilityStoreInfoRef	store_info,
			     SCDynamicStoreRef		*storeP,
			     sa_family_t		sa_family)
{
	__block Boolean		ok	= TRUE;

	switch (sa_family) {
		case AF_UNSPEC :
			store_info->entity = NULL;
			break;
		case AF_INET :
			store_info->entity = kSCEntNetIPv4;
			break;
		case AF_INET6 :
			store_info->entity = kSCEntNetIPv6;
			break;
		default :
			return FALSE;
	}

	if (store_info->dict != NULL) {
		// if info already available
		return TRUE;
	}

	dispatch_sync(_storeInfo_queue(), ^{
		if (S_storeInfoActive && (S_storeInfo.dict != NULL)) {
			// free any info
			ReachabilityStoreInfo_free(store_info);

			// copy the shared/available info
			ReachabilityStoreInfo_copy(&S_storeInfo, store_info);
		}

		if (store_info->store == NULL) {
			store_info->store = (storeP != NULL) ? *storeP : NULL;
			if (store_info->store != NULL) {
				// keep a reference to the passed in SCDynamicStore
				CFRetain(store_info->store);
			} else {
				store_info->store = SCDynamicStoreCreate(NULL, CFSTR("SCNetworkReachability"), NULL, NULL);
				if (store_info->store == NULL) {
					SCLog(TRUE, LOG_ERR, CFSTR("ReachabilityStoreInfo_update SCDynamicStoreCreate() failed"));
					return;
				}

				if (storeP != NULL) {
					// and pass back a reference
					*storeP = store_info->store;
					CFRetain(*storeP);
				}
			}
		}

		if (sa_family == AF_UNSPEC) {
			// if the address family was not specified than
			// all we wanted, for now, was to establish the
			// SCDynamicStore session
			return;
		}

		if (store_info->dict != NULL) {
			// or we have picked up the shared info
			return;
		}

		ok = ReachabilityStoreInfo_fill(store_info);
		if (!ok) {
			return;
		}

		if (!_SC_CFEqual(store_info->dict, S_storeInfo.dict)) {
			// free any old info
			ReachabilityStoreInfo_free(&S_storeInfo);

			// save new info
			if (S_storeInfoActive &&
			    (store_info->dict != NULL)) {
				ReachabilityStoreInfo_copy(store_info, &S_storeInfo);
			}
		}
	});

	return ok;
}


#pragma mark -
#pragma mark PPP info


static int
updatePPPStatus(ReachabilityStoreInfoRef	store_info,
		const struct sockaddr		*sa,
		const char			*if_name,
		SCNetworkReachabilityFlags	*flags,
		CFStringRef			*ppp_server,
		const char			*log_prefix)
{
	CFIndex		i;
	CFStringRef	ppp_if;
	int		sc_status	= kSCStatusNoKey;

	if (!ReachabilityStoreInfo_update(store_info, NULL, sa->sa_family)) {
		return kSCStatusReachabilityUnknown;
	}

	if (store_info->n <= 0) {
		// if no services
		return kSCStatusNoKey;
	}

	// look for the [PPP] service which matches the provided interface

	ppp_if = CFStringCreateWithCStringNoCopy(NULL,
						 if_name,
						 kCFStringEncodingASCII,
						 kCFAllocatorNull);

	for (i=0; i < store_info->n; i++) {
		CFArrayRef	components;
		CFStringRef	key;
		CFNumberRef	num;
		CFDictionaryRef	p_setup;
		CFDictionaryRef	p_state;
		int32_t		ppp_demand;
		int32_t		ppp_status;
		CFStringRef	service		= NULL;
		CFStringRef	s_key		= (CFStringRef)    store_info->keys[i];
		CFDictionaryRef	s_dict		= (CFDictionaryRef)store_info->values[i];
		CFStringRef	s_if;

		if (!isA_CFString(s_key) || !isA_CFDictionary(s_dict)) {
			continue;
		}

		if (!CFStringHasSuffix(s_key, store_info->entity) ||
		    !CFStringHasPrefix(s_key, kSCDynamicStoreDomainState)) {
			continue;	// if not an active IPv4 or IPv6 entity
		}

		s_if = CFDictionaryGetValue(s_dict, kSCPropInterfaceName);
		if (!isA_CFString(s_if)) {
			continue;	// if no interface
		}

		if (!CFEqual(ppp_if, s_if)) {
			continue;	// if not this interface
		}

		// extract the service ID, get the PPP "state" entity for
		// the "Status", and get the PPP "setup" entity for the
		// the "DialOnDemand" flag
		components = CFStringCreateArrayBySeparatingStrings(NULL, s_key, CFSTR("/"));
		if (CFArrayGetCount(components) != 5) {
			CFRelease(components);
			break;
		}
		service = CFArrayGetValueAtIndex(components, 3);
		key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								  kSCDynamicStoreDomainState,
								  service,
								  kSCEntNetPPP);
		p_state = CFDictionaryGetValue(store_info->dict, key);
		CFRelease(key);
		key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								  kSCDynamicStoreDomainSetup,
								  service,
								  kSCEntNetPPP);
		p_setup = CFDictionaryGetValue(store_info->dict, key);
		CFRelease(key);
		CFRelease(components);

		// ensure that this is a PPP service
		if (!isA_CFDictionary(p_state)) {
			break;
		}

		sc_status = kSCStatusOK;

		*flags |= kSCNetworkReachabilityFlagsTransientConnection;

		// get PPP server
		if (ppp_server != NULL) {
			*ppp_server = CFDictionaryGetValue(s_dict, CFSTR("ServerAddress"));
			*ppp_server = isA_CFString(*ppp_server);
			if (*ppp_server != NULL) {
				CFRetain(*ppp_server);
			}
		}

		// get PPP status
		if (!CFDictionaryGetValueIfPresent(p_state,
						   kSCPropNetPPPStatus,
						   (const void **)&num) ||
		    !isA_CFNumber(num) ||
		    !CFNumberGetValue(num, kCFNumberSInt32Type, &ppp_status)) {
			break;
		}
		switch (ppp_status) {
			case PPP_RUNNING :
				// if we're really UP and RUNNING
				break;
			case PPP_ONHOLD :
				// if we're effectively UP and RUNNING
				break;
			case PPP_IDLE :
				// if we're not connected at all
				SCLog(_sc_debug, LOG_INFO, CFSTR("%s  PPP link idle"),
				      log_prefix);
				*flags |= kSCNetworkReachabilityFlagsConnectionRequired;
				break;
			case PPP_STATERESERVED :
				// if we're not connected at all
				SCLog(_sc_debug, LOG_INFO, CFSTR("%s  PPP link idle, dial-on-traffic to connect"),
				      log_prefix);
				*flags |= kSCNetworkReachabilityFlagsConnectionRequired;
				break;
			default :
				// if we're in the process of [dis]connecting
				SCLog(_sc_debug, LOG_INFO, CFSTR("%s  PPP link, connection in progress"),
				      log_prefix);
				*flags |= kSCNetworkReachabilityFlagsConnectionRequired;
				break;
		}

		// get PPP dial-on-traffic status
		if (isA_CFDictionary(p_setup) &&
		    CFDictionaryGetValueIfPresent(p_setup,
						  kSCPropNetPPPDialOnDemand,
						  (const void **)&num) &&
		    isA_CFNumber(num) &&
		    CFNumberGetValue(num, kCFNumberSInt32Type, &ppp_demand) &&
		    (ppp_demand != 0)) {
			*flags |= kSCNetworkReachabilityFlagsConnectionOnTraffic;
			if (ppp_status == PPP_IDLE) {
				*flags |= kSCNetworkReachabilityFlagsInterventionRequired;
			}
		}

		break;
	}

	CFRelease(ppp_if);

	return sc_status;
}


static int
updatePPPAvailable(ReachabilityStoreInfoRef	store_info,
		   const struct sockaddr	*sa,
		   SCNetworkReachabilityFlags	*flags,
		   const char			*log_prefix)
{
	CFIndex		i;
	int		sc_status	= kSCStatusNoKey;

	if (!ReachabilityStoreInfo_update(store_info,
					 NULL,
					 (sa != NULL) ? sa->sa_family : AF_INET)) {
		return kSCStatusReachabilityUnknown;
	}

	if (store_info->n <= 0) {
		// if no services
		return kSCStatusNoKey;
	}

	// look for an available service which will provide connectivity
	// for the requested address family.

	for (i = 0; i < store_info->n; i++) {
		CFArrayRef	components;
		Boolean		found		= FALSE;
		CFStringRef	i_key;
		CFDictionaryRef	i_dict;
		CFStringRef	p_key;
		CFDictionaryRef	p_dict;
		CFStringRef	service;
		CFStringRef	s_key		= (CFStringRef)    store_info->keys[i];
		CFDictionaryRef	s_dict		= (CFDictionaryRef)store_info->values[i];

		if (!isA_CFString(s_key) || !isA_CFDictionary(s_dict)) {
			continue;
		}

		if (!CFStringHasSuffix(s_key, store_info->entity) ||
		    !CFStringHasPrefix(s_key, kSCDynamicStoreDomainSetup)) {
			continue;	// if not an IPv4 or IPv6 entity
		}

		// extract service ID
		components = CFStringCreateArrayBySeparatingStrings(NULL, s_key, CFSTR("/"));
		if (CFArrayGetCount(components) != 5) {
			CFRelease(components);
			continue;
		}
		service = CFArrayGetValueAtIndex(components, 3);

		// check for [non-VPN] PPP entity
		p_key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								    kSCDynamicStoreDomainSetup,
								    service,
								    kSCEntNetPPP);
		p_dict = CFDictionaryGetValue(store_info->dict, p_key);
		CFRelease(p_key);

		i_key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								    kSCDynamicStoreDomainSetup,
								    service,
								    kSCEntNetInterface);
		i_dict = CFDictionaryGetValue(store_info->dict, i_key);
		CFRelease(i_key);

		if (isA_CFDictionary(p_dict) &&
		    isA_CFDictionary(i_dict) &&
		    CFDictionaryContainsKey(i_dict, kSCPropNetInterfaceDeviceName)) {
			CFNumberRef	num;

			// we have a PPP service for this address family
			found = TRUE;

			*flags |= kSCNetworkReachabilityFlagsReachable;
			*flags |= kSCNetworkReachabilityFlagsTransientConnection;
			*flags |= kSCNetworkReachabilityFlagsConnectionRequired;

			// get PPP dial-on-traffic status
			num = CFDictionaryGetValue(p_dict, kSCPropNetPPPDialOnDemand);
			if (isA_CFNumber(num)) {
				int32_t	ppp_demand;

				if (CFNumberGetValue(num, kCFNumberSInt32Type, &ppp_demand)) {
					if (ppp_demand) {
						*flags |= kSCNetworkReachabilityFlagsConnectionOnTraffic;
					}
				}
			}

			if (_sc_debug) {
				SCLog(TRUE, LOG_INFO, CFSTR("%s  status    = isReachable (after connect)"),
				      log_prefix);
				SCLog(TRUE, LOG_INFO, CFSTR("%s  service   = %@"),
				      log_prefix,
				      service);
			}

		}

		CFRelease(components);

		if (found) {
			sc_status = kSCStatusOK;
			break;
		}
	}

	return sc_status;
}


#pragma mark -
#pragma mark VPN info


#if	!TARGET_IPHONE_SIMULATOR
static int
updateVPNStatus(ReachabilityStoreInfoRef	store_info,
		const struct sockaddr		*sa,
		const char			*if_name,
		SCNetworkReachabilityFlags	*flags,
		CFStringRef			*vpn_server,
		const char			*log_prefix)
{
	CFIndex		i;
	CFStringRef	vpn_if;
	int		sc_status	= kSCStatusNoKey;

	if (!ReachabilityStoreInfo_update(store_info, NULL, sa->sa_family)) {
		return kSCStatusReachabilityUnknown;
	}

	if (store_info->n <= 0) {
		// if no services
		return kSCStatusNoKey;
	}

	// look for the [VPN] service which matches the provided interface

	vpn_if = CFStringCreateWithCStringNoCopy(NULL,
						 if_name,
						 kCFStringEncodingASCII,
						 kCFAllocatorNull);

	for (i=0; i < store_info->n; i++) {
		CFArrayRef	components;
		CFStringRef	key;
		CFNumberRef	num;
		CFDictionaryRef	p_state;
		int32_t		vpn_status;
		CFStringRef	service		= NULL;
		CFStringRef	s_key		= (CFStringRef)    store_info->keys[i];
		CFDictionaryRef	s_dict		= (CFDictionaryRef)store_info->values[i];
		CFStringRef	s_if;

		if (!isA_CFString(s_key) || !isA_CFDictionary(s_dict)) {
			continue;
		}

		if (!CFStringHasSuffix(s_key, store_info->entity) ||
		    !CFStringHasPrefix(s_key, kSCDynamicStoreDomainState)) {
			continue;	// if not an active IPv4 or IPv6 entity
		}

		s_if = CFDictionaryGetValue(s_dict, kSCPropInterfaceName);
		if (!isA_CFString(s_if)) {
			continue;	// if no interface
		}

		if (!CFEqual(vpn_if, s_if)) {
			continue;	// if not this interface
		}

		// extract the service ID and get the VPN "state" entity for
		// the "Status"
		components = CFStringCreateArrayBySeparatingStrings(NULL, s_key, CFSTR("/"));
		if (CFArrayGetCount(components) != 5) {
			CFRelease(components);
			break;
		}
		service = CFArrayGetValueAtIndex(components, 3);
		key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								  kSCDynamicStoreDomainState,
								  service,
								  kSCEntNetVPN);
		p_state = CFDictionaryGetValue(store_info->dict, key);
		CFRelease(key);
		CFRelease(components);

		// ensure that this is a VPN service
		if (!isA_CFDictionary(p_state)) {
			break;
		}

		sc_status = kSCStatusOK;

		*flags |= kSCNetworkReachabilityFlagsTransientConnection;

		// get VPN server
		if (vpn_server != NULL) {
			*vpn_server = CFDictionaryGetValue(s_dict, CFSTR("ServerAddress"));
			*vpn_server = isA_CFString(*vpn_server);
			if (*vpn_server != NULL) {
				CFRetain(*vpn_server);
			}
		}

		// get VPN status
		if (!CFDictionaryGetValueIfPresent(p_state,
						   kSCPropNetVPNStatus,
						   (const void **)&num) ||
		    !isA_CFNumber(num) ||
		    !CFNumberGetValue(num, kCFNumberSInt32Type, &vpn_status)) {
			break;
		}
#ifdef	HAVE_VPN_STATUS
		switch (vpn_status) {
			case VPN_RUNNING :
				// if we're really UP and RUNNING
				break;
			case VPN_IDLE :
			case VPN_LOADING :
			case VPN_LOADED :
			case VPN_UNLOADING :
				// if we're not connected at all
				SCLog(_sc_debug, LOG_INFO, CFSTR("%s  VPN link idle"),
				      log_prefix);
				*flags |= kSCNetworkReachabilityFlagsConnectionRequired;
				break;
			default :
				// if we're in the process of [dis]connecting
				SCLog(_sc_debug, LOG_INFO, CFSTR("%s  VPN link, connection in progress"),
				      log_prefix);
				*flags |= kSCNetworkReachabilityFlagsConnectionRequired;
				break;
		}
#endif	// HAVE_VPN_STATUS

		break;
	}

	CFRelease(vpn_if);

	return sc_status;
}


static int
updateVPNAvailable(ReachabilityStoreInfoRef	store_info,
		   const struct sockaddr	*sa,
		   SCNetworkReachabilityFlags	*flags,
		   const char			*log_prefix)
{
	CFIndex		i;
	int		sc_status	= kSCStatusNoKey;

	if (!ReachabilityStoreInfo_update(store_info,
					 NULL,
					 (sa != NULL) ? sa->sa_family : AF_INET)) {
		return kSCStatusReachabilityUnknown;
	}

	if (store_info->n <= 0) {
		// if no services
		return kSCStatusNoKey;
	}

	// look for an available service which will provide connectivity
	// for the requested address family.

	for (i = 0; i < store_info->n; i++) {
		CFArrayRef	components;
		Boolean		found		= FALSE;
		CFStringRef	i_key;
		CFDictionaryRef	i_dict;
		CFStringRef	p_key;
		CFDictionaryRef	p_dict;
		CFStringRef	service;
		CFStringRef	s_key		= (CFStringRef)    store_info->keys[i];
		CFDictionaryRef	s_dict		= (CFDictionaryRef)store_info->values[i];

		if (!isA_CFString(s_key) || !isA_CFDictionary(s_dict)) {
			continue;
		}

		if (!CFStringHasSuffix(s_key, store_info->entity) ||
		    !CFStringHasPrefix(s_key, kSCDynamicStoreDomainSetup)) {
			continue;	// if not an IPv4 or IPv6 entity
		}

		// extract service ID
		components = CFStringCreateArrayBySeparatingStrings(NULL, s_key, CFSTR("/"));
		if (CFArrayGetCount(components) != 5) {
			CFRelease(components);
			continue;
		}
		service = CFArrayGetValueAtIndex(components, 3);

		// check for VPN entity
		p_key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								    kSCDynamicStoreDomainSetup,
								    service,
								    kSCEntNetVPN);
		p_dict = CFDictionaryGetValue(store_info->dict, p_key);
		CFRelease(p_key);

		i_key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								    kSCDynamicStoreDomainSetup,
								    service,
								    kSCEntNetInterface);
		i_dict = CFDictionaryGetValue(store_info->dict, i_key);
		CFRelease(i_key);

		if (isA_CFDictionary(p_dict) &&
		    isA_CFDictionary(i_dict) &&
		    CFDictionaryContainsKey(i_dict, kSCPropNetInterfaceDeviceName)) {
			// we have a VPN service for this address family
			found = TRUE;

			*flags |= kSCNetworkReachabilityFlagsReachable;
			*flags |= kSCNetworkReachabilityFlagsTransientConnection;
			*flags |= kSCNetworkReachabilityFlagsConnectionRequired;

			if (_sc_debug) {
				SCLog(TRUE, LOG_INFO, CFSTR("%s  status    = isReachable (after connect)"),
				      log_prefix);
				SCLog(TRUE, LOG_INFO, CFSTR("%s  service   = %@"),
				      log_prefix,
				      service);
			}

		}

		CFRelease(components);

		if (found) {
			sc_status = kSCStatusOK;
			break;
		}
	}

	return sc_status;
}
#endif	// !TARGET_IPHONE_SIMULATOR


#pragma mark -
#pragma mark IPSec info


static int
updateIPSecStatus(ReachabilityStoreInfoRef	store_info,
		  const struct sockaddr		*sa,
		  const char			*if_name,
		  SCNetworkReachabilityFlags	*flags,
		  CFStringRef			*ipsec_server,
		  const char			*log_prefix)
{
	CFIndex		i;
	CFStringRef	ipsec_if;
	int		sc_status	= kSCStatusNoKey;

	if (!ReachabilityStoreInfo_update(store_info, NULL, sa->sa_family)) {
		return kSCStatusReachabilityUnknown;
	}

	if (store_info->n <= 0) {
		// if no services
		return kSCStatusNoKey;
	}

	// look for the [IPSec] service that matches the provided interface

	ipsec_if = CFStringCreateWithCStringNoCopy(NULL,
						   if_name,
						   kCFStringEncodingASCII,
						   kCFAllocatorNull);

	for (i=0; i < store_info->n; i++) {
		CFArrayRef	components;
		CFStringRef	key;
		CFDictionaryRef	i_state;
		int32_t		ipsec_status;
		CFNumberRef	num;
		CFStringRef	service		= NULL;
		CFStringRef	s_key		= (CFStringRef)    store_info->keys[i];
		CFDictionaryRef	s_dict		= (CFDictionaryRef)store_info->values[i];
		CFStringRef	s_if;

		if (!isA_CFString(s_key) || !isA_CFDictionary(s_dict)) {
			continue;
		}

		if (!CFStringHasSuffix(s_key, store_info->entity) ||
		    !CFStringHasPrefix(s_key, kSCDynamicStoreDomainState)) {
			continue;	// if not an IPv4 or IPv6 entity
		}

		s_if = CFDictionaryGetValue(s_dict, kSCPropInterfaceName);
		if (!isA_CFString(s_if)) {
			continue;	// if no interface
		}

		if (!CFEqual(ipsec_if, s_if)) {
			continue;	// if not this interface
		}

		// extract the service ID, get the IPSec "state" entity for
		// the "Status", and get the IPSec "setup" entity to confirm
		// that we're looking at what we're expecting
		components = CFStringCreateArrayBySeparatingStrings(NULL, s_key, CFSTR("/"));
		if (CFArrayGetCount(components) != 5) {
			CFRelease(components);
			break;
		}
		service = CFArrayGetValueAtIndex(components, 3);
		key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
								  kSCDynamicStoreDomainState,
								  service,
								  kSCEntNetIPSec);
		i_state = CFDictionaryGetValue(store_info->dict, key);
		CFRelease(key);
		CFRelease(components);

		// ensure that this is an IPSec service
		if (!isA_CFDictionary(i_state)) {
			break;
		}

		sc_status = kSCStatusOK;

		*flags |= kSCNetworkReachabilityFlagsTransientConnection;

		// get IPSec server
		if (ipsec_server != NULL) {
			*ipsec_server = CFDictionaryGetValue(s_dict, CFSTR("ServerAddress"));
			*ipsec_server = isA_CFString(*ipsec_server);
			if (*ipsec_server != NULL) {
				CFRetain(*ipsec_server);
			}
		}

		// get IPSec status
		if (!CFDictionaryGetValueIfPresent(i_state,
						   kSCPropNetIPSecStatus,
						   (const void **)&num) ||
		    !isA_CFNumber(num) ||
		    !CFNumberGetValue(num, kCFNumberSInt32Type, &ipsec_status)) {
			break;
		}
#ifdef	HAVE_IPSEC_STATUS
		switch (ipsec_status) {
			case IPSEC_RUNNING :
				// if we're really UP and RUNNING
				break;
			case IPSEC_IDLE :
				// if we're not connected at all
				SCLog(_sc_debug, LOG_INFO, CFSTR("%s  IPSec link idle"),
				      log_prefix);
				*flags |= kSCNetworkReachabilityFlagsConnectionRequired;
				break;
			default :
				// if we're in the process of [dis]connecting
				SCLog(_sc_debug, LOG_INFO, CFSTR("%s  IPSec link, connection in progress"),
				      log_prefix);
				*flags |= kSCNetworkReachabilityFlagsConnectionRequired;
				break;
		}
#endif	// HAVE_IPSEC_STATUS

		break;
	}

	CFRelease(ipsec_if);

	return sc_status;
}




#pragma mark -
#pragma mark Reachability engine


#define ROUNDUP(a, size) \
	(((a) & ((size)-1)) ? (1 + ((a) | ((size)-1))) : (a))

#define NEXT_SA(ap) (ap) = (struct sockaddr *) \
	((caddr_t)(ap) + ((ap)->sa_len ? ROUNDUP((ap)->sa_len,\
						 sizeof(uint32_t)) :\
						 sizeof(uint32_t)))

static void
get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info)
{
	int             i;

	for (i = 0; i < RTAX_MAX; i++) {
		if (addrs & (1 << i)) {
			rti_info[i] = sa;
			NEXT_SA(sa);
		} else
			rti_info[i] = NULL;
	}
}


#define BUFLEN (sizeof(struct rt_msghdr) + 512)	/* 8 * sizeof(struct sockaddr_in6) = 192 */


typedef struct {
	union {
		char			bytes[BUFLEN];
		struct rt_msghdr	rtm;
	} buf;
	int			error;
	struct sockaddr         *rti_info[RTAX_MAX];
	struct rt_msghdr	*rtm;
	struct sockaddr_dl	*sdl;
} route_info, *route_info_p;


/*
 * route_get()
 *	returns	zero if route exists an data returned, EHOSTUNREACH
 *	if no route, or errno for any other error.
 */
static int
route_get(const struct sockaddr	*address,
	  unsigned int		if_index,
	  route_info		*info)
{
	int			n;
	int			opt;
	pid_t			pid		= getpid();
	int			rsock;
	struct sockaddr         *sa;
	int32_t			seq		= OSAtomicIncrement32Barrier(&rtm_seq);
#ifndef	RTM_GET_SILENT
#warning Note: Using RTM_GET (and not RTM_GET_SILENT)
	static pthread_mutex_t	lock		= PTHREAD_MUTEX_INITIALIZER;
	int			sosize		= 48 * 1024;
#endif

	bzero(info, sizeof(*info));

	info->rtm = &info->buf.rtm;
	info->rtm->rtm_msglen  = sizeof(struct rt_msghdr);
	info->rtm->rtm_version = RTM_VERSION;
#ifdef	RTM_GET_SILENT
	info->rtm->rtm_type    = RTM_GET_SILENT;
#else
	info->rtm->rtm_type    = RTM_GET;
#endif
	info->rtm->rtm_flags   = RTF_STATIC|RTF_UP|RTF_HOST|RTF_GATEWAY;
	info->rtm->rtm_addrs   = RTA_DST|RTA_IFP; /* Both destination and device */
	info->rtm->rtm_pid     = pid;
	info->rtm->rtm_seq     = seq;

	if (if_index != 0) {
		info->rtm->rtm_flags |= RTF_IFSCOPE;
		info->rtm->rtm_index = if_index;
	}

	switch (address->sa_family) {
		case AF_INET6: {
			struct sockaddr_in6	*sin6;

			/* ALIGN: caller ensures that the address is aligned */
			sin6 = (struct sockaddr_in6 *)(void *)address;
			if ((IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) ||
			     IN6_IS_ADDR_MC_LINKLOCAL(&sin6->sin6_addr)) &&
			    (sin6->sin6_scope_id != 0)) {
				sin6->sin6_addr.s6_addr16[1] = htons(sin6->sin6_scope_id);
				sin6->sin6_scope_id = 0;
			}
			break;
		}
	}

	sa  = (struct sockaddr *) (info->rtm + 1);
	bcopy(address, sa, address->sa_len);
	n = ROUNDUP(sa->sa_len, sizeof(uint32_t));
	info->rtm->rtm_msglen += n;

	info->sdl = (struct sockaddr_dl *) ((void *)sa + n);
	info->sdl->sdl_family = AF_LINK;
	info->sdl->sdl_len = sizeof (struct sockaddr_dl);
	n = ROUNDUP(info->sdl->sdl_len, sizeof(uint32_t));
	info->rtm->rtm_msglen += n;

#ifndef	RTM_GET_SILENT
	pthread_mutex_lock(&lock);
#endif
	rsock = socket(PF_ROUTE, SOCK_RAW, PF_ROUTE);
	if (rsock == -1) {
		int	error	= errno;

#ifndef	RTM_GET_SILENT
		pthread_mutex_unlock(&lock);
#endif
		SCLog(TRUE, LOG_ERR, CFSTR("socket(PF_ROUTE) failed: %s"), strerror(error));
		return error;
	}
	opt = 1;
	if (ioctl(rsock, FIONBIO, &opt) < 0) {
		int	error	= errno;

		(void)close(rsock);
#ifndef	RTM_GET_SILENT
		pthread_mutex_unlock(&lock);
#endif
		SCLog(TRUE, LOG_ERR, CFSTR("ioctl(FIONBIO) failed: %s"), strerror(error));
		return error;
	}

#ifndef	RTM_GET_SILENT
	if (setsockopt(rsock, SOL_SOCKET, SO_RCVBUF, &sosize, sizeof(sosize)) == -1) {
		int	error	= errno;

		(void)close(rsock);
		pthread_mutex_unlock(&lock);
		SCLog(TRUE, LOG_ERR, CFSTR("setsockopt(SO_RCVBUF) failed: %s"), strerror(error));
		return error;
	}
#endif

	if (write(rsock, &info->buf, info->rtm->rtm_msglen) == -1) {
		int	error	= errno;

		(void)close(rsock);
#ifndef	RTM_GET_SILENT
		pthread_mutex_unlock(&lock);
#endif
		if (error != ESRCH) {
			SCLog(TRUE, LOG_ERR, CFSTR("write() failed: %s"), strerror(error));
			return error;
		}
		return EHOSTUNREACH;
	}

	/*
	 * Type, seq, pid identify our response.
	 * Routing sockets are broadcasters on input.
	 */
	while (TRUE) {
		int	n;

		n = read(rsock, &info->buf, sizeof(info->buf));
		if (n == -1) {
			int	error	= errno;

			if (error == EINTR) {
				continue;
			}
			(void)close(rsock);
#ifndef	RTM_GET_SILENT
			pthread_mutex_unlock(&lock);
#endif
			SCLog(TRUE, LOG_ERR,
			      CFSTR("SCNetworkReachability: routing socket"
				    " read() failed: %s"), strerror(error));
			return error;
		}
		if ((info->rtm->rtm_type == RTM_GET) 	&&
		    (info->rtm->rtm_seq == seq) 	&&
		    (info->rtm->rtm_pid == pid)) {
		    break;
		}
	}

	(void)close(rsock);
#ifndef	RTM_GET_SILENT
	pthread_mutex_unlock(&lock);
#endif

	get_rtaddrs(info->rtm->rtm_addrs, sa, info->rti_info);

//#define LOG_RTADDRS
#ifdef	LOG_RTADDRS
	{
		int	i;

		SCLog(_sc_debug, LOG_DEBUG, CFSTR("rtm_flags = 0x%8.8x"), info->rtm->rtm_flags);

		if ((info->rti_info[RTAX_NETMASK] != NULL) && (info->rti_info[RTAX_DST] != NULL)) {
			info->rti_info[RTAX_NETMASK]->sa_family = info->rti_info[RTAX_DST]->sa_family;
		}

		for (i = 0; i < RTAX_MAX; i++) {
			if (info->rti_info[i] != NULL) {
				char	addr[128];

				_SC_sockaddr_to_string(info->rti_info[i], addr, sizeof(addr));
				SCLog(_sc_debug, LOG_DEBUG, CFSTR("%d: %s"), i, addr);
			}
		}
	}
#endif	/* LOG_RTADDRS */

	if ((info->rti_info[RTAX_IFP] == NULL) ||
	    (info->rti_info[RTAX_IFP]->sa_family != AF_LINK)) {
		/* no interface info */
		SCLog(TRUE, LOG_DEBUG, CFSTR("route_get() no interface info"));
		return EINVAL;
	}

	/* ALIGN: accessors are retrieving byte values, cast ok. */
	info->sdl = (struct sockaddr_dl *)(void *) info->rti_info[RTAX_IFP];
	if ((info->sdl->sdl_nlen == 0) || (info->sdl->sdl_nlen > IFNAMSIZ)) {
		/* no interface name */
		return EHOSTUNREACH;
	}

	return 0;
}


static Boolean
checkAddress(ReachabilityStoreInfoRef	store_info,
	     const struct sockaddr	*address,
	     unsigned int		if_index,
	     ReachabilityInfo		*reach_info,
	     const char			*log_prefix)
{
	route_info		info;
	struct ifreq		ifr;
	char			if_name[IFNAMSIZ];
	int			isock		= -1;
	int			ret;
	int			sc_status	= kSCStatusReachabilityUnknown;
	CFStringRef		server		= NULL;
	char			*statusMessage	= NULL;
	struct sockaddr_in	v4mapped;

	_reach_set(reach_info, &NOT_REACHABLE, reach_info->cycle);

	if (address == NULL) {
		/* special case: check only for available paths off the system */
		goto checkAvailable;
	}

	switch (address->sa_family) {
		case AF_INET :
		case AF_INET6 :
			if (_sc_debug) {
				char	addr[128];
				char	if_name[IFNAMSIZ + 1];

				_SC_sockaddr_to_string(address, addr, sizeof(addr));

				if ((if_index != 0) &&
				    (if_indextoname(if_index, &if_name[1]) != NULL)) {
					if_name[0] = '%';
				} else {
					if_name[0] = '\0';
				}

				SCLog(TRUE, LOG_INFO, CFSTR("%scheckAddress(%s%s)"),
				      log_prefix,
				      addr,
				      if_name);
			}
			break;
		default :
			/*
			 * if no code for this address family (yet)
			 */
			SCLog(TRUE, LOG_INFO,
			      CFSTR("checkAddress(): unexpected address family %d"),
			      address->sa_family);
			sc_status = kSCStatusInvalidArgument;
			goto done;
	}

	if (address->sa_family == AF_INET6) {
		/* ALIGN: sin6_addr accessed aligned, cast ok. */
		struct sockaddr_in6	*sin6	= (struct sockaddr_in6 *)(void *)address;

		if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
			bzero(&v4mapped, sizeof(v4mapped));
			v4mapped.sin_len         = sizeof(v4mapped);
			v4mapped.sin_family      = AF_INET;
			v4mapped.sin_port        = sin6->sin6_port;
			v4mapped.sin_addr.s_addr = sin6->sin6_addr.__u6_addr.__u6_addr32[3];
			address = (struct sockaddr *)&v4mapped;
		}
	}

	ret = route_get(address, if_index, &info);
	switch (ret) {
		case 0 :
			break;
		case EHOSTUNREACH :
			// if no route
			goto checkAvailable;
		default :
			// if error
			sc_status = ret;
			goto done;
	}

	/* get the interface flags */

	isock = socket(AF_INET, SOCK_DGRAM, 0);
	if (isock == -1) {
		SCLog(TRUE, LOG_ERR, CFSTR("socket() failed: %s"), strerror(errno));
		goto done;
	}

	bzero(&ifr, sizeof(ifr));
	bcopy(info.sdl->sdl_data, ifr.ifr_name, info.sdl->sdl_nlen);

	if (ioctl(isock, SIOCGIFFLAGS, (char *)&ifr) == -1) {
		SCLog(TRUE, LOG_ERR, CFSTR("ioctl() failed: %s"), strerror(errno));
		goto done;
	}

	if (!(ifr.ifr_flags & IFF_UP)) {
		goto checkAvailable;
	}

	statusMessage = "isReachable";
	reach_info->flags |= kSCNetworkReachabilityFlagsReachable;

	if (info.rtm->rtm_flags & RTF_LOCAL) {
		statusMessage = "isReachable (is a local address)";
		reach_info->flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
	} else if (ifr.ifr_flags & IFF_LOOPBACK) {
		statusMessage = "isReachable (is loopback network)";
		reach_info->flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
	} else if ((info.rti_info[RTAX_IFA] != NULL) &&
		   (info.rti_info[RTAX_IFA]->sa_family != AF_LINK)) {
		void	*addr1	= (void *)address;
		void	*addr2	= (void *)info.rti_info[RTAX_IFA];
		size_t	len	= address->sa_len;

		if ((address->sa_family != info.rti_info[RTAX_IFA]->sa_family) &&
		    (address->sa_len    != info.rti_info[RTAX_IFA]->sa_len)) {
			SCLog(TRUE, LOG_NOTICE,
			      CFSTR("address family/length mismatch: %d/%d != %d/%d"),
			      address->sa_family,
			      address->sa_len,
			      info.rti_info[RTAX_IFA]->sa_family,
			      info.rti_info[RTAX_IFA]->sa_len);
			goto done;
		}

		switch (address->sa_family) {
			case AF_INET :
				/* ALIGN: cast ok, because only bcmp is used. */
				addr1 = &((struct sockaddr_in *)(void *)address)->sin_addr;
				addr2 = &((struct sockaddr_in *)(void *)info.rti_info[RTAX_IFA])->sin_addr;
				len = sizeof(struct in_addr);

				/*
				 * check if 0.0.0.0
				 */
				/* ALIGN: sin_addr should be aligned, cast ok. */
				if (((struct sockaddr_in *)(void *)address)->sin_addr.s_addr == 0) {
					statusMessage = "isReachable (this host)";
					reach_info->flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
				}
				break;
			case AF_INET6 :
				 /* ALIGN: cast ok, because only bcmp is used. */
				addr1 = &((struct sockaddr_in6 *)(void *)address)->sin6_addr;
				addr2 = &((struct sockaddr_in6 *)(void *)info.rti_info[RTAX_IFA])->sin6_addr;
				len = sizeof(struct in6_addr);
				break;
			default :
				break;
		}

		if (bcmp(addr1, addr2, len) == 0) {
			statusMessage = "isReachable (is interface address)";
			reach_info->flags |= kSCNetworkReachabilityFlagsIsLocalAddress;
		}
	}

	if (!(info.rtm->rtm_flags & RTF_GATEWAY) &&
	    (info.rti_info[RTAX_GATEWAY] != NULL) &&
	    (info.rti_info[RTAX_GATEWAY]->sa_family == AF_LINK) &&
	    !(ifr.ifr_flags & IFF_POINTOPOINT)) {
		reach_info->flags |= kSCNetworkReachabilityFlagsIsDirect;
	}

	bzero(&if_name, sizeof(if_name));
	bcopy(info.sdl->sdl_data,
	      if_name,
	      (info.sdl->sdl_nlen <= IFNAMSIZ) ? info.sdl->sdl_nlen : IFNAMSIZ);

	strlcpy(reach_info->if_name, if_name, sizeof(reach_info->if_name));
	reach_info->if_index = info.sdl->sdl_index;

	if (_sc_debug) {
		SCLog(TRUE, LOG_INFO, CFSTR("%s  status    = %s"), log_prefix, statusMessage);
		SCLog(TRUE, LOG_INFO, CFSTR("%s  device    = %s (%hu)"), log_prefix, if_name, info.sdl->sdl_index);
		SCLog(TRUE, LOG_INFO, CFSTR("%s  sdl_type  = 0x%x"), log_prefix, info.sdl->sdl_type);
		SCLog(TRUE, LOG_INFO, CFSTR("%s  ifr_flags = 0x%04hx"), log_prefix, ifr.ifr_flags);
		SCLog(TRUE, LOG_INFO, CFSTR("%s  rtm_flags = 0x%08x"), log_prefix, info.rtm->rtm_flags);
	}

	sc_status = kSCStatusOK;

	if (ifr.ifr_flags & IFF_POINTOPOINT) {
		reach_info->flags |= kSCNetworkReachabilityFlagsTransientConnection;
	}

	if (info.sdl->sdl_type == IFT_PPP) {
		/*
		 * 1. check if PPP service
		 * 2. check for dial-on-demand PPP link that is not yet connected
		 * 3. get PPP server address
		 */
		sc_status = updatePPPStatus(store_info, address, if_name, &reach_info->flags, &server, log_prefix);
	} else if (info.sdl->sdl_type == IFT_OTHER) {
		/*
		 * 1. check if IPSec service
		 * 2. get IPSec server address
		 */
		sc_status = updateIPSecStatus(store_info, address, if_name, &reach_info->flags, &server, log_prefix);

#if	!TARGET_IPHONE_SIMULATOR
		if (sc_status == kSCStatusNoKey) {
			/*
			 * 1. check if VPN service
			 * 2. get VPN server address
			 */
			sc_status = updateVPNStatus(store_info, address, if_name, &reach_info->flags, &server, log_prefix);
		}
#endif	// !TARGET_IPHONE_SIMULATOR
	}


	goto done;

    checkAvailable :


	sc_status = updatePPPAvailable(store_info, address, &reach_info->flags, log_prefix);
	if ((sc_status == kSCStatusOK) && (reach_info->flags != 0)) {
		goto done;
	}

#if	!TARGET_IPHONE_SIMULATOR
	sc_status = updateVPNAvailable(store_info, address, &reach_info->flags, log_prefix);
	if ((sc_status == kSCStatusOK) && (reach_info->flags != 0)) {
		goto done;
	}
#endif	// !TARGET_IPHONE_SIMULATOR

    done :

	if (reach_info->flags == 0) {
		SCLog(_sc_debug, LOG_INFO, CFSTR("%s  cannot be reached"), log_prefix);
	}

	if (isock != -1)	(void)close(isock);
	if (server != NULL)	CFRelease(server);
	if ((sc_status != kSCStatusOK) && (sc_status != kSCStatusNoKey)) {
		_SCErrorSet(sc_status);
		return FALSE;
	}

	return TRUE;
}


#pragma mark -
#pragma mark SCNetworkReachability APIs


static __inline__ CFTypeRef
isA_SCNetworkReachability(CFTypeRef obj)
{
	return (isA_CFType(obj, SCNetworkReachabilityGetTypeID()));
}


CFStringRef
_SCNetworkReachabilityCopyTargetDescription(SCNetworkReachabilityRef target)
{
	CFAllocatorRef			allocator	= CFGetAllocator(target);
	CFMutableStringRef		str;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	str = CFStringCreateMutable(allocator, 0);
	switch (targetPrivate->type) {
		case reachabilityTypeAddress :
		case reachabilityTypeAddressPair : {
			char		buf[64];

			if (targetPrivate->localAddress != NULL) {
				_SC_sockaddr_to_string(targetPrivate->localAddress, buf, sizeof(buf));
				CFStringAppendFormat(str, NULL, CFSTR("local address = %s"),
						     buf);
			}

			if (targetPrivate->remoteAddress != NULL) {
				_SC_sockaddr_to_string(targetPrivate->remoteAddress, buf, sizeof(buf));
				CFStringAppendFormat(str, NULL, CFSTR("%s%saddress = %s"),
						     targetPrivate->localAddress ? ", " : "",
						     (targetPrivate->type == reachabilityTypeAddressPair) ? "remote " : "",
						     buf);
			}
			break;
		}
		case reachabilityTypeName : {
			if ((targetPrivate->name != NULL)) {
				CFStringAppendFormat(str, NULL, CFSTR("name = %s"), targetPrivate->name);
			}
			if ((targetPrivate->serv != NULL)) {
				CFStringAppendFormat(str, NULL, CFSTR("%sserv = %s"),
						     targetPrivate->name != NULL ? ", " : "",
						     targetPrivate->serv);
			}
			break;
		}
	}

	return str;
}


CFStringRef
_SCNetworkReachabilityCopyTargetFlags(SCNetworkReachabilityRef target)
{
	CFAllocatorRef			allocator	= CFGetAllocator(target);
	CFStringRef			str;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	str = CFStringCreateWithFormat(allocator,
				       NULL,
				       CFSTR("flags = 0x%08x, if_index = %hu%s"),
				       targetPrivate->info.flags,
				       targetPrivate->info.if_index,
				       targetPrivate->info.sleeping ? ", z" : "");
	return str;
}


static CFStringRef
__SCNetworkReachabilityCopyDescription(CFTypeRef cf)
{
	CFAllocatorRef			allocator	= CFGetAllocator(cf);
	CFMutableStringRef		result;
	CFStringRef			str;
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)cf;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	result = CFStringCreateMutable(allocator, 0);
	CFStringAppendFormat(result, NULL, CFSTR("<SCNetworkReachability %p [%p]> {"), cf, allocator);

	// add target description
	str = _SCNetworkReachabilityCopyTargetDescription(target);
	CFStringAppend(result, str);
	CFRelease(str);

	// add additional "name" info
	if (targetPrivate->type == reachabilityTypeName) {
		if (targetPrivate->dnsMP != MACH_PORT_NULL) {
			CFStringAppendFormat(result, NULL, CFSTR(" (DNS query active)"));
		} else if (targetPrivate->dnsRetry != NULL) {
			CFStringAppendFormat(result, NULL, CFSTR(" (DNS retry queued)"));
		} else if ((targetPrivate->resolvedAddress != NULL) || (targetPrivate->resolvedAddressError != NETDB_SUCCESS)) {
			if (targetPrivate->resolvedAddress != NULL) {
				if (isA_CFArray(targetPrivate->resolvedAddress)) {
					CFIndex	i;
					CFIndex	n	= CFArrayGetCount(targetPrivate->resolvedAddress);

					CFStringAppendFormat(result, NULL, CFSTR(" ("));
					for (i = 0; i < n; i++) {
						CFDataRef	address;
						char		buf[64];
						struct sockaddr	*sa;

						address	= CFArrayGetValueAtIndex(targetPrivate->resolvedAddress, i);
						sa      = (struct sockaddr *)CFDataGetBytePtr(address);
						_SC_sockaddr_to_string(sa, buf, sizeof(buf));
						CFStringAppendFormat(result, NULL, CFSTR("%s%s"),
								     i > 0 ? ", " : "",
								     buf);
					}
				} else if (CFEqual(targetPrivate->resolvedAddress, kCFNull)) {
					CFStringAppendFormat(result, NULL, CFSTR(" (%s"),
							     gai_strerror(targetPrivate->resolvedAddressError));
				} else {
					CFStringAppendFormat(result, NULL, CFSTR(" (no addresses"));
				}
			} else {
				CFStringAppendFormat(result, NULL, CFSTR(" (%s"),
						     gai_strerror(targetPrivate->resolvedAddressError));
			}
			if (targetPrivate->llqActive) {
				CFStringAppendFormat(result, NULL, CFSTR("), DNS llq active"));
			} else {
				CFStringAppendFormat(result, NULL, CFSTR(")"));
			}
		} else if (targetPrivate->llqActive) {
			CFStringAppendFormat(result, NULL, CFSTR(" (DNS llq active)"));
		}
	}

	// add flags
	if (targetPrivate->scheduled) {
		str = _SCNetworkReachabilityCopyTargetFlags(target);
		CFStringAppendFormat(result, NULL, CFSTR(", %@"), str);
		CFRelease(str);
	}

	CFStringAppendFormat(result, NULL, CFSTR("}"));

	return result;
}


static void
__SCNetworkReachabilityDeallocate(CFTypeRef cf)
{
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)cf;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%srelease"),
	      targetPrivate->log_prefix);

#ifdef	HAVE_REACHABILITY_SERVER
	/* disconnect from the reachability server */

	if (targetPrivate->serverActive) {
		__SCNetworkReachabilityServer_targetRemove(target);
	}
#endif	// HAVE_REACHABILITY_SERVER

	/* release resources */

	pthread_mutex_destroy(&targetPrivate->lock);

	if (targetPrivate->name != NULL)
		CFAllocatorDeallocate(NULL, (void *)targetPrivate->name);

	if (targetPrivate->serv != NULL)
		CFAllocatorDeallocate(NULL, (void *)targetPrivate->serv);

	if (targetPrivate->resolvedAddress != NULL)
		CFRelease(targetPrivate->resolvedAddress);

	if (targetPrivate->localAddress != NULL)
		CFAllocatorDeallocate(NULL, (void *)targetPrivate->localAddress);

	if (targetPrivate->remoteAddress != NULL)
		CFAllocatorDeallocate(NULL, (void *)targetPrivate->remoteAddress);

	if (targetPrivate->rlsContext.release != NULL) {
		(*targetPrivate->rlsContext.release)(targetPrivate->rlsContext.info);
	}

	if (targetPrivate->onDemandName != NULL) {
		CFRelease(targetPrivate->onDemandName);
	}

	if (targetPrivate->onDemandRemoteAddress != NULL) {
		CFRelease(targetPrivate->onDemandRemoteAddress);
	}

	if (targetPrivate->onDemandServer != NULL) {
		CFRelease(targetPrivate->onDemandServer);
	}

	if (targetPrivate->onDemandServiceID != NULL) {
		CFRelease(targetPrivate->onDemandServiceID);
	}

#ifdef	HAVE_REACHABILITY_SERVER
	if (targetPrivate->serverDigest != NULL) {
		CFRelease(targetPrivate->serverDigest);
	}

	if (targetPrivate->serverGroup != NULL) {
		dispatch_release(targetPrivate->serverGroup);
	}

	if (targetPrivate->serverQueue != NULL) {
		dispatch_release(targetPrivate->serverQueue);
	}

	if (targetPrivate->serverWatchers != NULL) {
		CFRelease(targetPrivate->serverWatchers);
	}
#endif	// HAVE_REACHABILITY_SERVER

	return;
}


static void
__SCNetworkReachabilityInitialize(void)
{
	__kSCNetworkReachabilityTypeID = _CFRuntimeRegisterClass(&__SCNetworkReachabilityClass);

	// provide a way to enable SCNetworkReachability logging without
	// having to set _sc_debug=1.
	if (getenv("REACH_LOGGING") != NULL) {
		_sc_debug = TRUE;
	}

	// set per-process "bypass" of the SCNetworkReachability server
	if (getenv("LONG_LIVED_QUERY_BYPASS") != NULL) {
		D_llqBypass = TRUE;
	}

#ifdef	HAVE_REACHABILITY_SERVER
	// set per-process "bypass" of the SCNetworkReachability server
	if (getenv("REACH_SERVER_BYPASS") != NULL) {
		D_serverBypass = TRUE;
	}
#endif	// HAVE_REACHABILITY_SERVER

	pthread_mutexattr_init(&lock_attr);
	pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_ERRORCHECK);

	return;
}


__private_extern__
dispatch_queue_t
__SCNetworkReachability_concurrent_queue()
{
	static dispatch_once_t	once;
	static dispatch_queue_t	q;

	dispatch_once(&once, ^{
		q = dispatch_queue_create("SCNetworkReachabilty.concurrent",
					  DISPATCH_QUEUE_CONCURRENT);
		dispatch_queue_set_width(q, 32);
	});

	return q;
}


/*
 * __SCNetworkReachabilityPerformInlineNoLock
 *
 * Calls reachPerform()
 * - caller must be holding a reference to the target
 * - caller must *not* be holding the target lock
 * - caller must be running on the __SCNetworkReachability_concurrent_queue()
 */
static __inline__ void
__SCNetworkReachabilityPerformInlineNoLock(SCNetworkReachabilityRef target, Boolean needResolve)
{
	dispatch_queue_t		queue;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_LOCK(&targetPrivate->lock);

	if (needResolve) {
		// allow the DNS query to be [re-]started
		targetPrivate->needResolve = TRUE;
	}

	queue = targetPrivate->dispatchQueue;
	if (queue != NULL) {
		dispatch_group_t	group;

		dispatch_retain(queue);

		group = targetPrivate->dispatchGroup;
		dispatch_group_enter(group);

		MUTEX_UNLOCK(&targetPrivate->lock);

		dispatch_sync(queue, ^{
			reachPerform((void *)target);
			dispatch_group_leave(group);
			dispatch_release(queue);
		});
	} else {
		if (targetPrivate->rls != NULL) {
			CFRunLoopSourceSignal(targetPrivate->rls);
			_SC_signalRunLoop(target, targetPrivate->rls, targetPrivate->rlList);
		}

		MUTEX_UNLOCK(&targetPrivate->lock);
	}

	return;
}


#ifdef	HAVE_REACHABILITY_SERVER
/*
 * __SCNetworkReachabilityPerformNoLock
 *
 * Calls reachPerform()
 * - caller must *not* be holding the target lock
 * - caller must *not* running on the __SCNetworkReachability_concurrent_queue()
 */
__private_extern__
void
__SCNetworkReachabilityPerformNoLock(SCNetworkReachabilityRef target)
{
	CFRetain(target);
	dispatch_async(__SCNetworkReachability_concurrent_queue(), ^{
		__SCNetworkReachabilityPerformInlineNoLock(target, FALSE);
		CFRelease(target);
	});

	return;
}
#endif	// HAVE_REACHABILITY_SERVER


/*
 * __SCNetworkReachabilityPerformConcurrent
 *
 * Calls reachPerform()
 * - caller must be holding the target lock
 * - caller running on the __SCNetworkReachability_concurrent_queue()
 */
static __inline__ void
__SCNetworkReachabilityPerformConcurrent(SCNetworkReachabilityRef target)
{
	dispatch_queue_t		queue;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	queue = targetPrivate->dispatchQueue;
	if (queue != NULL) {
		dispatch_retain(queue);
		CFRetain(target);
		dispatch_group_async(targetPrivate->dispatchGroup, queue, ^{
			reachPerform((void *)target);
			CFRelease(target);
			dispatch_release(queue);
		});
	} else {
		if (targetPrivate->rls != NULL) {
			CFRunLoopSourceSignal(targetPrivate->rls);
			_SC_signalRunLoop(target, targetPrivate->rls, targetPrivate->rlList);
		}
	}

	return;
}


/*
 * __SCNetworkReachabilityPerform
 *
 * Calls reachPerform()
 * - caller must be holding the target lock
 * - caller not running on the __SCNetworkReachability_concurrent_queue()
 */
static void
__SCNetworkReachabilityPerform(SCNetworkReachabilityRef target)
{
	dispatch_queue_t		queue;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	queue = targetPrivate->dispatchQueue;
	if (queue != NULL) {
		dispatch_retain(queue);
		CFRetain(target);
		dispatch_group_async(targetPrivate->dispatchGroup, __SCNetworkReachability_concurrent_queue(), ^{
			dispatch_sync(queue, ^{
				reachPerform((void *)target);
				CFRelease(target);
				dispatch_release(queue);
			});
		});
	} else if (targetPrivate->rls != NULL) {
		CFRunLoopSourceSignal(targetPrivate->rls);
		_SC_signalRunLoop(target, targetPrivate->rls, targetPrivate->rlList);
	}

	return;
}


static SCNetworkReachabilityPrivateRef
__SCNetworkReachabilityCreatePrivate(CFAllocatorRef	allocator)
{
	SCNetworkReachabilityPrivateRef		targetPrivate;
	uint32_t				size;

	/* initialize runtime */
	pthread_once(&initialized, __SCNetworkReachabilityInitialize);

	/* allocate target */
	size          = sizeof(SCNetworkReachabilityPrivate) - sizeof(CFRuntimeBase);
	targetPrivate = (SCNetworkReachabilityPrivateRef)_CFRuntimeCreateInstance(allocator,
										  __kSCNetworkReachabilityTypeID,
										  size,
										  NULL);
	if (targetPrivate == NULL) {
		return NULL;
	}

	MUTEX_INIT(&targetPrivate->lock);

	targetPrivate->name				= NULL;
	targetPrivate->serv				= NULL;
	targetPrivate->hints				= HINTS_DEFAULT;
	targetPrivate->needResolve			= FALSE;
	targetPrivate->resolvedAddress			= NULL;
	targetPrivate->resolvedAddressError		= NETDB_SUCCESS;

	targetPrivate->if_index				= 0;

	targetPrivate->localAddress			= NULL;
	targetPrivate->remoteAddress			= NULL;

	targetPrivate->cycle				= 1;
	targetPrivate->info				= NOT_REACHABLE;
	targetPrivate->last_notify			= NOT_REPORTED;

	targetPrivate->scheduled			= FALSE;
	targetPrivate->rls				= NULL;
	targetPrivate->rlsFunction			= NULL;
	targetPrivate->rlsContext.info			= NULL;
	targetPrivate->rlsContext.retain		= NULL;
	targetPrivate->rlsContext.release		= NULL;
	targetPrivate->rlsContext.copyDescription	= NULL;
	targetPrivate->rlList				= NULL;

	targetPrivate->haveDNS				= FALSE;
	targetPrivate->dnsMP				= MACH_PORT_NULL;
	targetPrivate->dnsPort				= NULL;
	targetPrivate->dnsRLS				= NULL;
	targetPrivate->dnsSource			= NULL;
	targetPrivate->dnsQueryStart			= TIME_ZERO;
	targetPrivate->dnsQueryEnd			= TIME_ZERO;
	targetPrivate->dnsRetry				= NULL;
	targetPrivate->dnsRetryCount			= 0;

	targetPrivate->last_dns				= TIME_ZERO;
	targetPrivate->last_network			= TIME_ZERO;
#if	!TARGET_OS_IPHONE
	targetPrivate->last_power			= TIME_ZERO;
#endif	// !TARGET_OS_IPHONE
	targetPrivate->last_push			= TIME_ZERO;

	targetPrivate->onDemandBypass			= FALSE;
	targetPrivate->onDemandName			= NULL;
	targetPrivate->onDemandRemoteAddress		= NULL;
	targetPrivate->onDemandServer			= NULL;
	targetPrivate->onDemandServiceID		= NULL;


	targetPrivate->llqActive			= FALSE;
	targetPrivate->llqBypass			= D_llqBypass;
	targetPrivate->llqTarget			= NULL;
	targetPrivate->llqTimer				= NULL;

#ifdef	HAVE_REACHABILITY_SERVER
	targetPrivate->serverActive			= FALSE;
	targetPrivate->serverBypass			= D_serverBypass;
	targetPrivate->serverScheduled			= FALSE;
	targetPrivate->serverInfo			= NOT_REACHABLE;

	targetPrivate->serverDigest			= NULL;
	targetPrivate->serverGroup			= NULL;
	targetPrivate->serverInfoValid			= FALSE;
	targetPrivate->serverQueryActive		= 0;
	targetPrivate->serverQueue			= NULL;
	targetPrivate->serverReferences			= 0;
	targetPrivate->serverWatchers			= NULL;
#endif	// HAVE_REACHABILITY_SERVER

	targetPrivate->log_prefix[0] = '\0';
	if (_sc_log > 0) {
		snprintf(targetPrivate->log_prefix,
			 sizeof(targetPrivate->log_prefix),
			 "[%p] ",
			 targetPrivate);
	}

	return targetPrivate;
}




static const struct sockaddr *
is_valid_address(const struct sockaddr *address)
{
	const struct sockaddr	*valid	= NULL;
	static Boolean	warned	= FALSE;

	if ((address != NULL) &&
	    (address->sa_len <= sizeof(struct sockaddr_storage))) {
		switch (address->sa_family) {
			case AF_INET :
				if (address->sa_len >= sizeof(struct sockaddr_in)) {
					valid = address;
				} else {
					if (!warned) {
						SCLog(TRUE, LOG_ERR,
						      CFSTR("SCNetworkReachabilityCreateWithAddress[Pair] called with \"struct sockaddr *\" len %d < %d"),
						      address->sa_len,
						      sizeof(struct sockaddr_in));
						warned = TRUE;
					}
				}
				break;
			case AF_INET6 :
				if (address->sa_len >= sizeof(struct sockaddr_in6)) {
					valid = address;
				} else if (!warned) {
					SCLog(TRUE, LOG_ERR,
					      CFSTR("SCNetworkReachabilityCreateWithAddress[Pair] called with \"struct sockaddr *\" len %d < %d"),
					      address->sa_len,
					      sizeof(struct sockaddr_in6));
					warned = TRUE;
				}
				break;
			default :
				if (!warned) {
					SCLog(TRUE, LOG_ERR,
					      CFSTR("SCNetworkReachabilityCreateWithAddress[Pair] called with invalid address family %d"),
					      address->sa_family);
					warned = TRUE;
				}
		}
	}

	return valid;
}


SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithAddress(CFAllocatorRef		allocator,
				       const struct sockaddr	*address)
{
	SCNetworkReachabilityPrivateRef	targetPrivate;

	address = is_valid_address(address);
	if (address == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	targetPrivate = __SCNetworkReachabilityCreatePrivate(allocator);
	if (targetPrivate == NULL) {
		return NULL;
	}

	targetPrivate->type = reachabilityTypeAddress;
	targetPrivate->remoteAddress = CFAllocatorAllocate(NULL, address->sa_len, 0);
	bcopy(address, targetPrivate->remoteAddress, address->sa_len);

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%s%s %@"),
	      targetPrivate->log_prefix,
	      DEBUG_REACHABILITY_TYPE_ADDRESS,
	      targetPrivate);

	return (SCNetworkReachabilityRef)targetPrivate;
}


SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithAddressPair(CFAllocatorRef		allocator,
					   const struct sockaddr	*localAddress,
					   const struct sockaddr	*remoteAddress)
{
	SCNetworkReachabilityPrivateRef	targetPrivate;

	if ((localAddress == NULL) && (remoteAddress == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	if (localAddress != NULL) {
		localAddress = is_valid_address(localAddress);
		if (localAddress == NULL) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}

	if (remoteAddress != NULL) {
		remoteAddress = is_valid_address(remoteAddress);
		if (remoteAddress == NULL) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}

	targetPrivate = __SCNetworkReachabilityCreatePrivate(allocator);
	if (targetPrivate == NULL) {
		return NULL;
	}

	targetPrivate->type = reachabilityTypeAddressPair;

	if (localAddress != NULL) {
		targetPrivate->localAddress = CFAllocatorAllocate(NULL, localAddress->sa_len, 0);
		bcopy(localAddress, targetPrivate->localAddress, localAddress->sa_len);
	}

	if (remoteAddress != NULL) {
		targetPrivate->remoteAddress = CFAllocatorAllocate(NULL, remoteAddress->sa_len, 0);
		bcopy(remoteAddress, targetPrivate->remoteAddress, remoteAddress->sa_len);
	}

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%s%s %@"),
	      targetPrivate->log_prefix,
	      DEBUG_REACHABILITY_TYPE_ADDRESSPAIR,
	      targetPrivate);

	return (SCNetworkReachabilityRef)targetPrivate;
}


SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithName(CFAllocatorRef	allocator,
				    const char		*nodename)
{
	union {
		struct sockaddr		sa;
		struct sockaddr_in	sin;
		struct sockaddr_in6	sin6;
	} addr;
	int				nodenameLen;
	SCNetworkReachabilityPrivateRef	targetPrivate;

	if (nodename == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	nodenameLen = strlen(nodename);
	if (nodenameLen == 0) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	if (_SC_string_to_sockaddr(nodename, AF_UNSPEC, (void *)&addr, sizeof(addr)) != NULL) {
		/* if this "nodename" is really an IP[v6] address in disguise */
		return SCNetworkReachabilityCreateWithAddress(allocator, &addr.sa);
	}

	targetPrivate = __SCNetworkReachabilityCreatePrivate(allocator);
	if (targetPrivate == NULL) {
		return NULL;
	}

	targetPrivate->type = reachabilityTypeName;

	targetPrivate->name = CFAllocatorAllocate(NULL, nodenameLen + 1, 0);
	strlcpy((char *)targetPrivate->name, nodename, nodenameLen + 1);

	targetPrivate->needResolve = TRUE;
	targetPrivate->info.flags |= kSCNetworkReachabilityFlagsFirstResolvePending;
#ifdef	HAVE_REACHABILITY_SERVER
	targetPrivate->serverInfo.flags |= kSCNetworkReachabilityFlagsFirstResolvePending;
#endif	// HAVE_REACHABILITY_SERVER

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%s%s %@"),
	      targetPrivate->log_prefix,
	      DEBUG_REACHABILITY_TYPE_NAME,
	      targetPrivate);

	return (SCNetworkReachabilityRef)targetPrivate;
}




SCNetworkReachabilityRef
SCNetworkReachabilityCreateWithOptions(CFAllocatorRef	allocator,
				       CFDictionaryRef	options)
{
	const struct sockaddr		*addr_l		= NULL;
	const struct sockaddr		*addr_r		= NULL;
	CFDataRef			data;
	struct addrinfo			*hints		= NULL;
	CFStringRef			interface	= NULL;
	CFBooleanRef			llqBypass;
	CFStringRef			nodename;
	CFBooleanRef			onDemandBypass;
	CFBooleanRef			resolverBypass;
#ifdef	HAVE_REACHABILITY_SERVER
	CFBooleanRef			serverBypass;
#endif	// HAVE_REACHABILITY_SERVER
	CFStringRef			servname;
	SCNetworkReachabilityRef	target;
	SCNetworkReachabilityPrivateRef	targetPrivate;

	if (!isA_CFDictionary(options)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	nodename = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionNodeName);
	if ((nodename != NULL) &&
	    (!isA_CFString(nodename) || (CFStringGetLength(nodename) == 0))) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	servname = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionServName);
	if ((servname != NULL) &&
	    (!isA_CFString(servname) || (CFStringGetLength(servname) == 0))) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	data = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionLocalAddress);
	if (data != NULL) {
		if (!isA_CFData(data) || (CFDataGetLength(data) < sizeof(struct sockaddr_in))) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
		addr_l = (const struct sockaddr *)CFDataGetBytePtr(data);
	}
	data = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionRemoteAddress);
	if (data != NULL) {
		if (!isA_CFData(data) || (CFDataGetLength(data) < sizeof(struct sockaddr_in))) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
		addr_r = (const struct sockaddr *)CFDataGetBytePtr(data);
	}
	data = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionHints);
	if (data != NULL) {
		if (!isA_CFData(data) || (CFDataGetLength(data) != sizeof(targetPrivate->hints))) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}

		/* ALIGN: CF aligns to >8 byte boundries */
		hints = (struct addrinfo *)(void *)CFDataGetBytePtr(data);
		if ((hints->ai_addrlen   != 0)    ||
		    (hints->ai_addr      != NULL) ||
		    (hints->ai_canonname != NULL) ||
		    (hints->ai_next      != NULL)) {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}
	interface = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionInterface);
	if ((interface != NULL) &&
	    (!isA_CFString(interface) || (CFStringGetLength(interface) == 0))) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	onDemandBypass = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionConnectionOnDemandBypass);
	if ((onDemandBypass != NULL) && !isA_CFBoolean(onDemandBypass)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
	resolverBypass = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionResolverBypass);
	if ((resolverBypass != NULL) && !isA_CFBoolean(resolverBypass)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}


	llqBypass = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionLongLivedQueryBypass);
	if ((llqBypass != NULL) && !isA_CFBoolean(llqBypass)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

#ifdef	HAVE_REACHABILITY_SERVER
	serverBypass = CFDictionaryGetValue(options, kSCNetworkReachabilityOptionServerBypass);
	if ((serverBypass != NULL) && !isA_CFBoolean(serverBypass)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}
#endif	// HAVE_REACHABILITY_SERVER

	if ((nodename != NULL) || (servname != NULL)) {
		const char	*name;

		if ((addr_l != NULL) || (addr_r != NULL)) {
			// can't have both a name/serv and an address
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}

		name = _SC_cfstring_to_cstring(nodename, NULL, 0, kCFStringEncodingUTF8);
		target = SCNetworkReachabilityCreateWithName(allocator, name);
		CFAllocatorDeallocate(NULL, (void *)name);
	} else {
		if ((addr_l != NULL) && (addr_r != NULL)) {
			target = SCNetworkReachabilityCreateWithAddressPair(NULL, addr_l, addr_r);
		} else if (addr_r != NULL) {
			target = SCNetworkReachabilityCreateWithAddress(NULL, addr_r);
		} else if (addr_l != NULL) {
			target = SCNetworkReachabilityCreateWithAddress(NULL, addr_l);
		} else {
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}
	if (target == NULL) {
		return NULL;
	}

	targetPrivate = (SCNetworkReachabilityPrivateRef)target;
	if (targetPrivate->type == reachabilityTypeName) {
		if (servname != NULL) {
			targetPrivate->serv = _SC_cfstring_to_cstring(servname, NULL, 0, kCFStringEncodingUTF8);
		}
		if (hints != NULL) {
			bcopy(hints, &targetPrivate->hints, sizeof(targetPrivate->hints));
		}
	}

	if (interface != NULL) {
		if ((_SC_cfstring_to_cstring(interface,
					     targetPrivate->if_name,
					     sizeof(targetPrivate->if_name),
					     kCFStringEncodingASCII) == NULL) ||
		    ((targetPrivate->if_index = if_nametoindex(targetPrivate->if_name)) == 0)) {
			CFRelease(targetPrivate);
			_SCErrorSet(kSCStatusInvalidArgument);
			return NULL;
		}
	}


	if (llqBypass != NULL) {
		targetPrivate->llqBypass = CFBooleanGetValue(llqBypass);
	}

	if (onDemandBypass != NULL) {
		targetPrivate->onDemandBypass = CFBooleanGetValue(onDemandBypass);
	}

	if (resolverBypass != NULL) {
		targetPrivate->resolverBypass = CFBooleanGetValue(resolverBypass);
	}

#ifdef	HAVE_REACHABILITY_SERVER
	if (serverBypass != NULL) {
		targetPrivate->serverBypass = CFBooleanGetValue(serverBypass);
	}
#endif	// HAVE_REACHABILITY_SERVER

	if (_sc_debug && (_sc_log > 0)) {
		const char	*opt;

		switch (targetPrivate->type) {
			case reachabilityTypeName :
				opt = DEBUG_REACHABILITY_TYPE_NAME_OPTIONS;
				break;
			case reachabilityTypeAddress :
				opt = DEBUG_REACHABILITY_TYPE_ADDRESS_OPTIONS;
				break;
			case reachabilityTypeAddressPair :
				opt = DEBUG_REACHABILITY_TYPE_ADDRESSPAIR_OPTIONS;
				break;
			default :
				opt = "???";
				break;
		}

		SCLog(TRUE, LOG_INFO, CFSTR("%s%s %@"),
		      targetPrivate->log_prefix,
		      opt,
		      targetPrivate);
	}

	return (SCNetworkReachabilityRef)targetPrivate;
}


CFTypeID
SCNetworkReachabilityGetTypeID(void)
{
	pthread_once(&initialized, __SCNetworkReachabilityInitialize);	/* initialize runtime */
	return __kSCNetworkReachabilityTypeID;
}


CFArrayRef	/* CFArray[CFData], where each CFData is a (struct sockaddr *) */
SCNetworkReachabilityCopyResolvedAddress(SCNetworkReachabilityRef	target,
					 int				*error_num)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
	       return NULL;
	}

	if (targetPrivate->type != reachabilityTypeName) {
		_SCErrorSet(kSCStatusInvalidArgument);
	       return NULL;
	}

	if (error_num) {
		*error_num = targetPrivate->resolvedAddressError;
	}

	if (targetPrivate->resolvedAddress != NULL) {
		if (isA_CFArray(targetPrivate->resolvedAddress)) {
			return CFRetain(targetPrivate->resolvedAddress);
		} else {
			/* if status is known but no resolved addresses to return */
			_SCErrorSet(kSCStatusOK);
			return NULL;
		}
	}

	_SCErrorSet(kSCStatusReachabilityUnknown);
	return NULL;
}


static void
__SCNetworkReachabilitySetResolvedAddress(int32_t			status,
					  struct addrinfo		*res,
					  SCNetworkReachabilityRef	target)
{
	struct addrinfo				*resP;
	SCNetworkReachabilityPrivateRef		targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	if (targetPrivate->resolvedAddress != NULL) {
		CFRelease(targetPrivate->resolvedAddress);
		targetPrivate->resolvedAddress = NULL;
	}

	if ((status == 0) && (res != NULL)) {
		CFMutableArrayRef	addresses;

		addresses = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

		for (resP = res; resP; resP = resP->ai_next) {
			CFIndex		n;
			CFDataRef	newAddress;

			newAddress = CFDataCreate(NULL, (void *)resP->ai_addr, resP->ai_addr->sa_len);
			n = CFArrayGetCount(addresses);
			if ((n == 0) ||
			    !CFArrayContainsValue(addresses, CFRangeMake(0, n), newAddress)) {
				CFArrayAppendValue(addresses, newAddress);
			}
			CFRelease(newAddress);
		}

		/* save the resolved address[es] */
		targetPrivate->resolvedAddress      = addresses;
		targetPrivate->resolvedAddressError = NETDB_SUCCESS;
	} else {
		SCLog(_sc_debug, LOG_INFO, CFSTR("%sgetaddrinfo() failed: %s"),
		      targetPrivate->log_prefix,
		      gai_strerror(status));

		/* save the error associated with the attempt to resolve the name */
		targetPrivate->resolvedAddress      = CFRetain(kCFNull);
		targetPrivate->resolvedAddressError = status;
	}
	targetPrivate->needResolve = FALSE;

	if (res != NULL)	freeaddrinfo(res);

	if (targetPrivate->scheduled) {
		__SCNetworkReachabilityPerform(target);
	}

	return;
}


static void
__SCNetworkReachabilityCallbackSetResolvedAddress(int32_t status, struct addrinfo *res, void *context)
{
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)context;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	__dns_query_end(target,
			((status == 0) && (res != NULL)),	// if successful query
			dns_query_async,			// async
			&targetPrivate->dnsQueryStart,		// start time
			&targetPrivate->dnsQueryEnd);		// end time

	__SCNetworkReachabilitySetResolvedAddress(status, res, target);
	return;
}


/*
 * rankReachability()
 *   Not reachable       == 0
 *   Connection Required == 1
 *   Reachable           == 2
 */
static int
rankReachability(SCNetworkReachabilityFlags flags)
{
	int	rank = 0;

	if (flags & kSCNetworkReachabilityFlagsReachable)		rank = 2;
	if (flags & kSCNetworkReachabilityFlagsConnectionRequired)	rank = 1;
	return rank;
}


#pragma mark -
#pragma mark DNS name resolution


static CFStringRef
replyMPCopyDescription(const void *info)
{
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)info;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	return CFStringCreateWithFormat(NULL,
					NULL,
					CFSTR("<getaddrinfo_async_start reply MP> {%s%s%s%s%s, target = %p}"),
					targetPrivate->name != NULL ? "name = " : "",
					targetPrivate->name != NULL ? targetPrivate->name : "",
					targetPrivate->name != NULL && targetPrivate->serv != NULL ? ", " : "",
					targetPrivate->serv != NULL ? "serv = " : "",
					targetPrivate->serv != NULL ? targetPrivate->serv : "",
					target);
}


static void
getaddrinfo_async_handleCFReply(CFMachPortRef port, void *msg, CFIndex size, void *info);


static Boolean
enqueueAsyncDNSQuery_dispatch(SCNetworkReachabilityRef target)
{
	mach_port_t			mp;
	dispatch_source_t		source;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	mp = targetPrivate->dnsMP;

	// mach_port context <-- NULL (no msg received)
	mach_port_set_context(mach_task_self(), mp, (mach_vm_address_t)(uintptr_t)NULL);

	// create dispatch source to handle DNS reply
	source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV,
					mp,
					0,
					__SCNetworkReachability_concurrent_queue());
	if (source == NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("SCNetworkReachability dispatch_source_create() failed"));
		return FALSE;
	}

	//
	// We created the dispatch_source to listen for (and process) the mach IPC
	// reply to our async DNS query.  Because the source handler runs asychronously
	// we need to ensure that we're holding a reference to the target. Here, we take
	// a reference and setup the dispatch_source finalizer to drop it.
	//
	CFRetain(target);
	dispatch_set_context(source, (void *)target);
	dispatch_set_finalizer_f(source, (dispatch_function_t)CFRelease);

	dispatch_source_set_event_handler(source, ^{
		mach_msg_size_t			msg_size	= 8192;
		const mach_msg_options_t	options		= MACH_RCV_MSG
								| MACH_RCV_LARGE
								| MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_CTX)
								| MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0);

		while (TRUE) {
			kern_return_t		kr;
			mach_msg_header_t	*msg	= (mach_msg_header_t *)malloc(msg_size);

			kr = mach_msg(msg,			/* msg */
				      options,			/* options */
				      0,			/* send_size */
				      msg_size,			/* rcv_size */
				      mp,			/* rcv_name */
				      MACH_MSG_TIMEOUT_NONE,	/* timeout */
				      MACH_PORT_NULL);		/* notify */
			if (kr == KERN_SUCCESS) {
				// mach_port context <-- msg
				mach_port_set_context(mach_task_self(),
						      mp,
						      (mach_vm_address_t)(uintptr_t)msg);
			} else if (kr == MACH_RCV_TOO_LARGE) {
				msg_size *= 2;
				free(msg);
				continue;
			} else {
				SCLog(TRUE, LOG_ERR,
				      CFSTR("SCNetworkReachability async DNS handler, kr=0x%x"),
				      kr);
				free(msg);
			}
			break;
		}

		dispatch_source_cancel(source);
	});

	dispatch_source_set_cancel_handler(source, ^{
#if	!TARGET_OS_EMBEDDED
		mach_vm_address_t	context;
#else	// !TARGET_OS_EMBEDDED
		mach_port_context_t	context;
#endif	// !TARGET_OS_EMBEDDED
		kern_return_t		kr;
		mach_port_t		mp;

		// get the [async DNS query] mach port
		mp = (mach_port_t)dispatch_source_get_handle(source);

		// check if we have a received message
		kr = mach_port_get_context(mach_task_self(), mp, &context);
		if (kr == KERN_SUCCESS) {
			void	*msg;

			msg = (void *)(uintptr_t)context;
			if (msg != NULL) {
				MUTEX_LOCK(&targetPrivate->lock);
				getaddrinfo_async_handle_reply(msg);
				targetPrivate->dnsSource = NULL;
				targetPrivate->dnsMP = MACH_PORT_NULL;
				MUTEX_UNLOCK(&targetPrivate->lock);
				free(msg);
			} else {
				getaddrinfo_async_cancel(mp);
			}
		}

		dispatch_release(source);
	});

	targetPrivate->dnsSource = source;
	dispatch_resume(source);

	return TRUE;
}


static Boolean
enqueueAsyncDNSQuery_CF(SCNetworkReachabilityRef target)
{
	CFMachPortContext		context	= { 0
						  , (void *)target
						  , CFRetain
						  , CFRelease
						  , replyMPCopyDescription
						  };
	CFIndex				i;
	mach_port_t			mp;
	CFIndex				n;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	mp = targetPrivate->dnsMP;

	targetPrivate->dnsPort = _SC_CFMachPortCreateWithPort("SCNetworkReachability",
							      mp,
							      getaddrinfo_async_handleCFReply,
							      &context);
	if (targetPrivate->dnsPort == NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("SCNetworkReachability CFMachPortCreateWithPort() failed"));
		goto fail;
	}

	targetPrivate->dnsRLS = CFMachPortCreateRunLoopSource(NULL, targetPrivate->dnsPort, 0);
	if (targetPrivate->dnsRLS == NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("SCNetworkReachability CFMachPortCreateRunLoopSource() failed"));
		goto fail;
	}

	n = CFArrayGetCount(targetPrivate->rlList);
	for (i = 0; i < n; i += 3) {
		CFRunLoopRef	rl	= (CFRunLoopRef)CFArrayGetValueAtIndex(targetPrivate->rlList, i+1);
		CFStringRef	rlMode	= (CFStringRef) CFArrayGetValueAtIndex(targetPrivate->rlList, i+2);

		CFRunLoopAddSource(rl, targetPrivate->dnsRLS, rlMode);
	}

	return TRUE;

    fail :

	if (targetPrivate->dnsRLS != NULL) {
		CFRunLoopSourceInvalidate(targetPrivate->dnsRLS);
		CFRelease(targetPrivate->dnsRLS);
		targetPrivate->dnsRLS = NULL;
	}
	if (targetPrivate->dnsPort != NULL) {
		CFMachPortInvalidate(targetPrivate->dnsPort);
		CFRelease(targetPrivate->dnsPort);
		targetPrivate->dnsPort = NULL;
	}

	return FALSE;
}


static Boolean
enqueueAsyncDNSQuery(SCNetworkReachabilityRef target, mach_port_t mp)
{
	Boolean				ok		= FALSE;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	targetPrivate->dnsMP = mp;

	if (targetPrivate->dispatchQueue != NULL) {
		ok = enqueueAsyncDNSQuery_dispatch(target);
	} else if (targetPrivate->rls != NULL) {
		ok = enqueueAsyncDNSQuery_CF(target);
	}

	if (!ok) {
		targetPrivate->dnsMP = MACH_PORT_NULL;
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	return TRUE;
}


static void
dequeueAsyncDNSQuery(SCNetworkReachabilityRef target, Boolean cancel)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	if (targetPrivate->dnsPort != NULL) {
		CFMachPortInvalidate(targetPrivate->dnsPort);
		CFRelease(targetPrivate->dnsPort);
		targetPrivate->dnsPort = NULL;
	}

	if (targetPrivate->dnsRLS != NULL) {
		CFRelease(targetPrivate->dnsRLS);
		targetPrivate->dnsRLS = NULL;
	}

	if (targetPrivate->dnsSource != NULL) {
		dispatch_source_cancel(targetPrivate->dnsSource);
		targetPrivate->dnsSource = NULL;
		cancel = FALSE;		// the cancellation handler does the work
	}

	if (targetPrivate->dnsMP != MACH_PORT_NULL) {
		if (cancel) {
			getaddrinfo_async_cancel(targetPrivate->dnsMP);
		}
		targetPrivate->dnsMP = MACH_PORT_NULL;
	}

	return;
}


static void
getaddrinfo_async_handleCFReply(CFMachPortRef port, void *msg, CFIndex size, void *info)
{
	mach_port_t			mp		= CFMachPortGetPort(port);
	int32_t				status;
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)info;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_LOCK(&targetPrivate->lock);

	if (mp != targetPrivate->dnsMP) {
		// we've received a callback on the async DNS port but since the
		// associated CFMachPort doesn't match than the request must have
		// already been cancelled.
		SCLog(TRUE, LOG_ERR, CFSTR("processAsyncDNSReply(): mp != targetPrivate->dnsMP"));
		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	}

	dequeueAsyncDNSQuery(target, FALSE);
	status = getaddrinfo_async_handle_reply(msg);
	if ((status == 0) &&
	    (targetPrivate->resolvedAddress == NULL) && (targetPrivate->resolvedAddressError == NETDB_SUCCESS)) {
		Boolean	ok;

		// if the request is not complete and needs to be re-queued
		ok = enqueueAsyncDNSQuery(target, mp);
		if (!ok) {
			SCLog(TRUE, LOG_ERR, CFSTR("processAsyncDNSReply enqueueAsyncDNSQuery() failed"));
		}
	}

	MUTEX_UNLOCK(&targetPrivate->lock);

	return;
}


static Boolean
check_resolver_reachability(ReachabilityStoreInfoRef	store_info,
			    dns_resolver_t		*resolver,
			    SCNetworkReachabilityFlags	*flags,
			    Boolean			*haveDNS,
			    uint32_t			*resolver_if_index,
			    const char			*log_prefix)
{
	Boolean		ok	= TRUE;

	if (resolver_if_index) *resolver_if_index = 0;

	if (resolver->n_nameserver > 0) {
#if	!TARGET_IPHONE_SIMULATOR
		*flags   = (SCNetworkReachabilityFlags)resolver->reach_flags;
		if (resolver_if_index != NULL) {
			*resolver_if_index = resolver->if_index;
		}
#else	// !TARGET_IPHONE_SIMULATOR
		int	i;

		*flags = kSCNetworkReachabilityFlagsReachable;

		for (i = 0; i < resolver->n_nameserver; i++) {
			struct sockaddr		*address	= resolver->nameserver[i];
			ReachabilityInfo	ns_info;

			ok = checkAddress(store_info, address, resolver->if_index, &ns_info, log_prefix);
			if (!ok) {
				/* not today */
				break;
			}

			if ((i == 0) ||
			    (rankReachability(ns_info.flags) < rankReachability(*flags))) {
				/* return the worst case result */
				*flags = ns_info.flags;
				if (resolver_if_index != NULL) {
					*resolver_if_index = ns_info.if_index;
				}
			}
		}
#endif	// !TARGET_IPHONE_SIMULATOR
		*haveDNS = TRUE;
	} else {
		*flags   = kSCNetworkReachabilityFlagsReachable;
		*haveDNS = FALSE;
	}

	return ok;
}


static Boolean
check_matching_resolvers(ReachabilityStoreInfoRef	store_info,
			 dns_config_t			*dns_config,
			 const char			*fqdn,
			 unsigned int			if_index,
			 SCNetworkReachabilityFlags	*flags,
			 Boolean			*haveDNS,
			 uint32_t			*resolver_if_index,
			 int				*dns_config_index,
			 const char			*log_prefix)
{
	int		i;
	Boolean		matched		= FALSE;
	const char	*name		= fqdn;
	int32_t		n_resolvers;
	dns_resolver_t	**resolvers;

	if (if_index == 0) {
		n_resolvers = dns_config->n_resolver;
		resolvers   = dns_config->resolver;
	} else {
		n_resolvers = dns_config->n_scoped_resolver;
		resolvers   = dns_config->scoped_resolver;
	}

	/* In case we couldn't find a match, setting an index of -1
	   and resolver_if_index 0 */
	if (dns_config_index != NULL) *dns_config_index = -1;
	if (resolver_if_index != NULL) *resolver_if_index = 0;

	while (!matched && (name != NULL)) {
		int	len;

		/*
		 * check if the provided name (or sub-component)
		 * matches one of our resolver configurations.
		 */
		len = strlen(name);
		for (i = 0; i < n_resolvers; i++) {
			char		*domain;
			dns_resolver_t	*resolver;

			resolver = resolvers[i];
			if ((if_index != 0) && (if_index != resolver->if_index)) {
				continue;
			}

			domain   = resolver->domain;
			if (domain != NULL && (len == strlen(domain))) {
				if (strcasecmp(name, domain) == 0) {
					Boolean	ok;

					/*
					 * if name matches domain
					 */
					matched = TRUE;
					ok = check_resolver_reachability(store_info, resolver, flags, haveDNS,
									 resolver_if_index, log_prefix);
					if (!ok) {
						/* not today */
						return FALSE;
					}
					if (dns_config_index != NULL) *dns_config_index = i;
				}
			}
		}

		if (!matched) {
			/*
			 * we have not found a matching resolver, try
			 * a less qualified domain
			 */
			name = strchr(name, '.');
			if ((name != NULL) && (*name != '\0')) {
				name++;
			} else {
				name = NULL;
			}
		}
	}

	return matched;
}


static dns_resolver_t *
get_default_resolver(dns_config_t *dns_config, unsigned int if_index)
{
	int		i;
	int32_t		n_resolvers;
	dns_resolver_t	*resolver	= NULL;
	dns_resolver_t	**resolvers;

	if (if_index == 0) {
		n_resolvers = dns_config->n_resolver;
		resolvers   = dns_config->resolver;
	} else {
		n_resolvers = dns_config->n_scoped_resolver;
		resolvers   = dns_config->scoped_resolver;
	}

	for (i = 0; i < n_resolvers; i++) {
		if ((if_index != 0) && (if_index != resolvers[i]->if_index)) {
			continue;
		}

		if (((if_index == 0) && (i == 0)) ||
		    ((if_index != 0) && (resolver == NULL))) {
			// if this is the first (aka default) resolver
			resolver = resolvers[i];
		} else if ((resolvers[i]->domain == NULL) &&
			   (resolvers[i]->search_order < resolver->search_order)) {
			// if this is a default resolver with a lower search order
			resolver = resolvers[i];
		}
	}

	return resolver;
}


static dns_configuration_t *
dns_configuration_retain()
{
	pthread_mutex_lock(&dns_lock);

	if ((dns_configuration != NULL) && dns_token_valid) {
		int		check	= 0;
		uint32_t	status;

		/*
		 * check if the global [DNS] configuration snapshot needs
		 * to be updated
		 */
		status = notify_check(dns_token, &check);
		if (status != NOTIFY_STATUS_OK) {
			SCLog(TRUE, LOG_INFO, CFSTR("notify_check() failed, status=%lu"), status);
		}

		if ((status != NOTIFY_STATUS_OK) || (check != 0)) {
			/*
			 * if the snapshot needs to be refreshed
			 */
			if (dns_configuration->refs == 0) {
				dns_configuration_free(dns_configuration->config);
				CFAllocatorDeallocate(NULL, dns_configuration);
			}
			dns_configuration = NULL;
		}
	}

	if (dns_configuration == NULL) {
		dns_config_t	*new_config;

		new_config = dns_configuration_copy();
		if (new_config != NULL) {
			dns_configuration = CFAllocatorAllocate(NULL, sizeof(dns_configuration_t), 0);
			dns_configuration->config = new_config;
			dns_configuration->refs   = 0;
		}
	}

	if (dns_configuration != NULL) {
		dns_configuration->refs++;
	}

	pthread_mutex_unlock(&dns_lock);
	return dns_configuration;
}


static void
dns_configuration_release(dns_configuration_t *config)
{
	pthread_mutex_lock(&dns_lock);

	config->refs--;
	if (config->refs == 0) {
		if ((dns_configuration != config)) {
			dns_configuration_free(config->config);
			CFAllocatorDeallocate(NULL, config);
		}
	}

	pthread_mutex_unlock(&dns_lock);
	return;
}


static Boolean
dns_configuration_watch()
{
	int		dns_check	= 0;
	const char	*dns_key;
	Boolean		ok		= FALSE;
	uint32_t	status;

	pthread_mutex_lock(&dns_lock);

	dns_key = dns_configuration_notify_key();
	if (dns_key == NULL) {
		SCLog(TRUE, LOG_INFO, CFSTR("dns_configuration_notify_key() failed"));
		goto done;
	}

	status = notify_register_check(dns_key, &dns_token);
	if (status == NOTIFY_STATUS_OK) {
		dns_token_valid = TRUE;
	} else {
		SCLog(TRUE, LOG_INFO, CFSTR("notify_register_check() failed, status=%lu"), status);
		goto done;
	}

	status = notify_check(dns_token, &dns_check);
	if (status != NOTIFY_STATUS_OK) {
		SCLog(TRUE, LOG_INFO, CFSTR("notify_check() failed, status=%lu"), status);
		(void)notify_cancel(dns_token);
		dns_token_valid = FALSE;
		goto done;
	}

	ok = TRUE;

    done :

	pthread_mutex_unlock(&dns_lock);
	return ok;
}


static void
dns_configuration_unwatch()
{
	pthread_mutex_lock(&dns_lock);

	(void)notify_cancel(dns_token);
	dns_token_valid = FALSE;

	if ((dns_configuration != NULL) && (dns_configuration->refs == 0)) {
		dns_configuration_free(dns_configuration->config);
		CFAllocatorDeallocate(NULL, dns_configuration);
		dns_configuration = NULL;
	}

	pthread_mutex_unlock(&dns_lock);
	return;
}


static Boolean
_SC_R_checkResolverReachability(ReachabilityStoreInfoRef	store_info,
				SCNetworkReachabilityFlags	*flags,
				Boolean				*haveDNS,
				const char			*nodename,
				const char			*servname,
				unsigned int			if_index,
				uint32_t			*resolver_if_index,
				int				*dns_config_index,
				const char			*log_prefix
				)
{
	dns_resolver_t		*default_resolver;
	dns_configuration_t	*dns;
	Boolean			found			= FALSE;
	char			*fqdn			= (char *)nodename;
	int			i;
	Boolean			isFQDN			= FALSE;
	uint32_t		len;
	int			ndots			= 1;
	Boolean			ok			= TRUE;
	Boolean			useDefault		= FALSE;

	if (resolver_if_index) *resolver_if_index = 0;
	if (dns_config_index) *dns_config_index = -1;

	/*
	 * We first assume that all of the configured DNS servers
	 * are available.  Since we don't know which name server will
	 * be consulted to resolve the specified nodename we need to
	 * check the availability of ALL name servers.  We can only
	 * proceed if we know that our query can be answered.
	 */

	*flags   = kSCNetworkReachabilityFlagsReachable;
	*haveDNS = FALSE;

	len = (nodename != NULL) ? strlen(nodename) : 0;
	if (len == 0) {
		if ((servname == NULL) || (strlen(servname) == 0)) {
			// if no nodename or servname, return not reachable
			*flags = 0;
		}
		return ok;
	}

	dns = dns_configuration_retain();
	if (dns == NULL) {
		// if error
		SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS: no configuration"), log_prefix);
		goto done;
	}

	if (dns->config->n_resolver == 0) {
		// if no resolver configuration
		SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS: no resolvers"), log_prefix);
		goto done;
	}

	if (fqdn[len - 1] == '.') {
		isFQDN = TRUE;

		// trim trailing '.''s
		while ((len > 0) && (fqdn[len-1] == '.')) {
			if (fqdn == nodename) {
				fqdn = strdup(nodename);
			}
			fqdn[--len] = '\0';
		}
	}

	default_resolver = get_default_resolver(dns->config, if_index);

	/*
	 * check if the provided name matches a supplemental domain
	 */
	found = check_matching_resolvers(store_info, dns->config, fqdn, if_index,
					 flags, haveDNS, resolver_if_index,
					 dns_config_index, log_prefix);

	if (!found && !isFQDN) {
		/*
		 * if we did not match a supplemental domain name and if the
		 * provided name has enough "."s then the first query will be
		 * directed to the default resolver.
		 */
		char	*cp;
		int	dots;

#define	NDOTS_OPT	"ndots="
#define	NDOTS_OPT_LEN	(sizeof("ndots=") - 1)

		if (default_resolver->options != NULL) {
			cp = strstr(default_resolver->options, NDOTS_OPT);
			if ((cp != NULL) &&
			    ((cp == default_resolver->options) || isspace(cp[-1])) &&
			    ((cp[NDOTS_OPT_LEN] != '\0') && isdigit(cp[NDOTS_OPT_LEN]))) {
				char    *end;
				long    val;

				cp +=  NDOTS_OPT_LEN;
				errno = 0;
				val = strtol(cp, &end, 10);
				if ((*cp != '\0') && (cp != end) && (errno == 0) &&
				((*end == '\0') || isspace(*end))) {
					ndots = val;
				}
			}
		}

		dots = 0;
		for (cp = fqdn; *cp != '\0'; cp++) {
			if (*cp == '.') dots++;
		}

		if (dots > ndots) {
			useDefault = TRUE;
		}
	}

	if (!found && !isFQDN && !useDefault && (dns->config->n_resolver > 1)) {
		/*
		 * FQDN not specified, try matching w/search domains
		 */
		if (default_resolver->n_search > 0) {
			for (i = 0; !found && (i < default_resolver->n_search); i++) {
				int	ret;
				char	*search_fqdn	= NULL;

				ret = asprintf(&search_fqdn, "%s.%s", fqdn, default_resolver->search[i]);
				if (ret == -1) {
					continue;
				}

				// try the provided name with the search domain appended
				found = check_matching_resolvers(store_info,
								 dns->config,
								 search_fqdn,
								 if_index,
								 flags,
								 haveDNS,
								 resolver_if_index,
								 dns_config_index,
								 log_prefix);
				free(search_fqdn);
			}
		} else if (default_resolver->domain != NULL) {
			char	*dp;
			int	domain_parts	= 0;

			// count domain parts
			for (dp = default_resolver->domain; *dp != '\0'; dp++) {
				if (*dp == '.') {
					domain_parts++;
				}
			}

			// remove trailing dots
			for (dp--; (dp >= default_resolver->domain) && (*dp == '.'); dp--) {
				*dp = '\0';
				domain_parts--;
			}

			if (dp >= default_resolver->domain) {
				// dots are separators, bump # of components
				domain_parts++;
			}

			dp = default_resolver->domain;
			for (i = LOCALDOMAINPARTS; !found && (i <= (domain_parts - ndots)); i++) {
				int	ret;
				char	*search_fqdn	= NULL;

				ret = asprintf(&search_fqdn, "%s.%s", fqdn, dp);
				if (ret == -1) {
					continue;
				}

				// try the provided name with the [default] domain appended
				found = check_matching_resolvers(store_info,
								 dns->config,
								 search_fqdn,
								 if_index,
								 flags,
								 haveDNS,
								 resolver_if_index,
								 dns_config_index,
								 log_prefix);
				free(search_fqdn);

				// move to the next component of the [default] domain
				dp = strchr(dp, '.') + 1;
			}
		}
	}

	if (!found) {
		/*
		 * check the reachability of the default resolver
		 */
		ok = check_resolver_reachability(store_info, default_resolver, flags, haveDNS,
						 resolver_if_index, log_prefix);
		if (ok && dns_config_index != NULL) *dns_config_index = 0;
	}

	if (fqdn != nodename)	free(fqdn);

    done :

	if (dns != NULL) {
		dns_configuration_release(dns);
	}

	return ok;
}


Boolean
_SC_checkResolverReachability(SCDynamicStoreRef			*storeP,
			      SCNetworkReachabilityFlags	*flags,
			      Boolean				*haveDNS,
			      const char			*nodename,
			      const char			*servname)
{
	Boolean			ok;
	ReachabilityStoreInfo	store_info;

	ReachabilityStoreInfo_init(&store_info);
	ok = ReachabilityStoreInfo_update(&store_info, storeP, AF_UNSPEC);
	if (!ok) {
		goto done;
	}

	ok = _SC_R_checkResolverReachability(&store_info, flags, haveDNS, nodename,
					     servname, 0, NULL, NULL, "");

    done :

	ReachabilityStoreInfo_free(&store_info);
	return ok;
}

Boolean
__SC_checkResolverReachabilityInternal(SCDynamicStoreRef		*storeP,
				       SCNetworkReachabilityFlags	*flags,
				       Boolean				*haveDNS,
				       const char			*nodename,
				       const char			*servname,
				       uint32_t				*resolver_if_index,
				       int				*dns_config_index)
{
	Boolean			ok;
	ReachabilityStoreInfo	store_info;

	ReachabilityStoreInfo_init(&store_info);
	ok = ReachabilityStoreInfo_update(&store_info, storeP, AF_UNSPEC);
	if (!ok) {
		goto done;
	}

	ok = _SC_R_checkResolverReachability(&store_info, flags, haveDNS, nodename,
					     servname, 0, resolver_if_index, dns_config_index, "");

    done :

	ReachabilityStoreInfo_free(&store_info);
	return ok;
}

/*
 * _SC_checkResolverReachabilityByAddress()
 *
 * Given an IP address, determine whether a reverse DNS query can be issued
 * using the current network configuration.
 */
Boolean
_SC_checkResolverReachabilityByAddress(SCDynamicStoreRef		*storeP,
				       SCNetworkReachabilityFlags	*flags,
				       Boolean				*haveDNS,
				       struct sockaddr			*sa)
{
	int			i;
	Boolean			ok;
	char			ptr_name[128];
	ReachabilityStoreInfo	store_info;

	ReachabilityStoreInfo_init(&store_info);
	ok = ReachabilityStoreInfo_update(&store_info, storeP, AF_UNSPEC);
	if (!ok) {
		goto done;
	}

	/*
	 * Ideally, we would have an API that given a local IP
	 * address would return the DNS server(s) that would field
	 * a given PTR query.  Fortunately, we do have an SPI which
	 * which will provide this information given a "name" so we
	 * take the address, convert it into the inverse query name,
	 * and find out which servers should be consulted.
	 */

	switch (sa->sa_family) {
		case AF_INET : {
			union {
				in_addr_t	s_addr;
				unsigned char	b[4];
			} rev;
			/* ALIGN: assuming sa is aligned, then cast ok. */
			struct sockaddr_in	*sin	= (struct sockaddr_in *)(void *)sa;

			/*
			 * build "PTR" query name
			 *   NNN.NNN.NNN.NNN.in-addr.arpa.
			 */
			rev.s_addr = sin->sin_addr.s_addr;
			(void) snprintf(ptr_name, sizeof(ptr_name), "%u.%u.%u.%u.in-addr.arpa.",
					rev.b[3],
					rev.b[2],
					rev.b[1],
					rev.b[0]);

			break;
		}

		case AF_INET6 : {
			int			s	= 0;
			/* ALIGN: assume sa is aligned, cast ok. */
			struct sockaddr_in6	*sin6	= (struct sockaddr_in6 *)(void *)sa;
			int			x	= sizeof(ptr_name);
			int			n;

			/*
			 * build IPv6 "nibble" PTR query name (RFC 1886, RFC 3152)
			 *   N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.N.ip6.arpa.
			 */
			for (i = sizeof(sin6->sin6_addr) - 1; i >= 0; i--) {
				n = snprintf(&ptr_name[s], x, "%x.%x.",
					     ( sin6->sin6_addr.s6_addr[i]       & 0xf),
					     ((sin6->sin6_addr.s6_addr[i] >> 4) & 0xf));
				if ((n == -1) || (n >= x)) {
					goto done;
				}

				s += n;
				x -= n;
			}

			n = snprintf(&ptr_name[s], x, "ip6.arpa.");
			if ((n == -1) || (n >= x)) {
				goto done;
			}

			break;
		}

		default :
			goto done;
	}

	ok = _SC_R_checkResolverReachability(&store_info, flags, haveDNS, ptr_name, NULL, 0, NULL, NULL, "");

    done :

	ReachabilityStoreInfo_free(&store_info);
	return ok;
}


static Boolean
startAsyncDNSQuery(SCNetworkReachabilityRef target)
{
	int				error	= 0;
	mach_port_t			mp	= MACH_PORT_NULL;
	Boolean				ok;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	__dns_query_start(&targetPrivate->dnsQueryStart, &targetPrivate->dnsQueryEnd);

#ifdef	HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL
	if (targetPrivate->if_index == 0) {
#endif	/* HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL */
		error = getaddrinfo_async_start(&mp,
						targetPrivate->name,
						targetPrivate->serv,
						&targetPrivate->hints,
						__SCNetworkReachabilityCallbackSetResolvedAddress,
						(void *)target);
#ifdef	HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL
	} else {
		mp = _getaddrinfo_interface_async_call(targetPrivate->name,
						       targetPrivate->serv,
						       &targetPrivate->hints,
						       targetPrivate->if_name,
						       __SCNetworkReachabilityCallbackSetResolvedAddress,
						       (void *)target);
		if (mp == MACH_PORT_NULL) {
			error = EAI_SYSTEM;
		}
	}
#endif	/* HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL */
	if (error != 0) {
		/* save the error associated with the attempt to resolve the name */
		__SCNetworkReachabilityCallbackSetResolvedAddress(error, NULL, (void *)target);
		return FALSE;
	}

	ok = enqueueAsyncDNSQuery(target, mp);
	return ok;
}


#pragma mark -


static Boolean
enqueueAsyncDNSRetry(SCNetworkReachabilityRef	target)
{
	int64_t				delay;
	dispatch_source_t		source;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
					0,
					0,
					__SCNetworkReachability_concurrent_queue());
	if (source == NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("SCNetworkReachability retry dispatch_source_create() failed"));
		return FALSE;
	}

	// retain the target ... and release it when the [timer] source is released
	CFRetain(target);
	dispatch_set_context(source, (void *)target);
	dispatch_set_finalizer_f(source, (dispatch_function_t)CFRelease);

	dispatch_source_set_event_handler(source, ^(void) {
		__SCNetworkReachabilityPerformInlineNoLock(target, TRUE);
	});

	// start a one-shot timer
	delay = targetPrivate->dnsRetryCount * EAI_NONAME_RETRY_DELAY_USEC * NSEC_PER_USEC;
	dispatch_source_set_timer(source,
				  dispatch_time(DISPATCH_TIME_NOW, delay),	// start
				  0,						// interval
				  10 * NSEC_PER_MSEC);				// leeway

	targetPrivate->dnsRetry = source;
	dispatch_resume(source);

	return TRUE;
}


static void
dequeueAsyncDNSRetry(SCNetworkReachabilityRef	target)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	if (targetPrivate->dnsRetry != NULL) {
		dispatch_source_cancel(targetPrivate->dnsRetry);
		dispatch_release(targetPrivate->dnsRetry);
		targetPrivate->dnsRetry = NULL;
	}

	return;
}


#pragma mark -


static dispatch_queue_t
_llq_queue()
{
	static dispatch_once_t	once;
	static dispatch_queue_t	q;

	dispatch_once(&once, ^{
		q = dispatch_queue_create("SCNetworkReachabilty.longLivedQueries", NULL);
	});

	return q;
}


/*
 * _llq_notify
 *
 * Called to push out a target's DNS changes
 * - caller must be running on the _llq_queue()
 */
static void
_llq_notify(const void *value, void *context)
{
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)value;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_LOCK(&targetPrivate->lock);

	__dns_query_end(target,
			(targetPrivate->resolvedAddressError == NETDB_SUCCESS),	// if successful query
			dns_query_llq,						// long-lived-query
			&targetPrivate->dnsQueryStart,				// start time
			&targetPrivate->dnsQueryEnd);				// end time

	if (targetPrivate->scheduled) {
		__SCNetworkReachabilityPerform(target);
	}

	// last long-lived-query end time is new start time
	targetPrivate->dnsQueryStart = targetPrivate->dnsQueryEnd;

	MUTEX_UNLOCK(&targetPrivate->lock);
	return;
}


/*
 * _llq_callback
 *
 * Called to process mDNSResponder long-lived-query updates
 * - caller must be running on the _llq_queue()
 */
static void
_llq_callback(DNSServiceRef		sdRef,
	      DNSServiceFlags		flags,
	      uint32_t			interfaceIndex,
	      DNSServiceErrorType	errorCode,
	      const char		*hostname,
	      const struct sockaddr	*address,
	      uint32_t			ttl,
	      void			*context)
{
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)context;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_LOCK(&targetPrivate->lock);

	if (targetPrivate->llqTimer != NULL) {
		dispatch_source_cancel(targetPrivate->llqTimer);
		dispatch_release(targetPrivate->llqTimer);
		targetPrivate->llqTimer = NULL;
	}

	switch (errorCode) {
		case kDNSServiceErr_NoError :
			if (address != NULL) {
				CFMutableArrayRef	addresses;
				CFDataRef		llqAddress;

				if (targetPrivate->resolvedAddress != NULL) {
					if (isA_CFArray(targetPrivate->resolvedAddress)) {
						addresses = CFArrayCreateMutableCopy(NULL, 0, targetPrivate->resolvedAddress);
					} else {
						addresses = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
					}

					CFRelease(targetPrivate->resolvedAddress);
					targetPrivate->resolvedAddress = NULL;
				} else {
					addresses = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
				}

				llqAddress = CFDataCreate(NULL, (void *)address, address->sa_len);
				if (flags & kDNSServiceFlagsAdd) {
					// add address
					CFArrayAppendValue(addresses, llqAddress);
				} else {
					CFIndex	i;

					// remove address
					i = CFArrayGetFirstIndexOfValue(addresses,
									CFRangeMake(0, CFArrayGetCount(addresses)),
									llqAddress);
					if (i != kCFNotFound) {
						CFArrayRemoveValueAtIndex(addresses, i);
					}
				}
				CFRelease(llqAddress);

				if (CFArrayGetCount(addresses) > 0) {
					targetPrivate->resolvedAddress      = addresses;
					targetPrivate->resolvedAddressError = NETDB_SUCCESS;
				} else {
					// if host not found
					targetPrivate->resolvedAddress      = CFRetain(kCFNull);
					targetPrivate->resolvedAddressError = EAI_NONAME;
					CFRelease(addresses);
				}

				targetPrivate->needResolve = FALSE;
			}
			break;
		case kDNSServiceErr_NoSuchRecord :
			if (address != NULL) {
				// no IPv4/IPv6 address for name (NXDOMAIN)
				if (targetPrivate->resolvedAddress == NULL) {
					targetPrivate->resolvedAddress      = CFRetain(kCFNull);
					targetPrivate->resolvedAddressError = EAI_NONAME;
				}
				targetPrivate->needResolve = FALSE;
			}
			break;
		case kDNSServiceErr_Timeout :
			if (targetPrivate->resolvedAddress == NULL) {
				targetPrivate->resolvedAddress      = CFRetain(kCFNull);
				targetPrivate->resolvedAddressError = EAI_NONAME;
			}
			targetPrivate->needResolve = FALSE;
			break;
		default :
			SCLog(TRUE, LOG_ERR,
			      CFSTR("%sSCNetworkReachability _llq_callback w/error=%d"),
			      targetPrivate->log_prefix,
			      errorCode);
			break;
	}

	MUTEX_UNLOCK(&targetPrivate->lock);

	// the "more coming" flag applies to DNSService callouts for any/all
	// hosts that are being watched so we need to keep track of the targets
	// we have updated.  When we [finally] have the last callout then we
	// push our notifications for all of the updated targets.

	if (llqUpdated == NULL) {
		llqUpdated = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	CFSetAddValue(llqUpdated, target);

	if (!(flags & kDNSServiceFlagsMoreComing)) {
		CFSetApplyFunction(llqUpdated, _llq_notify, NULL);
		CFRelease(llqUpdated);
		llqUpdated = NULL;
	}

	return;
}


static Boolean
enqueueLongLivedQuery(SCNetworkReachabilityRef	target)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	if (targetPrivate->serv != NULL) {
		// if "serv" provided, can't use DNSServiceGetAddrInfo
		return FALSE;
	}

	if (memcmp(&targetPrivate->hints, &HINTS_DEFAULT, sizeof(targetPrivate->hints)) != 0) {
		// non-default "hints" provided, can't use DNSServiceGetAddrInfo
		return FALSE;
	}

	// mark the long lived query active
	targetPrivate->llqActive = TRUE;

	// track the DNS resolution time
	__dns_query_start(&targetPrivate->dnsQueryStart, &targetPrivate->dnsQueryEnd);

	CFRetain(target);
	dispatch_async(_llq_queue(), ^{
		DNSServiceErrorType	err;
		dispatch_source_t	source;

		MUTEX_LOCK(&targetPrivate->lock);

		if (targetPrivate->llqTarget != NULL) {
			// if already running
			MUTEX_UNLOCK(&targetPrivate->lock);
			CFRelease(target);
			return;
		}

		// if needed, start interacting with mDNSResponder

		if (llqMain == NULL) {
			err = DNSServiceCreateConnection(&llqMain);
			if (err != kDNSServiceErr_NoError) {
				SCLog(TRUE, LOG_ERR,
				      CFSTR("DNSServiceCreateConnection(&llqMain) failed, error = %d"),
				      err);

				targetPrivate->llqActive = FALSE;

				MUTEX_UNLOCK(&targetPrivate->lock);
				CFRelease(target);
				return;
			}

			err = DNSServiceSetDispatchQueue(llqMain, _llq_queue());
			if (err != kDNSServiceErr_NoError) {
				SCLog(TRUE, LOG_ERR,
				      CFSTR("DNSServiceSetDispatchQueue() failed, error = %d"),
				      err);
				DNSServiceRefDeallocate(llqMain);
				llqMain = NULL;

				targetPrivate->llqActive = FALSE;

				MUTEX_UNLOCK(&targetPrivate->lock);
				CFRelease(target);
				return;
			}
		}

		// start a long-lived-query for this target

		targetPrivate->llqTarget = llqMain;
		err = DNSServiceGetAddrInfo(&targetPrivate->llqTarget,		// sdRef
					    kDNSServiceFlagsReturnIntermediates // flags
					    | kDNSServiceFlagsShareConnection,
					    targetPrivate->if_index,		// interfaceIndex
					    0,					// protocol
					    targetPrivate->name,		// hostname
					    _llq_callback,			// callback
					    (void *)target);			// context
		if (err != kDNSServiceErr_NoError) {
			SCLog(TRUE, LOG_ERR,
			      CFSTR("DNSServiceGetAddrInfo() failed, error = %d"),
			      err);
			targetPrivate->llqTarget = NULL;
			if (llqCount == 0) {
				// if this was the first request
				DNSServiceRefDeallocate(llqMain);
				llqMain = NULL;
			}

			targetPrivate->llqActive = FALSE;

			MUTEX_UNLOCK(&targetPrivate->lock);
			CFRelease(target);
			return;
		}

		llqCount++;

		// if case we don't get any callbacks from our long-lived-query (this
		// could happen if the DNS servers do not respond), we start a timer
		// to ensure that we fire off at least one reachability callback.

		source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
						0,
						0,
						_llq_queue());
		if (source != NULL) {
			// retain the target ... and release it when the [timer] source is released
			CFRetain(target);
			dispatch_set_context(source, (void *)target);
			dispatch_set_finalizer_f(source, (dispatch_function_t)CFRelease);

			dispatch_source_set_event_handler(source, ^(void) {
				_llq_callback(NULL,			// sdRef
					      0,			// flags
					      0,			// interfaceIndex
					      kDNSServiceErr_Timeout,	// errorCode
					      NULL,			// hostname
					      NULL,			// address
					      0,			// ttl
					      (void *)target);		// context
			});

			dispatch_source_set_timer(source,
						  dispatch_time(DISPATCH_TIME_NOW,
								LLQ_TIMEOUT_NSEC),	// start
						  0,					// interval
						  10 * NSEC_PER_MSEC);			// leeway

			targetPrivate->llqTimer = source;
			dispatch_resume(source);
		} else {
			SCLog(TRUE, LOG_ERR,
			      CFSTR("SCNetworkReachability llq dispatch_source_create(no-reply) failed"));
		}

		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	});

	return TRUE;
}


static void
dequeueLongLivedQuery(SCNetworkReachabilityRef	target)
{
	DNSServiceRef			sdRef;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	// terminate the [target] llq timer
	if (targetPrivate->llqTimer != NULL) {
		dispatch_source_cancel(targetPrivate->llqTimer);
		dispatch_release(targetPrivate->llqTimer);
		targetPrivate->llqTimer = NULL;
	}

	// terminate the [target] long lived query
	sdRef = targetPrivate->llqTarget;
	targetPrivate->llqTarget = NULL;

	// mark the long lived query NOT active
	targetPrivate->llqActive = FALSE;

	if (sdRef != NULL) {
		dispatch_async(_llq_queue(), ^{
			DNSServiceRefDeallocate(sdRef);
			CFRelease(target);

			llqCount--;
			if (llqCount == 0) {
				// if no more queries active
				DNSServiceRefDeallocate(llqMain);
				llqMain = NULL;
			}
		});
	}

	return;
}


#pragma mark -
#pragma mark OnDemand


SCNetworkServiceRef
SCNetworkReachabilityCopyOnDemandService(SCNetworkReachabilityRef	target,
					 CFDictionaryRef		*userOptions)
{
	SCNetworkServiceRef		service		= NULL;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	if (targetPrivate->onDemandServiceID != NULL) {
		service = _SCNetworkServiceCopyActive(NULL, targetPrivate->onDemandServiceID);
	}

	if (userOptions != NULL) {
		if (targetPrivate->onDemandName != NULL) {
			*userOptions = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			CFDictionarySetValue((CFMutableDictionaryRef)*userOptions, kSCNetworkConnectionSelectionOptionOnDemandHostName, targetPrivate->onDemandName);
		} else {
			*userOptions = NULL;
		}
	}

	return service;
}


static void
__SCNetworkReachabilityOnDemandCheckCallback(SCNetworkReachabilityRef	onDemandServer,
					     SCNetworkReachabilityFlags	onDemandFlags,
					     void			*info)
{
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)info;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_LOCK(&targetPrivate->lock);

	if (!targetPrivate->scheduled) {
		// if not currently scheduled
		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	}

	SCLog(_sc_debug, LOG_INFO, CFSTR("%sOnDemand \"server\" status changed"),
	      targetPrivate->log_prefix);
	__SCNetworkReachabilityPerform(target);

	MUTEX_UNLOCK(&targetPrivate->lock);

	return;
}


static Boolean
__SCNetworkReachabilityOnDemandCheck(ReachabilityStoreInfoRef	store_info,
				     SCNetworkReachabilityRef	target,
				     Boolean			onDemandRetry,
				     SCNetworkReachabilityFlags	*flags)
{
	Boolean				ok;
	Boolean				onDemand		= FALSE;
	CFStringRef			onDemandRemoteAddress	= NULL;
	CFStringRef			onDemandServiceID	= NULL;
	SCNetworkConnectionStatus	onDemandStatus;
	SCDynamicStoreRef		store;
	SCNetworkReachabilityPrivateRef	targetPrivate		= (SCNetworkReachabilityPrivateRef)target;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

//	SCLog(_sc_debug, LOG_INFO,
//	      CFSTR("%s__SCNetworkReachabilityOnDemandCheck %s"),
//	      targetPrivate->log_prefix,
//	      onDemandRetry ? "after" : "before");

	if (targetPrivate->onDemandName == NULL) {
		targetPrivate->onDemandName = CFStringCreateWithCString(NULL, targetPrivate->name, kCFStringEncodingUTF8);
	}

	/*
	 * check if an OnDemand VPN configuration matches the name.
	 */
	store = store_info->store;
	ok = __SCNetworkConnectionCopyOnDemandInfoWithName(&store,
							   targetPrivate->onDemandName,
							   onDemandRetry,
							   &onDemandServiceID,
							   &onDemandStatus,
							   &onDemandRemoteAddress);
	if ((store_info->store == NULL) && (store != NULL)) {
		// if an SCDynamicStore session was added, keep it
		store_info->store = store;
	}
	if (!_SC_CFEqual(targetPrivate->onDemandRemoteAddress, onDemandRemoteAddress) ||
	    !_SC_CFEqual(targetPrivate->onDemandServiceID, onDemandServiceID)) {
		if (targetPrivate->onDemandRemoteAddress != NULL) {
			CFRelease(targetPrivate->onDemandRemoteAddress);
			targetPrivate->onDemandRemoteAddress = NULL;
		}

		if (targetPrivate->onDemandServer != NULL) {
			if (targetPrivate->dispatchQueue != NULL) {
				// unschedule
				__SCNetworkReachabilityUnscheduleFromRunLoop(targetPrivate->onDemandServer, NULL, NULL, TRUE);
			} else if (targetPrivate->rls != NULL) {
				CFIndex	i;
				CFIndex	n;

				// unschedule
				n = CFArrayGetCount(targetPrivate->rlList);
				for (i = 0; i < n; i += 3) {
					CFRunLoopRef	rl	= (CFRunLoopRef)CFArrayGetValueAtIndex(targetPrivate->rlList, i+1);
					CFStringRef	rlMode	= (CFStringRef) CFArrayGetValueAtIndex(targetPrivate->rlList, i+2);

					__SCNetworkReachabilityUnscheduleFromRunLoop(targetPrivate->onDemandServer, rl, rlMode, TRUE);
				}
			}

			CFRelease(targetPrivate->onDemandServer);
			targetPrivate->onDemandServer = NULL;
		}

		if (targetPrivate->onDemandServiceID != NULL) {
			CFRelease(targetPrivate->onDemandServiceID);
			targetPrivate->onDemandServiceID = NULL;
		}
	}
	if (ok) {
		if (onDemandStatus != kSCNetworkConnectionConnected) {
			/*
			 * if we have a VPN configuration matching the name *and* we need to
			 * bring the VPN up.  Combine our flags with those of the VPN server.
			 */
			if (targetPrivate->onDemandServer == NULL) {
				CFMutableDictionaryRef		options;

				options = CFDictionaryCreateMutable(NULL,
								    0,
								    &kCFTypeDictionaryKeyCallBacks,
								    &kCFTypeDictionaryValueCallBacks);
				CFDictionarySetValue(options, kSCNetworkReachabilityOptionNodeName, onDemandRemoteAddress);
				CFDictionarySetValue(options, kSCNetworkReachabilityOptionConnectionOnDemandBypass, kCFBooleanTrue);
#ifdef	HAVE_REACHABILITY_SERVER
				CFDictionarySetValue(options, kSCNetworkReachabilityOptionServerBypass, kCFBooleanTrue);
#endif	// HAVE_REACHABILITY_SERVER
				targetPrivate->onDemandServer = SCNetworkReachabilityCreateWithOptions(NULL, options);
				CFRelease(options);

				if (targetPrivate->scheduled) {
					SCNetworkReachabilityContext	context	= { 0, NULL, CFRetain, CFRelease, CFCopyDescription };

					context.info = (void *)target;
					SCNetworkReachabilitySetCallback(targetPrivate->onDemandServer,
									 __SCNetworkReachabilityOnDemandCheckCallback,
									 &context);

					// schedule server reachability to match that of the target
					if (targetPrivate->dispatchQueue != NULL) {
						__SCNetworkReachabilityScheduleWithRunLoop(targetPrivate->onDemandServer, NULL, NULL, targetPrivate->dispatchQueue, TRUE);
					} else {
						CFIndex	i;
						CFIndex	n;

						n = CFArrayGetCount(targetPrivate->rlList);
						for (i = 0; i < n; i += 3) {
							CFRunLoopRef	rl	= (CFRunLoopRef)CFArrayGetValueAtIndex(targetPrivate->rlList, i+1);
							CFStringRef	rlMode	= (CFStringRef) CFArrayGetValueAtIndex(targetPrivate->rlList, i+2);

							__SCNetworkReachabilityScheduleWithRunLoop(targetPrivate->onDemandServer, rl, rlMode, NULL, TRUE);
						}
					}
				}
			}

			ok = SCNetworkReachabilityGetFlags(targetPrivate->onDemandServer, flags);
			SCLog(_sc_debug, LOG_INFO, CFSTR("%s  status  * = 0x%08x"),
			      targetPrivate->log_prefix,
			      *flags);
			if (ok && (*flags & kSCNetworkReachabilityFlagsReachable)) {
				if (!(*flags & kSCNetworkReachabilityFlagsTransientConnection)) {
					// start clean if not already layered on a transient network
					*flags = 0;
				}
				*flags |= kSCNetworkReachabilityFlagsReachable;
				*flags |= kSCNetworkReachabilityFlagsTransientConnection;
				*flags |= kSCNetworkReachabilityFlagsConnectionRequired;
				*flags |= kSCNetworkReachabilityFlagsConnectionOnDemand;

				if (_sc_debug) {
					SCLog(TRUE, LOG_INFO, CFSTR("%s  service * = %@"),
					      targetPrivate->log_prefix,
					      onDemandServiceID);
					SCLog(TRUE, LOG_INFO, CFSTR("%s  status    = isReachable (after OnDemand connect)"),
					      targetPrivate->log_prefix);
				}

				onDemand = TRUE;
			}
		}

		if (onDemandRemoteAddress != NULL) {
			if (targetPrivate->onDemandRemoteAddress == NULL) {
				targetPrivate->onDemandRemoteAddress = onDemandRemoteAddress;
			} else {
				CFRelease(onDemandRemoteAddress);
			}
		}

		if (onDemandServiceID != NULL) {
			if (targetPrivate->onDemandServiceID == NULL) {
				targetPrivate->onDemandServiceID = onDemandServiceID;
			} else {
				CFRelease(onDemandServiceID);
			}
		}
	}

	return onDemand;
}


#pragma mark -
#pragma mark Reachability Flags


#ifdef	HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL
typedef struct {
	int		status;
	struct addrinfo	*res;
} reply_info;


static void
reply_callback(int32_t status, struct addrinfo *res, void *context)
{
	reply_info	*reply	= (reply_info *)context;

	reply->status = status;
	reply->res    = res;
	return;
}


static int
getaddrinfo_interface_sync(const char			*nodename,
			   const char			*servname,
			   const struct addrinfo	*hints,
			   const char			*interface,
			   struct addrinfo		**res)
{
	mach_port_t	mp;
	reply_info	reply	= { NETDB_SUCCESS, NULL };

	mp = _getaddrinfo_interface_async_call(nodename,
					       servname,
					       hints,
					       interface,
					       reply_callback,
					       (void *)&reply);
	if (mp == MACH_PORT_NULL) {
		return EAI_SYSTEM;
	}

	while (TRUE) {
		int		g_status;
		union {
			u_int8_t		buf[8192];
			mach_msg_empty_rcv_t	msg;
		}		m_reply;
		kern_return_t 	m_status;

		m_status = mach_msg(&m_reply.msg.header,	/* msg */
				    MACH_RCV_MSG,		/* options */
				    0,				/* send_size */
				    sizeof(m_reply),		/* rcv_size */
				    mp,				/* rcv_name */
				    MACH_MSG_TIMEOUT_NONE,	/* timeout */
				    MACH_PORT_NULL);		/* notify */
		if (m_status != KERN_SUCCESS) {
			return EAI_SYSTEM;
		}

		g_status = getaddrinfo_async_handle_reply((void *)m_reply.buf);
		if (g_status != 0) {
			if (reply.res != NULL) {
				freeaddrinfo(reply.res);
				reply.res = NULL;
			}
			return EAI_SYSTEM;
		}

		if ((reply.res != NULL) || (reply.status != NETDB_SUCCESS)) {
			// if we have a reply or an error
			break;
		}

		// if the request is not complete and needs to be re-queued
	}

	*res = reply.res;
	return reply.status;
}
#endif	/* HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL */


static Boolean
__SCNetworkReachabilityGetFlags(ReachabilityStoreInfoRef	store_info,
				SCNetworkReachabilityRef	target,
				ReachabilityInfo		*reach_info,
				Boolean				async)
{
	CFMutableArrayRef		addresses	= NULL;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;
	ReachabilityInfo		my_info		= NOT_REACHABLE;
	Boolean				ok		= TRUE;

	MUTEX_ASSERT_HELD(&targetPrivate->lock);

	_reach_set(reach_info, &NOT_REACHABLE, reach_info->cycle);

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

#ifdef	HAVE_REACHABILITY_SERVER
	if (!targetPrivate->serverBypass) {
		if (!targetPrivate->serverActive) {
			ok = __SCNetworkReachabilityServer_targetAdd(target);
			if (!ok) {
				targetPrivate->serverBypass = TRUE;
			}
		}

		if (targetPrivate->serverActive) {
			ok = __SCNetworkReachabilityServer_targetStatus(target);
			if (!ok) {
				SCLog(TRUE, LOG_DEBUG,
				      CFSTR("__SCNetworkReachabilityGetFlags _targetStatus() failed"));
				_SCErrorSet(kSCStatusFailed);
				goto done;
			}

			targetPrivate->cycle = targetPrivate->serverInfo.cycle;
			_reach_set(&my_info, &targetPrivate->serverInfo, targetPrivate->cycle);
			goto done;
		}
	}
#endif	// HAVE_REACHABILITY_SERVER

	switch (targetPrivate->type) {
		case reachabilityTypeAddress :
		case reachabilityTypeAddressPair : {
			/*
			 * Check "local" address
			 */
			if (targetPrivate->localAddress != NULL) {
				/*
				 * Check "local" address
				 */
				ok = checkAddress(store_info,
						  targetPrivate->localAddress,
						  targetPrivate->if_index,
						  &my_info,
						  targetPrivate->log_prefix);
				if (!ok) {
					goto error;	/* not today */
				}

				if (!(my_info.flags & kSCNetworkReachabilityFlagsIsLocalAddress)) {
					goto error;	/* not reachable, non-"local" address */
				}
			}

			/*
			 * Check "remote" address
			 */
			if (targetPrivate->remoteAddress != NULL) {
				/*
				 * in cases where we have "local" and "remote" addresses
				 * we need to re-initialize the to-be-returned flags.
				 */
				my_info = NOT_REACHABLE;

				/*
				 * Check "remote" address
				 */
				ok = checkAddress(store_info,
						  targetPrivate->remoteAddress,
						  targetPrivate->if_index,
						  &my_info,
						  targetPrivate->log_prefix);
				if (!ok) {
					goto error;	/* not today */
				}
			}

			break;

		}

		case reachabilityTypeName : {
			struct timeval			dnsQueryStart;
			struct timeval			dnsQueryEnd;
			int				error;
			SCNetworkReachabilityFlags	ns_flags;
			uint32_t			ns_if_index;
			struct addrinfo			*res;

			addresses = (CFMutableArrayRef)SCNetworkReachabilityCopyResolvedAddress(target, &error);
			if ((addresses != NULL) || (error != NETDB_SUCCESS)) {
				/* if resolved or an error had been detected */
				if (!async) {
					/* if not an async request */
					goto checkResolvedAddress;
				} else if (targetPrivate->llqActive) {
					/* if long-lived-query active */
					goto checkResolvedAddress;
				} else if ((targetPrivate->dnsMP == MACH_PORT_NULL) && !targetPrivate->needResolve) {
					struct timeval		elapsed;
					const struct timeval	retry_limit	= { EAI_NONAME_RETRY_LIMIT_USEC / USEC_PER_SEC,
										    EAI_NONAME_RETRY_LIMIT_USEC % USEC_PER_SEC };

					/*
					 * if this is an async request (i.e. someone is watching the reachability
					 * of this target), if no query active, and if no query is needed
					 */

					if ((error != EAI_NONAME)
#if defined(EAI_NODATA) && (EAI_NODATA != EAI_NONAME)
					    && (error != EAI_NODATA)
#endif
					   ) {
						/* if not "host not found" */
						goto checkResolvedAddress;
					}

					/*
					 * if our last DNS query returned EAI_NONAME then we
					 * "may" want to retry.
					 *
					 * Specifically, if the [DNS] configuration was updated a while
					 * back then we'll trust the EAI_NONAME reply. Otherwise, we
					 * want to try again to ensure that we didn't get caught in a
					 * race between the time when the configuration was changed and
					 * when mDNSResponder is really ready to handle the query.
					 *
					 * Retry handling details :
					 *
					 * Compare the time when the DNS configuration was last changed and
					 * when our DNS reply was started (->last_dns vs ->dnsQueryStart).
					 *
					 * Expected: 0 < last_dns (t1) < dnsQueryStart (t2)
					 *
					 * last  start  end   description                        action
					 * ====  =====  ====  =================================  ========
					 *  0     N/A    N/A  no change, query error             no retry
					 *  0     N/A    N/A  no change, query complete          no retry
					 *  N/A   N/A    0    changed, query in-flight or error  no retry
					 *  t1 >  t2          query started, then [DNS] changed  no retry
					 *  t1 == t2          changed & query started together   no retry
					 *  t1 <  t2          changed, then query started        retry
					 */

					if (!timerisset(&targetPrivate->last_dns)) {
						/*
						 * if we have not yet seen a DNS configuration
						 * change
						 */
						goto checkResolvedAddress;
					}

					if (!timerisset(&targetPrivate->dnsQueryEnd)) {
						/*
						 * if no query end time (new request in flight)
						 */
						goto checkResolvedAddress;
					}

					if (timercmp(&targetPrivate->last_dns,
						     &targetPrivate->dnsQueryStart,
						     >=)) {
						/*
						 * if our DNS query started and then, a
						 * short time later, the DNS configuration
						 * was changed we don't need to retry
						 * because we will be re-issuing (and not
						 * retrying) the query.
						 */
						goto checkResolvedAddress;
					}

					timersub(&targetPrivate->dnsQueryStart,
						 &targetPrivate->last_dns,
						 &elapsed);
					if (timercmp(&elapsed, &retry_limit, >)) {
						/*
						 * if the DNS query started after mDNSResponder
						 * had a chance to apply the last configuration
						 * then we should trust the EAI_NONAME reply.
						 */
						goto checkResolvedAddress;
					}

					/* retry the DNS query */

					if (targetPrivate->dnsRetry != NULL) {
						// no need to schedule if we already have a
						// retry query in flight
						break;
					}

					targetPrivate->dnsRetryCount++;

					SCLog(_sc_debug, LOG_INFO,
					      CFSTR("%sretry [%d] DNS query for %s%s%s%s%s"),
					      targetPrivate->log_prefix,
					      targetPrivate->dnsRetryCount,
					      targetPrivate->name != NULL ? "name = " : "",
					      targetPrivate->name != NULL ? targetPrivate->name : "",
					      targetPrivate->name != NULL && targetPrivate->serv != NULL ? ", " : "",
					      targetPrivate->serv != NULL ? "serv = " : "",
					      targetPrivate->serv != NULL ? targetPrivate->serv : "");

					enqueueAsyncDNSRetry(target);
					break;
				}
			}

			if (!targetPrivate->onDemandBypass) {
				Boolean	onDemand;

				/*
				 * before we attempt our initial DNS query, check if there is
				 * an OnDemand configuration that we should be using.
				 */
				onDemand = __SCNetworkReachabilityOnDemandCheck(store_info, target, FALSE, &my_info.flags);
				if (onDemand) {
					/* if OnDemand connection is needed */
					goto done;
				}
			}

			/* check the reachability of the DNS servers */
			ok = _SC_R_checkResolverReachability(store_info,
							     &ns_flags,
							     &targetPrivate->haveDNS,
							     targetPrivate->name,
							     targetPrivate->serv,
							     targetPrivate->if_index,
							     &ns_if_index,
							     NULL,
							     targetPrivate->log_prefix);
			if (!ok) {
				/* if we could not get DNS server info */
				SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS server reachability unknown"),
				      targetPrivate->log_prefix);
				goto error;
			} else if (rankReachability(ns_flags) < 2) {
				/*
				 * if DNS servers are not (or are no longer) reachable, set
				 * flags based on the availability of configured (but not
				 * active) services.
				 */

				SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS server(s) not available"),
				      targetPrivate->log_prefix);

				ok = checkAddress(store_info,
						  NULL,
						  targetPrivate->if_index,
						  &my_info,
						  targetPrivate->log_prefix);
				if (!ok) {
					SCLog(_sc_debug, LOG_INFO, CFSTR("%sNo available networks"),
					      targetPrivate->log_prefix);
					goto error;
				}

				if (async && targetPrivate->scheduled) {
					/*
					 * return "host not found", set flags appropriately,
					 * and schedule notification.
					 */
					__SCNetworkReachabilityCallbackSetResolvedAddress(EAI_NONAME,
											  NULL,
											  (void *)target);
					my_info.flags |= (targetPrivate->info.flags & kSCNetworkReachabilityFlagsFirstResolvePending);

					SCLog(_sc_debug, LOG_INFO, CFSTR("%sno DNS servers are reachable"),
					      targetPrivate->log_prefix);
					__SCNetworkReachabilityPerform(target);
				}
				break;
			}

			if (targetPrivate->resolverBypass) {
				/* if we are not resolving the name,
				 * set the flags of the resolvers */
				my_info.flags = ns_flags;
				my_info.if_index = ns_if_index;
				break;
			}

			if (async) {
				/* for async requests we return the last known status */
				my_info = targetPrivate->info;

				if (targetPrivate->dnsMP != MACH_PORT_NULL) {
					/* if request already in progress */
					SCLog(_sc_debug, LOG_INFO,
					      CFSTR("%swaiting for DNS reply"),
					      targetPrivate->log_prefix);
					if ((addresses != NULL) || (error != NETDB_SUCCESS)) {
						/* updated reachability based on the previous reply */
						goto checkResolvedAddress;
					}
					break;
				}

				if (targetPrivate->dnsRetry != NULL) {
					/* if we already have a "retry" queued */
					break;
				}

				if (targetPrivate->llqActive) {
					/* if long-lived-query active */
					SCLog(_sc_debug, LOG_INFO,
					      CFSTR("%swaiting for DNS updates"),
					      targetPrivate->log_prefix);
					if ((addresses != NULL) || (error != NETDB_SUCCESS)) {
						/* updated reachability based on the previous reply */
						goto checkResolvedAddress;
					}
					break;
				}

				if (!targetPrivate->llqBypass) {
					SCLog(_sc_debug, LOG_INFO,
					      CFSTR("%sstart long-lived DNS query for %s%s%s%s%s"),
					      targetPrivate->log_prefix,
					      targetPrivate->name != NULL ? "name = " : "",
					      targetPrivate->name != NULL ? targetPrivate->name : "",
					      targetPrivate->name != NULL && targetPrivate->serv != NULL ? ", " : "",
					      targetPrivate->serv != NULL ? "serv = " : "",
					      targetPrivate->serv != NULL ? targetPrivate->serv : "");

					/*
					 * initiate an long-lived DNS query
					 */
					if (enqueueLongLivedQuery(target)) {
						/* request initiated */
						break;
					}
				}

				SCLog(_sc_debug, LOG_INFO,
				      CFSTR("%sstart DNS query for %s%s%s%s%s"),
				      targetPrivate->log_prefix,
				      targetPrivate->name != NULL ? "name = " : "",
				      targetPrivate->name != NULL ? targetPrivate->name : "",
				      targetPrivate->name != NULL && targetPrivate->serv != NULL ? ", " : "",
				      targetPrivate->serv != NULL ? "serv = " : "",
				      targetPrivate->serv != NULL ? targetPrivate->serv : "");

				/*
				 * initiate an async DNS query
				 */
				if (startAsyncDNSQuery(target)) {
					/* request initiated */
					break;
				}

				/* if we could not initiate the request, process error */
				goto checkResolvedAddress;
			}

			SCLog(_sc_debug, LOG_INFO,
			      CFSTR("%scheck DNS for %s%s%s%s%s"),
			      targetPrivate->log_prefix,
			      targetPrivate->name != NULL ? "name = " : "",
			      targetPrivate->name != NULL ? targetPrivate->name : "",
			      targetPrivate->name != NULL && targetPrivate->serv != NULL ? ", " : "",
			      targetPrivate->serv != NULL ? "serv = " : "",
			      targetPrivate->serv != NULL ? targetPrivate->serv : "");

			/*
			 * OK, all of the DNS name servers are available.  Let's
			 * resolve the nodename into an address.
			 */
			__dns_query_start(&dnsQueryStart, &dnsQueryEnd);

#ifdef	HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL
			if (targetPrivate->if_index == 0) {
#endif	// HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL
				error = getaddrinfo(targetPrivate->name,
						    targetPrivate->serv,
						    &targetPrivate->hints,
						    &res);
#ifdef	HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL
			} else {
				error = getaddrinfo_interface_sync(targetPrivate->name,
								   targetPrivate->serv,
								   &targetPrivate->hints,
								   targetPrivate->if_name,
								   &res);
			}
#endif	// HAVE_GETADDRINFO_INTERFACE_ASYNC_CALL

			__dns_query_end(target,
					 ((error == 0) && (res != NULL)),	// if successful query
					 dns_query_sync,			// sync
					 &dnsQueryStart,			// start time
					 &dnsQueryEnd);				// end time

			__SCNetworkReachabilitySetResolvedAddress(error, res, target);

			addresses = (CFMutableArrayRef)SCNetworkReachabilityCopyResolvedAddress(target, &error);

		    checkResolvedAddress :

			/*
			 * We first assume that the requested host is NOT available.
			 * Then, check each address for accessibility and return the
			 * best status available.
			 */
			my_info = NOT_REACHABLE;

			if (isA_CFArray(addresses)) {
				CFIndex		i;
				CFIndex		n	= CFArrayGetCount(addresses);

				for (i = 0; i < n; i++) {
					ReachabilityInfo	ns_info	= NOT_REACHABLE;
					struct sockaddr		*sa;

					sa = (struct sockaddr *)CFDataGetBytePtr(CFArrayGetValueAtIndex(addresses, i));

					ok = checkAddress(store_info,
							  sa,
							  targetPrivate->if_index,
							  &ns_info,
							  targetPrivate->log_prefix);
					if (!ok) {
						goto error;	/* not today */
					}

					if (rankReachability(ns_info.flags) > rankReachability(my_info.flags)) {
						/* return the best case result */
						my_info = ns_info;
						if (rankReachability(my_info.flags) == 2) {
							/* we're in luck */
							break;
						}
					}
				}
			} else {
				if ((error == EAI_NONAME)
#if defined(EAI_NODATA) && (EAI_NODATA != EAI_NONAME)
				     || (error == EAI_NODATA)
#endif
				     ) {
					/*
					 * the target host name could not be resolved
					 */
					if (!targetPrivate->onDemandBypass) {
						Boolean	onDemand;

						/*
						 * our initial DNS query failed, check again to see if there
						 * there is an OnDemand configuration that we should be using.
						 */
						onDemand = __SCNetworkReachabilityOnDemandCheck(store_info, target, TRUE, &my_info.flags);
						if (onDemand) {
							/* if OnDemand connection is needed */
							goto done;
						}
					}

					if (!targetPrivate->haveDNS) {
						/*
						 * No DNS servers are defined. Set flags based on
						 * the availability of configured (but not active)
						 * services.
						 */
						ok = checkAddress(store_info,
								  NULL,
								  targetPrivate->if_index,
								  &my_info,
								  targetPrivate->log_prefix);
						if (!ok) {
							goto error;	/* not today */
						}

						if ((my_info.flags & kSCNetworkReachabilityFlagsReachable) &&
							(my_info.flags & kSCNetworkReachabilityFlagsConnectionRequired)) {
							/*
							 * Since we might pick up a set of DNS servers when this connection
							 * is established, don't reply with a "HOST NOT FOUND" error just yet.
							 */
							break;
						}

						/* Host not found, not reachable! */
						my_info = NOT_REACHABLE;
					}
				}
			}

			break;
		}
	}

    done:

	_reach_set(reach_info, &my_info, targetPrivate->cycle);

    error :

	if (addresses != NULL)	CFRelease(addresses);
	return ok;
}

int
SCNetworkReachabilityGetInterfaceIndex(SCNetworkReachabilityRef	target)
{
	SCNetworkReachabilityFlags	flags;
	int				if_index	= -1;
	Boolean				ok		= TRUE;
	ReachabilityStoreInfo		store_info;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return if_index;
	}

	ReachabilityStoreInfo_init(&store_info);

	MUTEX_LOCK(&targetPrivate->lock);

	if (targetPrivate->scheduled) {
		// if being watched, return the last known (and what should be current) status
		flags = targetPrivate->info.flags & ~kSCNetworkReachabilityFlagsFirstResolvePending;
		goto done;
	}


	ok = __SCNetworkReachabilityGetFlags(&store_info, target, &targetPrivate->info, FALSE);
	flags = targetPrivate->info.flags & ~kSCNetworkReachabilityFlagsFirstResolvePending;

    done :

	/* Only return the if_index if the connection is reachable not for reachable connection
	 * required etc ... */
	if (ok && rankReachability(flags) == 2) {
		if_index = targetPrivate->info.if_index;
	}

	MUTEX_UNLOCK(&targetPrivate->lock);
	ReachabilityStoreInfo_free(&store_info);
	return if_index;
}


Boolean
SCNetworkReachabilityGetFlags(SCNetworkReachabilityRef		target,
			      SCNetworkReachabilityFlags	*flags)
{
	Boolean				ok		= TRUE;
	ReachabilityStoreInfo		store_info;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	ReachabilityStoreInfo_init(&store_info);

	MUTEX_LOCK(&targetPrivate->lock);

	if (targetPrivate->scheduled) {
		// if being watched, return the last known (and what should be current) status
		*flags = targetPrivate->info.flags & ~kSCNetworkReachabilityFlagsFirstResolvePending;
		goto done;
	}


	ok = __SCNetworkReachabilityGetFlags(&store_info, target, &targetPrivate->info, FALSE);
	*flags = targetPrivate->info.flags & ~kSCNetworkReachabilityFlagsFirstResolvePending;

    done :

	MUTEX_UNLOCK(&targetPrivate->lock);
	ReachabilityStoreInfo_free(&store_info);
	return ok;
}


#pragma mark -
#pragma mark Notifications


static void
__SCNetworkReachabilityReachabilitySetNotifications(SCDynamicStoreRef	store)
{
	CFStringRef			key;
	CFMutableArrayRef		keys;
	CFStringRef			pattern;
	CFMutableArrayRef		patterns;

	keys     = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	patterns = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	// Setup:/Network/Global/IPv4 (for the ServiceOrder)
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainSetup,
							 kSCEntNetIPv4);
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	// State:/Network/Global/DNS
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetDNS);
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	// State:/Network/Global/IPv4 (default route)
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetIPv4);
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	// State:/Network/Global/OnDemand
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetOnDemand);
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	// Setup: per-service Interface info
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetInterface);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

	// per-service IPv4 info
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetIPv4);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetIPv4);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

	// per-service IPv6 info
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetIPv6);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetIPv6);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

	// per-service PPP info (for existence, kSCPropNetPPPDialOnDemand, kSCPropNetPPPStatus)
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetPPP);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetPPP);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

#if	!TARGET_IPHONE_SIMULATOR
	// per-service VPN info (for existence, kSCPropNetVPNStatus)
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetVPN);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetVPN);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
#endif	// !TARGET_IPHONE_SIMULATOR

	// per-service IPSec info (for existence, kSCPropNetIPSecStatus)
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainSetup,
							      kSCCompAnyRegex,
							      kSCEntNetIPSec);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);
	pattern = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							      kSCDynamicStoreDomainState,
							      kSCCompAnyRegex,
							      kSCEntNetIPSec);
	CFArrayAppendValue(patterns, pattern);
	CFRelease(pattern);

#if	!TARGET_OS_IPHONE
	// State: Power Management Capabilities
	key = SCDynamicStoreKeyCreate(NULL, CFSTR("%@%@"),
				      kSCDynamicStoreDomainState,
				      CFSTR(kIOPMSystemPowerCapabilitiesKeySuffix));
	CFArrayAppendValue(keys, key);
	CFRelease(key);
#endif	// TARGET_OS_IPHONE


	// SCDynamicStore key to force posting a reachability change
	CFArrayAppendValue(keys, SCNETWORKREACHABILITY_TRIGGER_KEY);

	(void)SCDynamicStoreSetNotificationKeys(store, keys, patterns);
	CFRelease(keys);
	CFRelease(patterns);

	return;
}


static dispatch_queue_t
_hn_queue()
{
	static dispatch_once_t	once;
	static dispatch_queue_t	q;

	dispatch_once(&once, ^{
		q = dispatch_queue_create("SCNetworkReachabilty.changes", NULL);
	});

	return q;
}


static void
__SCNetworkReachabilityHandleChanges(SCDynamicStoreRef	store,
				     CFArrayRef		changedKeys,
				     void		*info)
{
#if	!TARGET_OS_IPHONE
	Boolean			cpuStatusChanged	= FALSE;
#endif	// !TARGET_OS_IPHONE
	Boolean			dnsConfigChanged	= FALSE;
	CFIndex			i;
	Boolean			forcedChange		= FALSE;
	CFStringRef		key;
	CFIndex			nChanges;
	CFIndex			nGlobals		= 0;
	CFIndex			nTargets;
	Boolean			networkConfigChanged	= FALSE;
	struct timeval		now;
#if	!TARGET_OS_IPHONE
	Boolean			powerStatusChanged	= FALSE;
#endif	// !TARGET_OS_IPHONE
	ReachabilityStoreInfo	store_info;
	const void *		targets_q[N_QUICK];
	const void **		targets			= targets_q;
	__block CFSetRef	watchers		= NULL;

	nChanges = CFArrayGetCount(changedKeys);
	if (nChanges == 0) {
		/* if no changes */
		return;
	}

	/* "something" changed, start fresh */
	ReachabilityStoreInfo_save(NULL);

	dispatch_sync(_hn_queue(), ^{
		/* grab the currently watched targets */
		if (hn_targets != NULL) {
			watchers = CFSetCreateCopy(NULL, hn_targets);
		}
	});

	nTargets = (watchers != NULL) ? CFSetGetCount(watchers) : 0;
	if (nTargets == 0) {
		/* if no addresses being monitored */
		goto done;
	}

	/* grab the current time */
	(void)gettimeofday(&now, NULL);

#if	!TARGET_OS_IPHONE
	key = SCDynamicStoreKeyCreate(NULL, CFSTR("%@%@"),
				      kSCDynamicStoreDomainState,
				      CFSTR(kIOPMSystemPowerCapabilitiesKeySuffix));
	if (CFArrayContainsValue(changedKeys, CFRangeMake(0, nChanges), key)) {
		CFNumberRef	num;

		nGlobals++;

		num = SCDynamicStoreCopyValue(store, key);
		if (num != NULL) {
			if (isA_CFNumber(num) &&
			    CFNumberGetValue(num, kCFNumberSInt32Type, &power_capabilities)) {
				static Boolean	haveCPU_old	= TRUE;
				Boolean		haveCPU_new;

				powerStatusChanged = TRUE;

				haveCPU_new = (power_capabilities & kIOPMSystemPowerStateCapabilityCPU) != 0;
				if ((haveCPU_old != haveCPU_new) && haveCPU_new) {
					/*
					 * if the power state now shows CPU availability
					 * then we will assume that the DNS configuration
					 * has changed.  This will force us to re-issue
					 * our DNS queries since mDNSResponder does not
					 * attempt to resolve names when "sleeping".
					 */
					cpuStatusChanged = TRUE;
					dnsConfigChanged = TRUE;
				}
				haveCPU_old = haveCPU_new;
			}

			CFRelease(num);
		}
	}
	CFRelease(key);
#endif	// !TARGET_OS_IPHONE

	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetDNS);
	if (CFArrayContainsValue(changedKeys, CFRangeMake(0, nChanges), key)) {
		nGlobals++;
		dnsConfigChanged = TRUE;	/* the DNS server(s) have changed */
	}
	CFRelease(key);

	if (CFArrayContainsValue(changedKeys, CFRangeMake(0, nChanges), SCNETWORKREACHABILITY_TRIGGER_KEY)) {
		nGlobals++;
		forcedChange = TRUE;		/* an SCDynamicStore driven "network" change */
	}

	if (nChanges > nGlobals) {
		networkConfigChanged = TRUE;
	}

	if (_sc_debug) {
		unsigned int		changes			= 0;
		static const char	*change_strings[]	= {
			// with no "power" status change
			"",
			"network ",
			"DNS ",
			"network and DNS ",
#if	!TARGET_OS_IPHONE
			// with "power" status change
			"power ",
			"network and power ",
			"DNS and power ",
			"network, DNS, and power ",

			// with "power" status change (including CPU "on")
			"power* ",
			"network and power* ",
			"DNS and power* ",
			"network, DNS, and power* ",
#endif	// !TARGET_OS_IPHONE
		};

#if	!TARGET_OS_IPHONE
		#define	PWR	4
		if (powerStatusChanged) {
			changes |= PWR;
			if (cpuStatusChanged) {
				changes += PWR;
			}
		}
#endif	// !TARGET_OS_IPHONE

		#define	DNS	2
		if (dnsConfigChanged) {
			changes |= DNS;
		}

		#define	NET	1
		if (networkConfigChanged) {
			changes |= NET;
		}

		SCLog(TRUE, LOG_INFO,
		      CFSTR("process %s%sconfiguration change"),
		      forcedChange ? "[forced] " : "",
		      change_strings[changes]);
	}

	ReachabilityStoreInfo_init(&store_info);

	if (nTargets > (CFIndex)(sizeof(targets_q) / sizeof(CFTypeRef)))
		targets = CFAllocatorAllocate(NULL, nTargets * sizeof(CFTypeRef), 0);
	CFSetGetValues(watchers, targets);
	for (i = 0; i < nTargets; i++) {
		SCNetworkReachabilityRef	target		= targets[i];
		SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

		MUTEX_LOCK(&targetPrivate->lock);

		if (dnsConfigChanged) {
			targetPrivate->last_dns = now;
			targetPrivate->dnsRetryCount = 0;
		}

		if (networkConfigChanged) {
			targetPrivate->last_network = now;
		}

#if	!TARGET_OS_IPHONE
		if (powerStatusChanged) {
			targetPrivate->last_power = now;
		}
#endif	// !TARGET_OS_IPHONE

		if (targetPrivate->type == reachabilityTypeName) {
			Boolean		dnsChanged	= dnsConfigChanged;

			if (!dnsChanged) {
				/*
				 * if the DNS configuration didn't change we still need to
				 * check that the DNS servers are accessible.
				 */
				SCNetworkReachabilityFlags	ns_flags;
				Boolean				ok;

				/* check the reachability of the DNS servers */
				ok = ReachabilityStoreInfo_update(&store_info, &store, AF_UNSPEC);
				if (ok) {
					ok = _SC_R_checkResolverReachability(&store_info,
									     &ns_flags,
									     &targetPrivate->haveDNS,
									     targetPrivate->name,
									     targetPrivate->serv,
									     targetPrivate->if_index,
									     NULL,
									     NULL,
									     targetPrivate->log_prefix);
				}

				if (!ok) {
					/* if we could not get DNS server info */
					SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS server reachability unknown"),
					      targetPrivate->log_prefix);
					dnsChanged = TRUE;
				} else if (rankReachability(ns_flags) < 2) {
					/*
					 * if DNS servers are not (or are no longer) reachable, set
					 * flags based on the availability of configured (but not
					 * active) services.
					 */
					SCLog(_sc_debug, LOG_INFO, CFSTR("%sDNS server(s) not available"),
					      targetPrivate->log_prefix);
					dnsChanged = TRUE;
				}
			}

			if (dnsChanged) {
				if (targetPrivate->dnsMP != MACH_PORT_NULL) {
					/* cancel the outstanding DNS query */
					SCLog(_sc_debug, LOG_INFO,
					      CFSTR("%scancel DNS query for %s%s%s%s%s"),
					      targetPrivate->log_prefix,
					      targetPrivate->name != NULL ? "name = " : "",
					      targetPrivate->name != NULL ? targetPrivate->name : "",
					      targetPrivate->name != NULL && targetPrivate->serv != NULL ? ", " : "",
					      targetPrivate->serv != NULL ? "serv = " : "",
					      targetPrivate->serv != NULL ? targetPrivate->serv : "");
					dequeueAsyncDNSQuery(target, TRUE);
				}

				if (targetPrivate->dnsRetry != NULL) {
					/* cancel the outstanding DNS retry */
					dequeueAsyncDNSRetry(target);
				}

				/* schedule request to resolve the name again */
				targetPrivate->needResolve = TRUE;
			}
		}

		if (forcedChange) {
			targetPrivate->cycle++;
		}

		if (targetPrivate->scheduled) {
			__SCNetworkReachabilityPerform(target);
		}

		MUTEX_UNLOCK(&targetPrivate->lock);
	}
	if (targets != targets_q)	CFAllocatorDeallocate(NULL, targets);

	ReachabilityStoreInfo_free(&store_info);

    done :

	if (watchers != NULL) CFRelease(watchers);
	return;
}


#if	!TARGET_OS_IPHONE

static Boolean
darkWakeNotify(SCNetworkReachabilityRef target)
{
	return FALSE;
}


static Boolean
systemIsAwake(IOPMSystemPowerStateCapabilities power_capabilities)
{

#define POWER_CAPABILITIES_NEED	(kIOPMSystemPowerStateCapabilityCPU		\
				 | kIOPMSystemPowerStateCapabilityNetwork	\
				 | kIOPMSystemPowerStateCapabilityDisk)

	if ((power_capabilities & POWER_CAPABILITIES_NEED) != POWER_CAPABILITIES_NEED) {
		/*
		 * we're not awake (from a networking point of view) unless we
		 * have the CPU, disk, *and* network.
		 */
		return FALSE;
	}

	if ((power_capabilities & kIOPMSytemPowerStateCapabilitiesMask) == POWER_CAPABILITIES_NEED) {
		/*
		 * if all we have is the CPU, disk, and network than this must
		 * be a "maintenance" wake.
		 */
		return FALSE;
	}

	return TRUE;
}

#endif	// !TARGET_OS_IPHONE


static void
reachPerform(void *info)
{
	void				*context_info;
	void				(*context_release)(const void *);
	uint64_t			cycle;
	Boolean				defer		= FALSE;
	Boolean				forced;
	Boolean				ok;
	ReachabilityInfo		reach_info	= NOT_REACHABLE;
	SCNetworkReachabilityCallBack	rlsFunction;
	ReachabilityStoreInfo		store_info;
	SCNetworkReachabilityRef	target		= (SCNetworkReachabilityRef)info;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	SCLog(_sc_debug, LOG_INFO, CFSTR("%schecking target reachability"),
	      targetPrivate->log_prefix);


	MUTEX_LOCK(&targetPrivate->lock);

	if (targetPrivate->dnsRetry != NULL) {
		// cancel DNS retry
		dequeueAsyncDNSRetry(target);
	}

	if (!targetPrivate->scheduled) {
		// if not currently scheduled
		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	}

	/* update reachability, notify if status changed */
	ReachabilityStoreInfo_init(&store_info);
	ok = __SCNetworkReachabilityGetFlags(&store_info, target, &reach_info, TRUE);
	ReachabilityStoreInfo_free(&store_info);
	if (!ok) {
		/* if reachability status not available */
		SCLog(_sc_debug, LOG_INFO, CFSTR("%flags not available"),
		      targetPrivate->log_prefix);
		reach_info = NOT_REACHABLE;
	}

#if	!TARGET_OS_IPHONE
	/*
	 * We want to defer the notification if this is a maintenance wake *and*
	 * the reachability flags that we would be reporting to the application
	 * are better than those that we last reported.
	 */
	if (!systemIsAwake(power_capabilities)) {
		/* if this is a maintenace wake */
		reach_info.sleeping = TRUE;

		if (rankReachability(reach_info.flags) >= rankReachability(targetPrivate->info.flags)) {
			/*
			 * don't report the change if the new reachability flags are
			 * the same or "better"
			 */
			defer = !darkWakeNotify(target);
		} else if (!__reach_changed(&targetPrivate->last_notify, &reach_info)) {
			/*
			 * if we have already posted this change
			 */
			defer = !darkWakeNotify(target);
		}
	}
#endif	// !TARGET_OS_IPHONE

	cycle = targetPrivate->cycle;
	forced = ((cycle != 0) && (targetPrivate->info.cycle != cycle));

	if (!forced && !__reach_changed(&targetPrivate->info, &reach_info)) {
		if (_sc_debug) {
			if (targetPrivate->info.sleeping == reach_info.sleeping) {
				SCLog(TRUE, LOG_INFO,
				      CFSTR("%sflags/interface match (now 0x%08x/%hu%s)"),
				      targetPrivate->log_prefix,
				      reach_info.flags,
				      reach_info.if_index,
				      reach_info.sleeping ? ", z" : "");
			} else {
				SCLog(TRUE, LOG_INFO,
				      CFSTR("%sflags/interface equiv (was 0x%08x/%hu%s, now 0x%08x/%hu%s)"),
				      targetPrivate->log_prefix,
				      targetPrivate->info.flags,
				      targetPrivate->info.if_index,
				      targetPrivate->info.sleeping ? ", z" : "",
				      reach_info.flags,
				      reach_info.if_index,
				      reach_info.sleeping ? ", z" : "");
			}

		}
		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	}

	SCLog(_sc_debug, LOG_INFO,
	      CFSTR("%sflags/interface have changed (was 0x%08x/%hu%s, now 0x%08x/%hu%s)%s%s"),
	      targetPrivate->log_prefix,
	      targetPrivate->info.flags,
	      targetPrivate->info.if_index,
	      targetPrivate->info.sleeping ? ", z" : "",
	      reach_info.flags,
	      reach_info.if_index,
	      reach_info.sleeping ? ", z" : "",
	      defer ? ", deferred" : "",
	      forced ? ", forced" : "");

	/* as needed, defer the notification */
	if (defer) {
		MUTEX_UNLOCK(&targetPrivate->lock);
		return;
	}

	/* update flags / interface */
	_reach_set(&targetPrivate->info, &reach_info, cycle);

	/* save last notification info */
	_reach_set(&targetPrivate->last_notify, &reach_info, cycle);

	/* save last notification time */
	(void)gettimeofday(&targetPrivate->last_push, NULL);

	/* callout */
	rlsFunction = targetPrivate->rlsFunction;
	if (targetPrivate->rlsContext.retain != NULL) {
		context_info	= (void *)(*targetPrivate->rlsContext.retain)(targetPrivate->rlsContext.info);
		context_release	= targetPrivate->rlsContext.release;
	} else {
		context_info	= targetPrivate->rlsContext.info;
		context_release	= NULL;
	}

	MUTEX_UNLOCK(&targetPrivate->lock);

	if (rlsFunction != NULL) {
		(*rlsFunction)(target,
			       reach_info.flags & ~kSCNetworkReachabilityFlagsFirstResolvePending,
			       context_info);
	}

	if (context_release != NULL) {
		(*context_release)(context_info);
	}

	return;
}


Boolean
SCNetworkReachabilitySetCallback(SCNetworkReachabilityRef	target,
				 SCNetworkReachabilityCallBack	callout,
				 SCNetworkReachabilityContext	*context)
{
	SCNetworkReachabilityPrivateRef	targetPrivate = (SCNetworkReachabilityPrivateRef)target;

	MUTEX_LOCK(&targetPrivate->lock);

	if (targetPrivate->rlsContext.release != NULL) {
		/* let go of the current context */
		(*targetPrivate->rlsContext.release)(targetPrivate->rlsContext.info);
	}

	targetPrivate->rlsFunction   			= callout;
	targetPrivate->rlsContext.info			= NULL;
	targetPrivate->rlsContext.retain		= NULL;
	targetPrivate->rlsContext.release		= NULL;
	targetPrivate->rlsContext.copyDescription	= NULL;
	if (context) {
		bcopy(context, &targetPrivate->rlsContext, sizeof(SCNetworkReachabilityContext));
		if (context->retain != NULL) {
			targetPrivate->rlsContext.info = (void *)(*context->retain)(context->info);
		}
	}

	MUTEX_UNLOCK(&targetPrivate->lock);

	return TRUE;
}


static CFStringRef
reachRLSCopyDescription(const void *info)
{
	SCNetworkReachabilityRef		target	= (SCNetworkReachabilityRef)info;

	return CFStringCreateWithFormat(NULL,
					NULL,
					CFSTR("<SCNetworkReachability RLS> {target = %p}"),
					target);
}


static Boolean
__SCNetworkReachabilityScheduleWithRunLoop(SCNetworkReachabilityRef	target,
					   CFRunLoopRef			runLoop,
					   CFStringRef			runLoopMode,
					   dispatch_queue_t		queue,
					   Boolean			onDemand)
{
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;
	Boolean				init		= FALSE;
	__block Boolean			ok		= FALSE;

	MUTEX_LOCK(&targetPrivate->lock);

	if ((targetPrivate->dispatchQueue != NULL) ||		// if we are already scheduled with a dispatch queue
	    ((queue != NULL) && targetPrivate->scheduled)) {	// if we are already scheduled on a CFRunLoop
		_SCErrorSet(kSCStatusInvalidArgument);
		goto done;
	}

#ifdef	HAVE_REACHABILITY_SERVER
	if (!targetPrivate->serverBypass) {
		if (!targetPrivate->serverActive) {
			ok = __SCNetworkReachabilityServer_targetAdd(target);
			if (!ok) {
				targetPrivate->serverBypass = TRUE;
			}
		}

		if (targetPrivate->serverActive) {
			if (targetPrivate->scheduled) {
				// if already scheduled
				goto watch;
			}

			ok = __SCNetworkReachabilityServer_targetSchedule(target);
			if (!ok) {
				SCLog(TRUE, LOG_DEBUG,
				      CFSTR("__SCNetworkReachabilityScheduleWithRunLoop _targetMonitor() failed"));
				_SCErrorSet(kSCStatusFailed);
				goto done;
			}

			goto watch;
		}
	}
#endif	// HAVE_REACHABILITY_SERVER

	/* schedule the SCNetworkReachability did-something-change handler */

	dispatch_sync(_hn_queue(), ^{
		ok = FALSE;

		if (!onDemand && (hn_store == NULL)) {
			/*
			 * if we are not monitoring any hosts, start watching
			 */
			if (!dns_configuration_watch()) {
				// if error
				_SCErrorSet(kSCStatusFailed);
				return;
			}

			hn_store = SCDynamicStoreCreate(NULL,
							CFSTR("SCNetworkReachability"),
							__SCNetworkReachabilityHandleChanges,
							NULL);
			if (hn_store == NULL) {
				SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreCreate() failed"));
				dns_configuration_unwatch();
				return;
			}

			__SCNetworkReachabilityReachabilitySetNotifications(hn_store);

			hn_dispatchQueue = dispatch_queue_create("SCNetworkReachabilty.changes", NULL);
			if (hn_dispatchQueue == NULL) {
				SCLog(TRUE, LOG_ERR, CFSTR("__SCNetworkReachabilityScheduleWithRunLoop dispatch_queue_create() failed"));
				CFRelease(hn_store);
				hn_store = NULL;
				dns_configuration_unwatch();
				_SCErrorSet(kSCStatusFailed);
				return;
			}

			ok = SCDynamicStoreSetDispatchQueue(hn_store, hn_dispatchQueue);
			if (!ok) {
				SCLog(TRUE, LOG_ERR, CFSTR("SCDynamicStoreSetDispatchQueue() failed"));
				dispatch_release(hn_dispatchQueue);
				hn_dispatchQueue = NULL;
				CFRelease(hn_store);
				hn_store = NULL;
				dns_configuration_unwatch();
				return;
			}
			hn_targets = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

			ReachabilityStoreInfo_enable(TRUE);
		}

		CFSetAddValue(hn_targets, target);

		ok = TRUE;
	});

	if (!ok) {
		goto done;
	}

#ifdef	HAVE_REACHABILITY_SERVER
    watch :
#endif	// HAVE_REACHABILITY_SERVER

	if (!targetPrivate->scheduled) {
		CFRunLoopSourceContext	context = { 0				// version
						  , (void *)target		// info
						  , CFRetain			// retain
						  , CFRelease			// release
						  , reachRLSCopyDescription	// copyDescription
						  , CFEqual			// equal
						  , CFHash			// hash
						  , NULL			// schedule
						  , NULL			// cancel
						  , reachPerform		// perform
						  };

		if (runLoop != NULL) {
			targetPrivate->rls    = CFRunLoopSourceCreate(NULL, 0, &context);
			targetPrivate->rlList = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		}

		if (targetPrivate->type == reachabilityTypeName) {
			/*
			 * we're now scheduled so let's ensure that we
			 * are starting with a clean slate before we
			 * resolve the name
			 */
			if (targetPrivate->resolvedAddress != NULL) {
				CFRelease(targetPrivate->resolvedAddress);
				targetPrivate->resolvedAddress = NULL;
			}
			targetPrivate->resolvedAddressError = NETDB_SUCCESS;
			targetPrivate->needResolve = TRUE;
			_reach_set(&targetPrivate->info, &NOT_REACHABLE, targetPrivate->info.cycle);
			targetPrivate->info.flags |= kSCNetworkReachabilityFlagsFirstResolvePending;
#ifdef	HAVE_REACHABILITY_SERVER
			_reach_set(&targetPrivate->serverInfo, &NOT_REACHABLE, targetPrivate->serverInfo.cycle);
			targetPrivate->serverInfo.flags |= kSCNetworkReachabilityFlagsFirstResolvePending;
#endif	// HAVE_REACHABILITY_SERVER
		}

		targetPrivate->scheduled = TRUE;

		init = TRUE;
	}

	if (queue != NULL) {
		// retain dispatch queue
		dispatch_retain(queue);
		targetPrivate->dispatchQueue = queue;

		//
		// We've taken a reference to the client's dispatch_queue and we
		// want to hold on to that reference until we've processed any/all
		// notifications.  To facilitate this we create a group, dispatch
		// any notification blocks to via that group, and when the caller
		// has told us to stop the notifications (unschedule) we wait for
		// the group to empty and use the group's finalizer to release
		// our reference to the client's queue.
		//

		// make sure that we have group to track any async requests
		targetPrivate->dispatchGroup = dispatch_group_create();

		// retain the target ... and release it when the group is released
		CFRetain(target);
		dispatch_set_context(targetPrivate->dispatchGroup, (void *)target);
		dispatch_set_finalizer_f(targetPrivate->dispatchGroup, (dispatch_function_t)CFRelease);
	} else {
		if (!_SC_isScheduled(NULL, runLoop, runLoopMode, targetPrivate->rlList)) {
			/*
			 * if we do not already have host notifications scheduled with
			 * this runLoop / runLoopMode
			 */
			CFRunLoopAddSource(runLoop, targetPrivate->rls, runLoopMode);

			if (targetPrivate->dnsRLS != NULL) {
				// if we have an active async DNS query too
				CFRunLoopAddSource(runLoop, targetPrivate->dnsRLS, runLoopMode);
			}
		}

		_SC_schedule(target, runLoop, runLoopMode, targetPrivate->rlList);
	}

	if (init) {
		ReachabilityInfo	reach_info	= NOT_REACHABLE;
		ReachabilityStoreInfo	store_info;

		/*
		 * if we have yet to schedule SC notifications for this address
		 * - initialize current reachability status
		 */
		ReachabilityStoreInfo_init(&store_info);
		if (__SCNetworkReachabilityGetFlags(&store_info, target, &reach_info, TRUE)) {
			/*
			 * if reachability status available
			 * - set flags
			 * - schedule notification to report status via callback
			 */
#ifdef	HAVE_REACHABILITY_SERVER
			reach_info.flags |= (targetPrivate->info.flags & kSCNetworkReachabilityFlagsFirstResolvePending);
#endif	// HAVE_REACHABILITY_SERVER
			_reach_set(&targetPrivate->info, &reach_info, targetPrivate->cycle);
			__SCNetworkReachabilityPerform(target);
		} else {
			/* if reachability status not available, async lookup started */
			_reach_set(&targetPrivate->info, &NOT_REACHABLE, targetPrivate->cycle);
#ifdef	HAVE_REACHABILITY_SERVER
			_reach_set(&targetPrivate->serverInfo, &NOT_REACHABLE, targetPrivate->cycle);
#endif	// HAVE_REACHABILITY_SERVER
		}
		ReachabilityStoreInfo_free(&store_info);
	}

	if (targetPrivate->onDemandServer != NULL) {
		__SCNetworkReachabilityScheduleWithRunLoop(targetPrivate->onDemandServer, runLoop, runLoopMode, queue, TRUE);
	}

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%sscheduled"),
	      targetPrivate->log_prefix);

	ok = TRUE;

    done :

	MUTEX_UNLOCK(&targetPrivate->lock);
	return ok;
}


static Boolean
__SCNetworkReachabilityUnscheduleFromRunLoop(SCNetworkReachabilityRef	target,
					     CFRunLoopRef		runLoop,
					     CFStringRef		runLoopMode,
					     Boolean			onDemand)
{
	dispatch_group_t		drainGroup	= NULL;
	dispatch_queue_t		drainQueue	= NULL;
	CFIndex				n		= 0;
	Boolean				ok		= FALSE;
	SCNetworkReachabilityPrivateRef	targetPrivate	= (SCNetworkReachabilityPrivateRef)target;

	// hold a reference while we unschedule
	CFRetain(target);

	MUTEX_LOCK(&targetPrivate->lock);

	if (((runLoop == NULL) && (targetPrivate->dispatchQueue == NULL)) ||	// if we should be scheduled on a dispatch queue (but are not)
	    ((runLoop != NULL) && (targetPrivate->dispatchQueue != NULL))) {	// if we should be scheduled on a CFRunLoop (but are not)
		_SCErrorSet(kSCStatusInvalidArgument);
		goto done;
	}

	if (!targetPrivate->scheduled) {
		// if not currently scheduled
		_SCErrorSet(kSCStatusInvalidArgument);
		goto done;
	}

	// unschedule the target specific sources
	if (targetPrivate->dispatchQueue != NULL) {
		if (targetPrivate->onDemandServer != NULL) {
			__SCNetworkReachabilityUnscheduleFromRunLoop(targetPrivate->onDemandServer, NULL, NULL, TRUE);
		}

		// save dispatchQueue, release reference when we've queue'd blocks complete, allow re-scheduling
		drainGroup = targetPrivate->dispatchGroup;
		targetPrivate->dispatchGroup = NULL;
		drainQueue = targetPrivate->dispatchQueue;
		targetPrivate->dispatchQueue = NULL;
	} else {
		if (!_SC_unschedule(target, runLoop, runLoopMode, targetPrivate->rlList, FALSE)) {
			// if not currently scheduled
			_SCErrorSet(kSCStatusInvalidArgument);
			goto done;
		}

		if (targetPrivate->onDemandServer != NULL) {
			__SCNetworkReachabilityUnscheduleFromRunLoop(targetPrivate->onDemandServer, runLoop, runLoopMode, TRUE);
		}

		n = CFArrayGetCount(targetPrivate->rlList);
		if ((n == 0) || !_SC_isScheduled(NULL, runLoop, runLoopMode, targetPrivate->rlList)) {
			// if target is no longer scheduled for this runLoop / runLoopMode
			CFRunLoopRemoveSource(runLoop, targetPrivate->rls, runLoopMode);

			if (targetPrivate->dnsRLS != NULL) {
				// if we have an active async DNS query too
				CFRunLoopRemoveSource(runLoop, targetPrivate->dnsRLS, runLoopMode);
			}

			if (n == 0) {
				// if *all* notifications have been unscheduled
				CFRelease(targetPrivate->rlList);
				targetPrivate->rlList = NULL;
				CFRunLoopSourceInvalidate(targetPrivate->rls);
				CFRelease(targetPrivate->rls);
				targetPrivate->rls = NULL;
			}
		}
	}

	if (n == 0) {
#ifdef	HAVE_REACHABILITY_SERVER
		//
		// Cancel our request for server monitoring
		//
		if (targetPrivate->serverActive) {
			ok = __SCNetworkReachabilityServer_targetUnschedule(target);
			if (!ok) {
				SCLog(TRUE, LOG_DEBUG,
				      CFSTR("__SCNetworkReachabilityUnscheduleFromRunLoop _targetMonitor() failed"));
				_SCErrorSet(kSCStatusFailed);
			}
		}
#endif	// HAVE_REACHABILITY_SERVER

		// if *all* notifications have been unscheduled
		targetPrivate->scheduled = FALSE;
	}

#ifdef	HAVE_REACHABILITY_SERVER
	if (targetPrivate->serverActive) {
		goto unwatch;
	}
#endif	// HAVE_REACHABILITY_SERVER

	if (n == 0) {
		if (targetPrivate->dnsMP != MACH_PORT_NULL) {
			// if we have an active async DNS query
			dequeueAsyncDNSQuery(target, TRUE);
		}

		if (targetPrivate->dnsRetry != NULL) {
			// if we have an outstanding DNS retry
			dequeueAsyncDNSRetry(target);
		}

		if (targetPrivate->llqActive) {
			// if we have a long-lived-query
			dequeueLongLivedQuery(target);
		}

		dispatch_sync(_hn_queue(), ^{
			CFSetRemoveValue(hn_targets, target);

			if (onDemand) {
				return;
			}

			if (CFSetGetCount(hn_targets) > 0) {
				return;
			}

			// if we are no longer monitoring any targets
			SCDynamicStoreSetDispatchQueue(hn_store, NULL);
			dispatch_release(hn_dispatchQueue);
			hn_dispatchQueue = NULL;
			CFRelease(hn_store);
			hn_store = NULL;
			CFRelease(hn_targets);
			hn_targets = NULL;

			ReachabilityStoreInfo_enable(FALSE);
			ReachabilityStoreInfo_save(NULL);

			/*
			 * until we start monitoring again, ensure that
			 * any resources associated with tracking the
			 * DNS configuration have been released.
			 */
			dns_configuration_unwatch();
		});
	}

#ifdef	HAVE_REACHABILITY_SERVER
    unwatch :
#endif	// HAVE_REACHABILITY_SERVER

	SCLog((_sc_debug && (_sc_log > 0)), LOG_INFO, CFSTR("%sunscheduled"),
	      targetPrivate->log_prefix);

	ok = TRUE;

    done :

	MUTEX_UNLOCK(&targetPrivate->lock);

	if (drainGroup != NULL) {
		dispatch_group_notify(drainGroup, __SCNetworkReachability_concurrent_queue(), ^{
			// release group/queue references
			dispatch_release(drainQueue);
			dispatch_release(drainGroup);	// releases our target reference
		});
	}

	// release our reference
	CFRelease(target);

	return ok;
}

Boolean
SCNetworkReachabilityScheduleWithRunLoop(SCNetworkReachabilityRef	target,
					 CFRunLoopRef			runLoop,
					 CFStringRef			runLoopMode)
{
	if (!isA_SCNetworkReachability(target) || (runLoop == NULL) || (runLoopMode == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	return __SCNetworkReachabilityScheduleWithRunLoop(target, runLoop, runLoopMode, NULL, FALSE);
}

Boolean
SCNetworkReachabilityUnscheduleFromRunLoop(SCNetworkReachabilityRef	target,
					   CFRunLoopRef			runLoop,
					   CFStringRef			runLoopMode)
{
	if (!isA_SCNetworkReachability(target) || (runLoop == NULL) || (runLoopMode == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	return __SCNetworkReachabilityUnscheduleFromRunLoop(target, runLoop, runLoopMode, FALSE);
}

Boolean
SCNetworkReachabilitySetDispatchQueue(SCNetworkReachabilityRef	target,
				      dispatch_queue_t		queue)
{
	Boolean	ok	= FALSE;

	if (!isA_SCNetworkReachability(target)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (queue != NULL) {
		ok = __SCNetworkReachabilityScheduleWithRunLoop(target, NULL, NULL, queue, FALSE);
	} else {
		ok = __SCNetworkReachabilityUnscheduleFromRunLoop(target, NULL, NULL, FALSE);
	}

	return ok;
}
