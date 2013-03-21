/*
 * Copyright (c) 2004-2005 Apple Computer, Inc. All rights reserved.
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
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb_async.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStoreCopyDHCPInfo.h>
#include <SystemConfiguration/SCValidation.h>
#include <SystemConfiguration/SCPrivate.h>	// for SCLog(), SCPrint()

#include <notify.h>


static SCDynamicStoreRef	store		= NULL;
static CFRunLoopSourceRef	rls		= NULL;

static CFMachPortRef		dnsPort		= NULL;
static CFRunLoopSourceRef	dnsRLS		= NULL;
static struct timeval		dnsQueryStart;

static Boolean			_verbose	= FALSE;


/* SPI (from SCNetworkReachability.c) */
Boolean
_SC_checkResolverReachability(SCDynamicStoreRef         *storeP,
			      SCNetworkConnectionFlags  *flags,
			      Boolean                   *haveDNS,
			      const char *              nodename);


/*
 * checkResolverReachabilityByAddress()
 *
 * Given an IP address, determine whether a reverse DNS query can be issued
 * using the current network configuration.
 */
static Boolean
checkResolverReachabilityByAddress(SCDynamicStoreRef store, struct sockaddr *sa)
{
	SCNetworkConnectionFlags	flags;
	Boolean				haveDNS;
	int				i;
	Boolean				ok		= FALSE;
	char				ptr_name[128];

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
			struct sockaddr_in	*sin	= (struct sockaddr_in *)sa;

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
			struct sockaddr_in6	*sin6	= (struct sockaddr_in6 *)sa;
			int			x	= sizeof(ptr_name);
			int			n;

#define	USE_NIBBLE_QUERY
#ifdef	USE_NIBBLE_QUERY
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
#else	/* USE_NIBBLE_QUERY */
			/*
			 * build IPv6 "bit-string" PTR query name (RFC 2673)
			 *   \[xNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN].ip6.arpa.
			 */
			n = snprintf(&ptr_name[0], x, "\\[x");
			if ((n == -1) || (n >= x)) {
				goto done;
			}

			s += n;
			x -= n;
			for (i = 0; i < 16; i++) {
				n = snprintf(&ptr_name[s], x, "%2.2x", sin6->sin6_addr.s6_addr[i]);
				if ((n == -1) || (n >= x)) {
					goto done;
				}

				s += n;
				x -= n;
			}

			n = snprintf(&ptr_name[s], x, "].ip6.arpa.");
			if ((n == -1) || (n >= x)) {
				goto done;
			}
#endif	/* USE_NIBBLE_QUERY */

			break;
		}

		default :
			goto done;
	}

	ok = _SC_checkResolverReachability(&store, &flags, &haveDNS, ptr_name);
	if (ok) {
		if (!(flags & kSCNetworkFlagsReachable) ||
			(flags & kSCNetworkFlagsConnectionRequired)) {
			// if not reachable *OR* connection required
			ok = FALSE;
		}
	}

    done :

	return ok;
}


#define	HOSTNAME_NOTIFY_KEY	"com.apple.system.hostname"


static void
set_hostname(CFStringRef hostname)
{
	if (hostname != NULL) {
		char	old_name[MAXHOSTNAMELEN];
		char	new_name[MAXHOSTNAMELEN];

		if (gethostname(old_name, sizeof(old_name)) == -1) {
			SCLog(TRUE, LOG_ERR, CFSTR("gethostname() failed: %s"), strerror(errno));
			old_name[0] = '\0';
		}

		if (_SC_cfstring_to_cstring(hostname,
					    new_name,
					    sizeof(new_name),
					    kCFStringEncodingUTF8) == NULL) {
			SCLog(TRUE, LOG_ERR, CFSTR("could not convert [new] hostname"));
			new_name[0] = '\0';
		}

		old_name[sizeof(old_name)-1] = '\0';
		new_name[sizeof(new_name)-1] = '\0';
		if (strcmp(old_name, new_name) != 0) {
			if (sethostname(new_name, strlen(new_name)) == 0) {
				uint32_t	status;

				SCLog(TRUE, LOG_NOTICE,
				      CFSTR("setting hostname to \"%s\""),
				      new_name);

				status = notify_post(HOSTNAME_NOTIFY_KEY);
				if (status != NOTIFY_STATUS_OK) {
					SCLog(TRUE, LOG_ERR,
					      CFSTR("notify_post(" HOSTNAME_NOTIFY_KEY ") failed: error=%lu"),
					      status);
				}
			} else {
				SCLog(TRUE, LOG_ERR,
				      CFSTR("sethostname(%s, %d) failed: %s"),
				      new_name,
				      strlen(new_name),
				      strerror(errno));
			}
		}
	}

	return;
}


#define	HOSTCONFIG	"/etc/hostconfig"
#define HOSTNAME_KEY	"HOSTNAME="
#define	AUTOMATIC	"-AUTOMATIC-"

#define HOSTNAME_KEY_LEN	(sizeof(HOSTNAME_KEY) - 1)

static CFStringRef
copy_static_name()
{
	FILE *		f;
	char		buf[256];
	CFStringRef	name	= NULL;

	f = fopen(HOSTCONFIG, "r");
	if (f == NULL) {
		return NULL;
	}

	while (fgets(buf, sizeof(buf), f) != NULL) {
		char *	bp;
		int	n;
		char *	np;
		Boolean	str_escape;
		Boolean	str_quote;

		n = strlen(buf);
		if (buf[n-1] == '\n') {
			/* the entire line fit in the buffer, remove the newline */
			buf[n-1] = '\0';
		} else {
			/* eat the remainder of the line */
			do {
				n = fgetc(f);
			} while ((n != '\n') && (n != EOF));
		}

		// skip leading white space
		bp = &buf[0];
		while (isspace(*bp)) {
			bp++;
		}

		// find "HOSTNAME=" key
		if (strncmp(bp, HOSTNAME_KEY, HOSTNAME_KEY_LEN) != 0) {
			continue;	// if not
		}

		// get the hostname string
		bp += HOSTNAME_KEY_LEN;
		str_escape = FALSE;
		str_quote  = FALSE;

		np = &buf[0];
		while (*bp != '\0') {
			char	ch = *bp;

			switch (ch) {
				case '\\' :
					if (!str_escape) {
						str_escape = TRUE;
						bp++;
						continue;
					}
					break;
				case '"' :
					if (!str_escape) {
						str_quote = !str_quote;
						bp++;
						continue;
					}
					break;
				default :
					break;
			}

			if (str_escape) {
				str_escape = FALSE;
			} else if (!str_quote && (isspace(ch) || (ch == '#'))) {
				break;
			}

			*np++ = ch;
			bp++;
		}

		*np = '\0';

		if (name != NULL) {
			CFRelease(name);
			name = NULL;
		}

		if (str_quote) {
			// the shell won't parse this file so neither will we
			break;
		}

		if (strcmp(buf, AUTOMATIC) == 0) {
			// skip "-AUTOMATIC-"
			continue;
		}

		name = CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
	}

	(void) fclose(f);
	return name;
}


#ifndef	kSCPropNetHostName
#define	kSCPropNetHostName	CFSTR("HostName")
#endif


static CFStringRef
copy_prefs_hostname(SCDynamicStoreRef store)
{
	CFDictionaryRef		dict;
	CFStringRef		key;
	CFStringRef		name		= NULL;

	key  = SCDynamicStoreKeyCreateComputerName(NULL);
	dict = SCDynamicStoreCopyValue(store, key);
	CFRelease(key);
	if (dict == NULL) {
		goto done;
	}
	if (!isA_CFDictionary(dict)) {
		goto done;
	}

	name = isA_CFString(CFDictionaryGetValue(dict, kSCPropNetHostName));
	if (name == NULL) {
		goto done;
	}
	CFRetain(name);

    done :

	if (dict != NULL)	CFRelease(dict);
	
	return name;
}


static CFStringRef
copy_primary_service(SCDynamicStoreRef store)
{
	CFDictionaryRef	dict;
	CFStringRef	key;
	CFStringRef	serviceID	= NULL;

	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetIPv4);
	dict = SCDynamicStoreCopyValue(store, key);
	CFRelease(key);

	if (dict != NULL) {
		if (isA_CFDictionary(dict)) {
			serviceID = CFDictionaryGetValue(dict, kSCDynamicStorePropNetPrimaryService);
			if (isA_CFString(serviceID)) {
				CFRetain(serviceID);
			} else {
				serviceID = NULL;
			}
		}
		CFRelease(dict);
	}

	return serviceID;
}


static CFStringRef
copy_primary_ip(SCDynamicStoreRef store, CFStringRef serviceID)
{
	CFDictionaryRef	dict;
	CFStringRef	key;
	CFStringRef	address	= NULL;

	key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							  kSCDynamicStoreDomainState,
							  serviceID,
							  kSCEntNetIPv4);
	dict = SCDynamicStoreCopyValue(store, key);
	CFRelease(key);

	if (dict != NULL) {
		if (isA_CFDictionary(dict)) {
			CFArrayRef	addresses;

			addresses = CFDictionaryGetValue(dict, kSCPropNetIPv4Addresses);
			if (isA_CFArray(addresses) && (CFArrayGetCount(addresses) > 0)) {
				address = CFArrayGetValueAtIndex(addresses, 0);
				if (isA_CFString(address)) {
					CFRetain(address);
				} else {
					address = NULL;
				}
			}
		}
		CFRelease(dict);
	}

	return address;
}


#define	DHCP_OPTION_HOSTNAME	12

static CFStringRef
copy_dhcp_name(SCDynamicStoreRef store, CFStringRef serviceID)
{
	CFDictionaryRef	info;
	CFStringRef	name	= NULL;

	info = SCDynamicStoreCopyDHCPInfo(store, serviceID);
	if (info != NULL) {
		CFDataRef	data;

		data = DHCPInfoGetOptionData(info, DHCP_OPTION_HOSTNAME);
		if (data != NULL) {
			name = CFStringCreateFromExternalRepresentation(NULL, data, kCFStringEncodingUTF8);
		}

		CFRelease(info);
	}

	return name;
}


static void
reverseDNSComplete(int32_t status, char *host, char *serv, void *context)
{
	struct timeval		dnsQueryComplete;
	struct timeval		dnsQueryElapsed;
	CFStringRef		hostname;
	SCDynamicStoreRef	store	= (SCDynamicStoreRef)context;

	(void) gettimeofday(&dnsQueryComplete, NULL);
	timersub(&dnsQueryComplete, &dnsQueryStart, &dnsQueryElapsed);
	SCLog(_verbose, LOG_INFO,
	      CFSTR("async DNS complete%s (query time = %d.%3.3d)"),
	      ((status == 0) && (host != NULL)) ? "" : ", host not found",
	      dnsQueryElapsed.tv_sec,
	      dnsQueryElapsed.tv_usec / 1000);

	// use reverse DNS name, if available

	switch (status) {
		case 0 :
			/*
			 * if [reverse] DNS query was successful
			 */
			if (host != NULL) {
				hostname = CFStringCreateWithCString(NULL, host, kCFStringEncodingUTF8);
				SCLog(TRUE, LOG_INFO, CFSTR("hostname (reverse DNS query) = %@"), hostname);
				set_hostname(hostname);
				CFRelease(hostname);
				goto done;
			}
			break;

		case EAI_NONAME :
			/*
			 * if no name available
			 */
			break;

		default :
			/*
			 * Hmmmm...
			 */
			SCLog(TRUE, LOG_ERR, CFSTR("getnameinfo() failed: %s"), gai_strerror(status));
	}

	// get local (multicast DNS) name, if available

	hostname = SCDynamicStoreCopyLocalHostName(store);
	if (hostname != NULL) {
		CFMutableStringRef	localName;

		SCLog(TRUE, LOG_INFO, CFSTR("hostname (multicast DNS) = %@"), hostname);
		localName = CFStringCreateMutableCopy(NULL, 0, hostname);
		CFStringAppend(localName, CFSTR(".local"));
		set_hostname(localName);
		CFRelease(localName);
		CFRelease(hostname);
		goto done;
	}

	// use "localhost" if not other name is available

	set_hostname(CFSTR("localhost"));

    done :

	if (host != NULL)	free(host);
	if (serv != NULL)	free(serv);
	return;
}


static void
getnameinfo_async_handleCFReply(CFMachPortRef port, void *msg, CFIndex size, void *info)
{
	getnameinfo_async_handle_reply(msg);

	if (port == dnsPort) {
		CFRelease(dnsRLS);
		dnsRLS = NULL;
		CFRelease(dnsPort);
		dnsPort = NULL;
	}

	return;
}


static void
start_dns_query(SCDynamicStoreRef store, CFStringRef address)
{
	char			addr[64];
	Boolean			ok;
	struct sockaddr		*sa;
	struct sockaddr_in	sin;
	struct sockaddr_in6	sin6;

	if (_SC_cfstring_to_cstring(address, addr, sizeof(addr), kCFStringEncodingASCII) == NULL) {
		SCLog(TRUE, LOG_ERR, CFSTR("could not convert [primary] address"));
		return;
	}

	bzero(&sin, sizeof(sin));
	sin.sin_len    = sizeof(sin);
	sin.sin_family = AF_INET;

	bzero(&sin6, sizeof(sin6));
	sin6.sin6_len    = sizeof(sin6);
	sin6.sin6_family = AF_INET6;

	if (inet_aton(addr, &sin.sin_addr) == 1) {
		/*
		 * if IPv4 address
		 */
		sa = (struct sockaddr *)&sin;
	} else if (inet_pton(AF_INET6, addr, &sin6.sin6_addr) == 1) {
		/*
		 * if IPv6 address
		 */
		char	*p;

		p = strchr(addr, '%');
		if (p != NULL) {
			sin6.sin6_scope_id = if_nametoindex(p+1);
		}

		sa = (struct sockaddr *)&sin6;
	} else {
		goto done;
	}

	ok = checkResolverReachabilityByAddress(store, sa);
	if (ok) {
		CFMachPortContext	context	= { 0, (void *)store, CFRetain, CFRelease, CFCopyDescription };
		mach_port_t		port;
		int32_t			error;

		if ((dnsPort != NULL) && (dnsRLS != NULL)) {
			/* if we already have an active async DNS query */
			lu_async_call_cancel(CFMachPortGetPort(dnsPort));
			CFRelease(dnsRLS);
			dnsRLS = NULL;
			CFRelease(dnsPort);
			dnsPort = NULL;
		}

		(void) gettimeofday(&dnsQueryStart, NULL);

		error = getnameinfo_async_start(&port,
						sa,
						sa->sa_len,
						0,		// flags
						reverseDNSComplete,
						NULL);
		if (error != 0) {
			goto done;
		}

		dnsPort = CFMachPortCreateWithPort(NULL,
						   port,
						   getnameinfo_async_handleCFReply,
						   &context,
						   NULL);
		dnsRLS = CFMachPortCreateRunLoopSource(NULL, dnsPort, 0);
		CFRunLoopAddSource(CFRunLoopGetCurrent(), dnsRLS, kCFRunLoopDefaultMode);
	}

    done :

	return;
}


static void
update_hostname(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info)
{
	CFStringRef	address		= NULL;
	CFStringRef	hostname	= NULL;
	CFStringRef	serviceID	= NULL;

	// get static hostname, if available

	hostname = copy_static_name();
	if (hostname != NULL) {
		SCLog(TRUE, LOG_INFO, CFSTR("hostname (static) = %@"), hostname);
		set_hostname(hostname);
		goto done;
	}

	// get [prefs] hostname, if available

	hostname = copy_prefs_hostname(store);
	if (hostname != NULL) {
		SCLog(TRUE, LOG_INFO, CFSTR("hostname (prefs) = %@"), hostname);
		set_hostname(hostname);
		goto done;
	}

	// get primary service ID

	serviceID = copy_primary_service(store);
	if (serviceID == NULL) {
		goto mDNS;
	}

	// get DHCP provided name, if available

	hostname = copy_dhcp_name(store, serviceID);
	if (hostname != NULL) {
		SCLog(TRUE, LOG_INFO, CFSTR("hostname (DHCP) = %@"), hostname);
		set_hostname(hostname);
		goto done;
	}

	// get DNS name associated with primary IP, if available

	address = copy_primary_ip(store, serviceID);
	if (address != NULL) {
		// start reverse DNS query using primary IP address
		(void) start_dns_query(store, address);
		goto done;
	}

    mDNS :

	// get local (multicast DNS) name, if available

	hostname = SCDynamicStoreCopyLocalHostName(store);
	if (hostname != NULL) {
		CFMutableStringRef	localName;

		SCLog(TRUE, LOG_INFO, CFSTR("hostname (multicast DNS) = %@"), hostname);
		localName = CFStringCreateMutableCopy(NULL, 0, hostname);
		CFStringAppend(localName, CFSTR(".local"));
		set_hostname(localName);
		CFRelease(localName);
		goto done;
	}

	// use "localhost" if not other name is available

	set_hostname(CFSTR("localhost"));

    done :

	if (address)	CFRelease(address);
	if (hostname)	CFRelease(hostname);
	if (serviceID)	CFRelease(serviceID);

	return;
}


__private_extern__
void
load_hostname(Boolean verbose)
{
	CFStringRef		key;
	CFMutableArrayRef	keys		= NULL;
	CFMutableArrayRef	patterns	= NULL;

	if (verbose) {
		_verbose = TRUE;
	}

	/* initialize a few globals */

	store = SCDynamicStoreCreate(NULL, CFSTR("set-hostname"), update_hostname, NULL);
	if (store == NULL) {
		SCLog(TRUE, LOG_ERR,
		      CFSTR("SCDynamicStoreCreate() failed: %s"),
		      SCErrorString(SCError()));
		goto error;
	}

	/* establish notification keys and patterns */

	keys     = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	patterns = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	/* ...watch for primary service / interface changes */
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetIPv4);
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	/* ...watch for DNS configuration changes */
	key = SCDynamicStoreKeyCreateNetworkGlobalEntity(NULL,
							 kSCDynamicStoreDomainState,
							 kSCEntNetDNS);
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	/* ...watch for (per-service) DHCP option changes */
	key = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL,
							  kSCDynamicStoreDomainState,
							  kSCCompAnyRegex,
							  kSCEntNetDHCP);
	CFArrayAppendValue(patterns, key);
	CFRelease(key);

	/* ...watch for (BSD) hostname changes */
	key = SCDynamicStoreKeyCreateComputerName(NULL);
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	/* ...watch for local (multicast DNS) hostname changes */
	key = SCDynamicStoreKeyCreateHostNames(NULL);
	CFArrayAppendValue(keys, key);
	CFRelease(key);

	/* register the keys/patterns */
	if (!SCDynamicStoreSetNotificationKeys(store, keys, patterns)) {
		SCLog(TRUE, LOG_ERR,
		      CFSTR("SCDynamicStoreSetNotificationKeys() failed: %s"),
		      SCErrorString(SCError()));
		goto error;
	}

	rls = SCDynamicStoreCreateRunLoopSource(NULL, store, 0);
	if (!rls) {
		SCLog(TRUE, LOG_ERR,
		      CFSTR("SCDynamicStoreCreateRunLoopSource() failed: %s"),
		      SCErrorString(SCError()));
		goto error;
	}
	CFRunLoopAddSource(CFRunLoopGetCurrent(), rls, kCFRunLoopDefaultMode);

	CFRelease(keys);
	CFRelease(patterns);
	return;

    error :

	if (keys != NULL)	CFRelease(keys);
	if (patterns != NULL)	CFRelease(patterns);
	if (store != NULL)	CFRelease(store);
	return;
}


#ifdef	MAIN
int
main(int argc, char **argv)
{

#ifdef	DEBUG

	_sc_log = FALSE;
	if ((argc > 1) && (strcmp(argv[1], "-d") == 0)) {
		_sc_verbose = TRUE;
		argv++;
		argc--;
	}

	CFStringRef		address;
	CFStringRef		hostname;
	CFStringRef		serviceID;
	SCDynamicStoreRef	store;

	store = SCDynamicStoreCreate(NULL, CFSTR("set-hostname"), NULL, NULL);
	if (store == NULL) {
		SCPrint(TRUE, stdout,
			CFSTR("SCDynamicStoreCreate() failed: %s\n"),
			SCErrorString(SCError()));
		exit(1);
	}

	// get static hostname
	hostname = copy_static_name();
	if (hostname != NULL) {
		SCPrint(TRUE, stdout, CFSTR("hostname (static) = %@\n"), hostname);
		CFRelease(hostname);
	}

	// get [prefs] hostname, if available
	hostname = copy_prefs_hostname(store);
	if (hostname != NULL) {
		SCPrint(TRUE, stdout, CFSTR("hostname (prefs) = %@\n"), hostname);
		CFRelease(hostname);
	}

	// get primary service
	serviceID = copy_primary_service(store);
	if (serviceID != NULL) {
		SCPrint(TRUE, stdout, CFSTR("primary service ID = %@\n"), serviceID);
	} else {
		SCPrint(TRUE, stdout, CFSTR("No primary service\n"));
		goto mDNS;
	}

	if ((argc == (2+1)) && (argv[1][0] == 's')) {
		if (serviceID != NULL)	CFRelease(serviceID);
		serviceID = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
		SCPrint(TRUE, stdout, CFSTR("alternate service ID = %@\n"), serviceID);
	}

	// get DHCP provided name
	hostname = copy_dhcp_name(store, serviceID);
	if (hostname != NULL) {
		SCPrint(TRUE, stdout, CFSTR("hostname (DHCP) = %@\n"), hostname);
		CFRelease(hostname);
	}

	// get primary IP address
	address = copy_primary_ip(store, serviceID);
	if (address != NULL) {
		SCPrint(TRUE, stdout, CFSTR("primary address = %@\n"), address);

		if ((argc == (2+1)) && (argv[1][0] == 'a')) {
			if (address != NULL)	CFRelease(address);
			address = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
			SCPrint(TRUE, stdout, CFSTR("alternate primary address = %@\n"), address);
		}

		// start reverse DNS query using primary IP address
		start_dns_query(store, address);
		CFRelease(address);
	}

	CFRelease(serviceID);

    mDNS :

	// get local (multicast DNS) name, if available

	hostname = SCDynamicStoreCopyLocalHostName(store);
	if (hostname != NULL) {
		CFMutableStringRef	localName;

		SCPrint(TRUE, stdout, CFSTR("hostname (multicast DNS) = %@\n"), hostname);
		localName = CFStringCreateMutableCopy(NULL, 0, hostname);
		CFStringAppend(localName, CFSTR(".local"));
		CFRelease(localName);
	}

	if (hostname != NULL)	CFRelease(hostname);

	update_hostname(store, NULL, NULL);

	CFRelease(store);

	CFRunLoopRun();

#else	/* DEBUG */

	_sc_log     = FALSE;
	_sc_verbose = (argc > 1) ? TRUE : FALSE;

	load_hostname((argc > 1) ? TRUE : FALSE);
	CFRunLoopRun();
	/* not reached */

#endif	/* DEBUG */

	exit(0);
	return 0;
}
#endif	/* MAIN */
