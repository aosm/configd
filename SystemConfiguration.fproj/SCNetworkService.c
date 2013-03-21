/*
 * Copyright (c) 2004-2007 Apple Inc. All rights reserved.
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
 * May 13, 2004		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */


#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFRuntime.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include "SCNetworkConfigurationInternal.h"
#include <SystemConfiguration/SCValidation.h>
#include <SystemConfiguration/SCPrivate.h>

#include <pthread.h>


static CFStringRef	__SCNetworkServiceCopyDescription	(CFTypeRef cf);
static void		__SCNetworkServiceDeallocate		(CFTypeRef cf);
static Boolean		__SCNetworkServiceEqual			(CFTypeRef cf1, CFTypeRef cf2);
static CFHashCode	__SCNetworkServiceHash			(CFTypeRef cf);


static CFTypeID __kSCNetworkServiceTypeID	= _kCFRuntimeNotATypeID;


static const CFRuntimeClass __SCNetworkServiceClass = {
	0,					// version
	"SCNetworkService",			// className
	NULL,					// init
	NULL,					// copy
	__SCNetworkServiceDeallocate,		// dealloc
	__SCNetworkServiceEqual,		// equal
	__SCNetworkServiceHash,			// hash
	NULL,					// copyFormattingDesc
	__SCNetworkServiceCopyDescription	// copyDebugDesc
};


static pthread_once_t		initialized	= PTHREAD_ONCE_INIT;


static CFStringRef
__SCNetworkServiceCopyDescription(CFTypeRef cf)
{
	CFAllocatorRef			allocator	= CFGetAllocator(cf);
	CFMutableStringRef		result;
	SCNetworkServicePrivateRef	servicePrivate	= (SCNetworkServicePrivateRef)cf;

	result = CFStringCreateMutable(allocator, 0);
	CFStringAppendFormat(result, NULL, CFSTR("<SCNetworkService %p [%p]> {"), cf, allocator);
	CFStringAppendFormat(result, NULL, CFSTR("id = %@"), servicePrivate->serviceID);
	CFStringAppendFormat(result, NULL, CFSTR(", prefs = %p"), servicePrivate->prefs);
	CFStringAppendFormat(result, NULL, CFSTR("}"));

	return result;
}


static void
__SCNetworkServiceDeallocate(CFTypeRef cf)
{
	SCNetworkServicePrivateRef	servicePrivate	= (SCNetworkServicePrivateRef)cf;

	/* release resources */

	CFRelease(servicePrivate->serviceID);
	if (servicePrivate->interface != NULL)	CFRelease(servicePrivate->interface);
	CFRelease(servicePrivate->prefs);

	return;
}


static Boolean
__SCNetworkServiceEqual(CFTypeRef cf1, CFTypeRef cf2)
{
	SCNetworkServicePrivateRef	s1	= (SCNetworkServicePrivateRef)cf1;
	SCNetworkServicePrivateRef	s2	= (SCNetworkServicePrivateRef)cf2;

	if (s1 == s2)
		return TRUE;

	if (s1->prefs != s2->prefs)
		return FALSE;   // if not the same prefs

	if (!CFEqual(s1->serviceID, s2->serviceID))
		return FALSE;	// if not the same service identifier

	return TRUE;
}


static CFHashCode
__SCNetworkServiceHash(CFTypeRef cf)
{
	SCNetworkServicePrivateRef	servicePrivate	= (SCNetworkServicePrivateRef)cf;

	return CFHash(servicePrivate->serviceID);
}


static void
__SCNetworkServiceInitialize(void)
{
	__kSCNetworkServiceTypeID = _CFRuntimeRegisterClass(&__SCNetworkServiceClass);
	return;
}


__private_extern__ SCNetworkServicePrivateRef
__SCNetworkServiceCreatePrivate(CFAllocatorRef		allocator,
				SCPreferencesRef	prefs,
				CFStringRef		serviceID,
				SCNetworkInterfaceRef   interface)
{
	SCNetworkServicePrivateRef		servicePrivate;
	uint32_t				size;

	/* initialize runtime */
	pthread_once(&initialized, __SCNetworkServiceInitialize);

	/* allocate target */
	size           = sizeof(SCNetworkServicePrivate) - sizeof(CFRuntimeBase);
	servicePrivate = (SCNetworkServicePrivateRef)_CFRuntimeCreateInstance(allocator,
									      __kSCNetworkServiceTypeID,
									      size,
									      NULL);
	if (servicePrivate == NULL) {
		return NULL;
	}

	servicePrivate->prefs		= CFRetain(prefs);
	servicePrivate->serviceID	= CFStringCreateCopy(NULL, serviceID);
	servicePrivate->interface       = (interface != NULL) ? CFRetain(interface) : NULL;

	return servicePrivate;
}


#pragma mark -
#pragma mark SCNetworkService APIs


#define	N_QUICK	64


static CFDictionaryRef
_protocolTemplate(SCNetworkServiceRef service, CFStringRef protocolType)
{
	CFDictionaryRef			newEntity       = NULL;
	SCNetworkServicePrivateRef      servicePrivate  = (SCNetworkServicePrivateRef)service;

	if (servicePrivate->interface != NULL) {
		SCNetworkInterfaceRef   childInterface;
		CFStringRef		childInterfaceType      = NULL;
		CFStringRef		interfaceType;

		interfaceType = SCNetworkInterfaceGetInterfaceType(servicePrivate->interface);
		childInterface = SCNetworkInterfaceGetInterface(servicePrivate->interface);
		if (childInterface != NULL) {
			childInterfaceType = SCNetworkInterfaceGetInterfaceType(childInterface);
		}

		newEntity = __copyProtocolTemplate(interfaceType, childInterfaceType, protocolType);
	}

	if (newEntity == NULL) {
		newEntity = CFDictionaryCreate(NULL,
					       NULL,
					       NULL,
					       0,
					       &kCFTypeDictionaryKeyCallBacks,
					       &kCFTypeDictionaryValueCallBacks);
	}

	return newEntity;
}


Boolean
SCNetworkServiceAddProtocolType(SCNetworkServiceRef service, CFStringRef protocolType)
{
	CFDictionaryRef			entity;
	CFDictionaryRef			newEntity       = NULL;
	Boolean				ok		= FALSE;
	CFStringRef			path;
	SCNetworkProtocolRef		protocol;
	SCNetworkServicePrivateRef      servicePrivate  = (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (!__SCNetworkProtocolIsValidType(protocolType)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      protocolType);			// entity

	entity = SCPreferencesPathGetValue(servicePrivate->prefs, path);
	if (entity != NULL) {
		// if "protocol" already exists
		_SCErrorSet(kSCStatusKeyExists);
		goto done;
	}

	newEntity = CFDictionaryCreate(NULL,
				       NULL,
				       NULL,
				       0,
				       &kCFTypeDictionaryKeyCallBacks,
				       &kCFTypeDictionaryValueCallBacks);
	ok = SCPreferencesPathSetValue(servicePrivate->prefs, path, newEntity);
	CFRelease(newEntity);
	if (!ok) {
		goto done;
	}

	protocol  = SCNetworkServiceCopyProtocol(service, protocolType);
	newEntity = _protocolTemplate(service, protocolType);
	ok = SCNetworkProtocolSetConfiguration(protocol, newEntity);
	CFRelease(newEntity);
	CFRelease(protocol);

    done :

	CFRelease(path);
	return ok;
}


CFArrayRef /* of SCNetworkServiceRef's */
SCNetworkServiceCopyAll(SCPreferencesRef prefs)
{
	CFMutableArrayRef	array;
	CFIndex			n;
	CFStringRef		path;
	CFDictionaryRef		services;

	path = SCPreferencesPathKeyCreateNetworkServices(NULL);
	services = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);

	if ((services != NULL) && !isA_CFDictionary(services)) {
		return NULL;
	}

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	n = (services != NULL) ? CFDictionaryGetCount(services) : 0;
	if (n > 0) {
		CFIndex		i;
		const void *    keys_q[N_QUICK];
		const void **   keys	= keys_q;
		const void *    vals_q[N_QUICK];
		const void **   vals	= vals_q;

		if (n > (CFIndex)(sizeof(keys_q) / sizeof(CFTypeRef))) {
			keys = CFAllocatorAllocate(NULL, n * sizeof(CFTypeRef), 0);
			vals = CFAllocatorAllocate(NULL, n * sizeof(CFPropertyListRef), 0);
		}
		CFDictionaryGetKeysAndValues(services, keys, vals);
		for (i = 0; i < n; i++) {
			CFDictionaryRef			entity;
			SCNetworkServicePrivateRef	servicePrivate;

			if (!isA_CFDictionary(vals[i])) {
				SCLog(TRUE,
				      LOG_INFO,
				      CFSTR("SCNetworkServiceCopyAll(): error w/service \"%@\"\n"),
				      keys[i]);
				continue;
			}

			entity = CFDictionaryGetValue(vals[i], kSCEntNetInterface);
			if (!isA_CFDictionary(entity)) {
				// if no "interface"
				SCLog(TRUE,
				      LOG_INFO,
				      CFSTR("SCNetworkServiceCopyAll(): no \"%@\" entity for service \"%@\"\n"),
				      kSCEntNetInterface,
				      keys[i]);
				continue;
			}

			servicePrivate = __SCNetworkServiceCreatePrivate(NULL, prefs, keys[i], NULL);
			CFArrayAppendValue(array, (SCNetworkServiceRef)servicePrivate);
			CFRelease(servicePrivate);
		}
		if (keys != keys_q) {
			CFAllocatorDeallocate(NULL, keys);
			CFAllocatorDeallocate(NULL, vals);
		}
	}

	return array;
}


/*
 * build a list of all of a servives entity types that are associated
 * with the services interface.  The list will include :
 *
 * - entity types associated with the interface type (Ethernet, FireWire, PPP, ...)
 * - entity types associated with the interface sub-type (PPPSerial, PPPoE, L2TP, PPTP, ...)
 * - entity types associated with the hardware device (Ethernet, AirPort, FireWire, Modem, ...)
 */
static CFSetRef
_copyInterfaceEntityTypes(CFDictionaryRef protocols)
{
	CFDictionaryRef interface;
	CFMutableSetRef interface_entity_types;

	interface_entity_types = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);

	interface = CFDictionaryGetValue(protocols, kSCEntNetInterface);
	if (isA_CFDictionary(interface)) {
		CFStringRef	entities[]	= { kSCPropNetInterfaceType,
						    kSCPropNetInterfaceSubType,
						    kSCPropNetInterfaceHardware };
		int		i;

		// include the "Interface" entity itself
		CFSetAddValue(interface_entity_types, kSCEntNetInterface);

		// include the entities associated with the interface
		for (i = 0; i < sizeof(entities)/sizeof(entities[0]); i++) {
			CFStringRef     entity;

			entity = CFDictionaryGetValue(interface, entities[i]);
			if (isA_CFString(entity)) {
				CFSetAddValue(interface_entity_types, entity);
			}
		}

		/*
		 * and, because we've found some misguided network preference code
		 * developers leaving [PPP] entity dictionaries around even though
		 * they are unused and/or unneeded...
		 */
		CFSetAddValue(interface_entity_types, kSCEntNetPPP);
	}

	return interface_entity_types;
}


SCNetworkServiceRef
SCNetworkServiceCopy(SCPreferencesRef prefs, CFStringRef serviceID)
{
	CFDictionaryRef			entity;
	CFStringRef			path;
	SCNetworkServicePrivateRef      servicePrivate;

	if (!isA_CFString(serviceID)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,			// allocator
							      serviceID,		// service
							      kSCEntNetInterface);      // entity
	entity = SCPreferencesPathGetValue(prefs, path);
	CFRelease(path);

	if (!isA_CFDictionary(entity)) {
		// a "service" must have an "interface"
		_SCErrorSet(kSCStatusNoKey);
		return NULL;
	}

	servicePrivate = __SCNetworkServiceCreatePrivate(NULL, prefs, serviceID, NULL);
	return (SCNetworkServiceRef)servicePrivate;
}


SCNetworkProtocolRef
SCNetworkServiceCopyProtocol(SCNetworkServiceRef service, CFStringRef protocolType)
{
	CFSetRef			non_protocol_entities;
	CFStringRef			path;
	CFDictionaryRef			protocols;
	SCNetworkProtocolPrivateRef	protocolPrivate = NULL;
	SCNetworkServicePrivateRef	servicePrivate  = (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	if (!isA_CFString(protocolType)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      NULL);				// entity
	protocols = SCPreferencesPathGetValue(servicePrivate->prefs, path);
	CFRelease(path);

	if ((protocols != NULL) && !isA_CFDictionary(protocols)) {
		// if corrupt prefs
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	non_protocol_entities = _copyInterfaceEntityTypes(protocols);
	if (CFSetContainsValue(non_protocol_entities, protocolType)) {
		// if the "protocolType" matches an interface entity type
		_SCErrorSet(kSCStatusInvalidArgument);
		goto done;
	}

	if (!CFDictionaryContainsKey(protocols, protocolType)) {
		// if the "protocolType" entity does not exist
		_SCErrorSet(kSCStatusNoKey);
		goto done;
	}

	protocolPrivate = __SCNetworkProtocolCreatePrivate(NULL, protocolType, service);

    done :

	CFRelease(non_protocol_entities);

	return (SCNetworkProtocolRef)protocolPrivate;
}


CFArrayRef /* of SCNetworkProtocolRef's */
SCNetworkServiceCopyProtocols(SCNetworkServiceRef service)
{
	CFMutableArrayRef		array;
	CFIndex				n;
	CFSetRef			non_protocol_entities;
	CFStringRef			path;
	CFDictionaryRef			protocols;
	SCNetworkServicePrivateRef	servicePrivate  = (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      NULL);				// entity
	protocols = SCPreferencesPathGetValue(servicePrivate->prefs, path);
	CFRelease(path);

	if (!isA_CFDictionary(protocols)) {
		return NULL;
	}

	non_protocol_entities = _copyInterfaceEntityTypes(protocols);

	array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

	n = CFDictionaryGetCount(protocols);
	if (n > 0) {
		CFIndex				i;
		const void *			keys_q[N_QUICK];
		const void **			keys		= keys_q;
		const void *			vals_q[N_QUICK];
		const void **			vals		= vals_q;

		if (n > (CFIndex)(sizeof(keys_q) / sizeof(CFTypeRef))) {
			keys = CFAllocatorAllocate(NULL, n * sizeof(CFTypeRef), 0);
			vals = CFAllocatorAllocate(NULL, n * sizeof(CFPropertyListRef), 0);
		}
		CFDictionaryGetKeysAndValues(protocols, keys, vals);
		for (i = 0; i < n; i++) {
			SCNetworkProtocolPrivateRef	protocolPrivate;

			if (!isA_CFDictionary(vals[i])) {
				// if it's not a dictionary then it can't be a protocol entity
				continue;
			}

			if (CFSetContainsValue(non_protocol_entities, keys[i])) {
				// skip any non-protocol (interface) entities
				continue;
			}

			protocolPrivate = __SCNetworkProtocolCreatePrivate(NULL, keys[i], service);
			CFArrayAppendValue(array, (SCNetworkProtocolRef)protocolPrivate);

			CFRelease(protocolPrivate);
		}
		if (keys != keys_q) {
			CFAllocatorDeallocate(NULL, keys);
			CFAllocatorDeallocate(NULL, vals);
		}
	}

	CFRelease(non_protocol_entities);

	return array;
}


static Boolean
__SCNetworkServiceSetInterfaceEntity(SCNetworkServiceRef     service,
				     SCNetworkInterfaceRef   interface)
{
	CFDictionaryRef			entity;
	Boolean				ok;
	CFStringRef			path;
	SCNetworkServicePrivateRef      servicePrivate		= (SCNetworkServicePrivateRef)service;

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      kSCEntNetInterface);		// entity
	entity = __SCNetworkInterfaceCopyInterfaceEntity(interface);
	ok = SCPreferencesPathSetValue(servicePrivate->prefs, path, entity);
	CFRelease(entity);
	CFRelease(path);

	return ok;
}


static void
mergeDict(const void *key, const void *value, void *context)
{
	CFMutableDictionaryRef	newDict	= (CFMutableDictionaryRef)context;

	CFDictionarySetValue(newDict, key, value);
	return;
}


SCNetworkServiceRef
SCNetworkServiceCreate(SCPreferencesRef prefs, SCNetworkInterfaceRef interface)
{
	CFArrayRef			components;
	CFArrayRef			interface_config;
	CFStringRef			interface_name;
	SCNetworkInterfaceRef		newInterface;
	CFStringRef			path;
	CFStringRef			prefix;
	CFStringRef			serviceID;
	SCNetworkServicePrivateRef	servicePrivate;
	CFArrayRef			supported_protocols;

	if (!isA_SCNetworkInterface(interface)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	// only allow network interfaces which support one or more protocols
	// to be added to a service.  The one exception is that we allow
	// third-party interface types to be configured.
	supported_protocols = SCNetworkInterfaceGetSupportedProtocolTypes(interface);
	if (supported_protocols == NULL) {
		CFStringRef	interface_type;

		interface_type = SCNetworkInterfaceGetInterfaceType(interface);
		if (CFStringFind(interface_type, CFSTR("."), 0).location == kCFNotFound) {
			return NULL;
		}
	}

	// establish the service
	prefix = SCPreferencesPathKeyCreateNetworkServices(NULL);
	path = __SCPreferencesPathCreateUniqueChild_WithMoreSCFCompatibility(prefs, prefix);
	if (path == NULL) path = SCPreferencesPathCreateUniqueChild(prefs, prefix);
	CFRelease(prefix);
	if (path == NULL) {
		return NULL;
	}

	components = CFStringCreateArrayBySeparatingStrings(NULL, path, CFSTR("/"));
	CFRelease(path);

	serviceID = CFArrayGetValueAtIndex(components, 2);
	servicePrivate = __SCNetworkServiceCreatePrivate(NULL, prefs, serviceID, NULL);
	CFRelease(components);

	// duplicate the interface and associate the copy with the new service
	newInterface = (SCNetworkInterfaceRef)__SCNetworkInterfaceCreateCopy(NULL,
									     interface,
									     prefs,
									     serviceID);
	servicePrivate->interface = newInterface;

	// establish "default" configuration(s) for the interface
	for (interface = newInterface;
	     interface != NULL;
	     interface = SCNetworkInterfaceGetInterface(interface)) {
		SCNetworkInterfaceRef   childInterface;
		CFStringRef		childInterfaceType      = NULL;
		CFDictionaryRef		config;
		CFStringRef		interfaceType;

		interfaceType = SCNetworkInterfaceGetInterfaceType(interface);
		childInterface = SCNetworkInterfaceGetInterface(interface);
		if (childInterface != NULL) {
			childInterfaceType = SCNetworkInterfaceGetInterfaceType(childInterface);
		}

		config = __copyInterfaceTemplate(interfaceType, childInterfaceType);
		if (config != NULL) {
			if (CFEqual(interfaceType, kSCNetworkInterfaceTypeBluetooth) ||
			    CFEqual(interfaceType, kSCNetworkInterfaceTypeIrDA     ) ||
			    CFEqual(interfaceType, kSCNetworkInterfaceTypeModem    ) ||
			    CFEqual(interfaceType, kSCNetworkInterfaceTypeSerial   ) ||
			    CFEqual(interfaceType, kSCNetworkInterfaceTypeWWAN     )) {
				CFDictionaryRef	overrides;
				CFStringRef	script;

				overrides = __SCNetworkInterfaceGetTemplateOverrides(interface, kSCNetworkInterfaceTypeModem);

				// a ConnectionScript (and related keys) from the interface
				// should trump the settings from the configuration template.
				if ((overrides != NULL) &&
				    CFDictionaryContainsKey(overrides, kSCPropNetModemConnectionScript)) {
					CFMutableDictionaryRef	newConfig;

					newConfig = CFDictionaryCreateMutableCopy(NULL, 0, config);
					CFDictionaryRemoveValue(newConfig, kSCPropNetModemConnectionPersonality);
					CFDictionaryRemoveValue(newConfig, kSCPropNetModemConnectionScript);
					CFDictionaryRemoveValue(newConfig, kSCPropNetModemDeviceVendor);
					CFDictionaryRemoveValue(newConfig, kSCPropNetModemDeviceModel);
					CFRelease(config);
					config = newConfig;
				}

				// update template for v.92 modems
				if ((overrides == NULL) &&
				    CFDictionaryGetValueIfPresent(config,
					   			  kSCPropNetModemConnectionScript,
								  (const void **)&script) &&
				    CFEqual(script, CFSTR("v.34 Personality")) &&
				    _SCNetworkInterfaceIsModemV92(interface)) {
					CFMutableDictionaryRef	newConfig;

					newConfig = CFDictionaryCreateMutableCopy(NULL, 0, config);
					CFDictionarySetValue(newConfig,
							     kSCPropNetModemConnectionPersonality,
							     CFSTR("v.92 Personality"));
					CFDictionarySetValue(newConfig,
							     kSCPropNetModemDeviceModel,
							     CFSTR("Apple Modem (v.92)"));
					CFRelease(config);
					config = newConfig;
				}

				if (overrides != NULL) {
					CFMutableDictionaryRef	newConfig;

					newConfig = CFDictionaryCreateMutableCopy(NULL, 0, config);
					CFDictionaryApplyFunction(overrides, mergeDict, newConfig);
					CFRelease(config);
					config = newConfig;
				}
			} else if (CFEqual(interfaceType, kSCNetworkInterfaceTypePPP)) {
				CFDictionaryRef		overrides;

				overrides = __SCNetworkInterfaceGetTemplateOverrides(interface, kSCNetworkInterfaceTypePPP);
				if (overrides != NULL) {
					CFMutableDictionaryRef	newConfig;

					newConfig = CFDictionaryCreateMutableCopy(NULL, 0, config);
					CFDictionaryApplyFunction(overrides, mergeDict, newConfig);
					CFRelease(config);
					config = newConfig;
				}
			}

			if (!__SCNetworkInterfaceSetConfiguration(interface, NULL, config, TRUE)) {
				SCLog(TRUE, LOG_DEBUG,
				      CFSTR("SCNetworkService __SCNetworkInterfaceSetConfiguration failed(), interface=%@, type=NULL"),
				      interface);
			}
			CFRelease(config);
		}
	}

	// add the interface [entity] to the service
	(void) __SCNetworkServiceSetInterfaceEntity((SCNetworkServiceRef)servicePrivate,
						    servicePrivate->interface);

	// push the [deep] interface configuration into the service.
	interface_config = __SCNetworkInterfaceCopyDeepConfiguration(servicePrivate->interface);
	__SCNetworkInterfaceSetDeepConfiguration(servicePrivate->interface, interface_config);
	if (interface_config != NULL) CFRelease(interface_config);

	// set the service name to match that of the associated interface
	//
	// Note: It might seem a bit odd to call SCNetworkServiceGetName
	// followed by an immediate call to SCNetworkServiceSetName.  The
	// trick here is that if no name has previously been set, the
	// "get" function will return the name of the associated interface.
	//
	// ... and we "set" a name to ensure that applications that do
	// not use the APIs will still find a UserDefinedName property
	// in the SCDynamicStore.
	//
	interface_name = SCNetworkServiceGetName((SCNetworkServiceRef)servicePrivate);
	if (interface_name != NULL) {
		(void) SCNetworkServiceSetName((SCNetworkServiceRef)servicePrivate,
					       interface_name);
	}

	return (SCNetworkServiceRef)servicePrivate;
}


Boolean
SCNetworkServiceEstablishDefaultConfiguration(SCNetworkServiceRef service)
{
	CFIndex			i;
	SCNetworkInterfaceRef	interface;
	CFIndex			n;
	CFArrayRef		protocolTypes;

	interface = SCNetworkServiceGetInterface(service);
	if (interface == NULL) {
		return FALSE;
	}

	protocolTypes = SCNetworkInterfaceGetSupportedProtocolTypes(interface);
	n = (protocolTypes != NULL) ? CFArrayGetCount(protocolTypes) : 0;
	for (i = 0; i < n; i++) {
		Boolean			enabled;
		CFDictionaryRef		newEntity	= NULL;
		Boolean			ok;
		SCNetworkProtocolRef	protocol	= NULL;
		CFStringRef		protocolType;

		protocolType = CFArrayGetValueAtIndex(protocolTypes, i);
		ok = SCNetworkServiceAddProtocolType(service, protocolType);
		if (!ok && (SCError() != kSCStatusKeyExists)) {
			// could not add protocol
			goto nextProtocol;
		}

		protocol = SCNetworkServiceCopyProtocol(service, protocolType);
		if (protocol == NULL) {
			// oops, somethings wrong (should never happen)
			goto nextProtocol;
		}

		newEntity = _protocolTemplate(service, protocolType);
		ok = SCNetworkProtocolSetConfiguration(protocol, newEntity);
		if (!ok) {
			// could not set default configuration
			goto nextProtocol;
		}

		enabled = !CFDictionaryContainsKey(newEntity, kSCResvInactive);
		ok = SCNetworkProtocolSetEnabled(protocol, enabled);
		if (!ok) {
			// could not enable/disable protocol
			goto nextProtocol;
		}

	    nextProtocol :

		if (newEntity != NULL) CFRelease(newEntity);
		if (protocol  != NULL) CFRelease(protocol);
	}

	return TRUE;
}


Boolean
SCNetworkServiceGetEnabled(SCNetworkServiceRef service)
{
	Boolean				enabled;
	CFStringRef			path;
	SCNetworkServicePrivateRef      servicePrivate  = (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      NULL);				// entity
	enabled = __getPrefsEnabled(servicePrivate->prefs, path);
	CFRelease(path);

	return enabled;
}


SCNetworkInterfaceRef
SCNetworkServiceGetInterface(SCNetworkServiceRef service)
{
	SCNetworkServicePrivateRef      servicePrivate  = (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	if (servicePrivate->interface == NULL) {
		CFDictionaryRef entity;
		CFStringRef     path;

		path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
								      servicePrivate->serviceID,	// service
								      kSCEntNetInterface);		// entity
		entity = SCPreferencesPathGetValue(servicePrivate->prefs, path);
		CFRelease(path);

		if (isA_CFDictionary(entity)) {
			servicePrivate->interface = _SCNetworkInterfaceCreateWithEntity(NULL, entity, service);
		}
	}

	return servicePrivate->interface;
}


CFStringRef
SCNetworkServiceGetName(SCNetworkServiceRef service)
{
	CFDictionaryRef			entity;
	SCNetworkInterfaceRef		interface;
	SCNetworkServicePrivateRef	servicePrivate	= (SCNetworkServicePrivateRef)service;
	CFStringRef			name		= NULL;
	CFStringRef			path;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      NULL);				// entity
	entity = SCPreferencesPathGetValue(servicePrivate->prefs, path);
	CFRelease(path);

	if (isA_CFDictionary(entity)) {
		name = CFDictionaryGetValue(entity, kSCPropUserDefinedName);
		name = isA_CFString(name);
	}

	interface = SCNetworkServiceGetInterface(service);
	while (interface != NULL) {
		SCNetworkInterfaceRef   childInterface;

		childInterface = SCNetworkInterfaceGetInterface(interface);
		if (childInterface == NULL) {
			break;
		}

		interface = childInterface;
	}

	if (interface != NULL) {
		if (name != NULL) {
			CFStringRef	interface_name;

			interface_name = __SCNetworkInterfaceGetNonLocalizedDisplayName(interface);
			if ((interface_name != NULL) && CFEqual(name, interface_name)) {
				// if service name matches the [non-]localized
				// interface name
				name = NULL;
			}
		}

		if (name == NULL) {
			name = SCNetworkInterfaceGetLocalizedDisplayName(interface);
		}
	}

	return name;
}


CFStringRef
SCNetworkServiceGetServiceID(SCNetworkServiceRef service)
{
	SCNetworkServicePrivateRef	servicePrivate	= (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	return servicePrivate->serviceID;
}


CFTypeID
SCNetworkServiceGetTypeID(void)
{
	pthread_once(&initialized, __SCNetworkServiceInitialize);	/* initialize runtime */
	return __kSCNetworkServiceTypeID;
}


Boolean
SCNetworkServiceRemove(SCNetworkServiceRef service)
{
	Boolean				ok		= FALSE;
	SCNetworkServicePrivateRef	servicePrivate	= (SCNetworkServicePrivateRef)service;
	CFArrayRef			sets;
	CFStringRef			path;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	// remove service from all sets

	sets = SCNetworkSetCopyAll(servicePrivate->prefs);
	if (sets != NULL) {
		CFIndex	i;
		CFIndex n;

		n = CFArrayGetCount(sets);
		for (i = 0; i < n; i++) {
			SCNetworkSetRef	set;

			set = CFArrayGetValueAtIndex(sets, i);
			ok = SCNetworkSetRemoveService(set, service);
			if (!ok && (SCError() != kSCStatusNoKey)) {
				CFRelease(sets);
				return ok;
			}
		}
		CFRelease(sets);
	}

	// remove service

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      NULL);				// entity
	ok = SCPreferencesPathRemoveValue(servicePrivate->prefs, path);
	CFRelease(path);

	return ok;
}


Boolean
SCNetworkServiceRemoveProtocolType(SCNetworkServiceRef service, CFStringRef protocolType)
{
	CFDictionaryRef			entity;
	Boolean				ok		= FALSE;
	CFStringRef			path;
	SCNetworkServicePrivateRef      servicePrivate  = (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (!__SCNetworkProtocolIsValidType(protocolType)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      protocolType);			// entity

	entity = SCPreferencesPathGetValue(servicePrivate->prefs, path);
	if (entity == NULL) {
		// if "protocol" does not exist
		_SCErrorSet(kSCStatusNoKey);
		goto done;
	}

	ok = SCPreferencesPathRemoveValue(servicePrivate->prefs, path);

    done :

	CFRelease(path);
	return ok;
}


Boolean
SCNetworkServiceSetEnabled(SCNetworkServiceRef service, Boolean enabled)
{
	Boolean				ok;
	CFStringRef			path;
	SCNetworkServicePrivateRef      servicePrivate  = (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      NULL);				// entity
	ok = __setPrefsEnabled(servicePrivate->prefs, path, enabled);
	CFRelease(path);

	return ok;
}


Boolean
SCNetworkServiceSetName(SCNetworkServiceRef service, CFStringRef name)
{
	CFDictionaryRef			entity;
	Boolean				ok		= FALSE;
	CFStringRef			path;
	SCNetworkServicePrivateRef	servicePrivate	= (SCNetworkServicePrivateRef)service;

	if (!isA_SCNetworkService(service)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if ((name != NULL) && !isA_CFString(name)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	if (name != NULL) {
		SCNetworkInterfaceRef	interface;

		interface = SCNetworkServiceGetInterface(service);
		while (interface != NULL) {
			SCNetworkInterfaceRef	childInterface;

			childInterface = SCNetworkInterfaceGetInterface(interface);
			if (childInterface == NULL) {
				break;
			}

			interface = childInterface;
		}

		if (interface != NULL) {
			CFStringRef	interface_name;

			interface_name = SCNetworkInterfaceGetLocalizedDisplayName(interface);
			if ((interface_name != NULL) && CFEqual(name, interface_name)) {
				// if service name matches the localized interface name
				// then store the non-localized name.
				interface_name = __SCNetworkInterfaceGetNonLocalizedDisplayName(interface);
				if (interface_name != NULL) {
					name = interface_name;
				}
			}
		}
	}

#define PREVENT_DUPLICATE_SERVICE_NAMES
#ifdef  PREVENT_DUPLICATE_SERVICE_NAMES
	if (name != NULL) {
		CFArrayRef      sets;

		// ensure that each service is uniquely named within its sets

		sets = SCNetworkSetCopyAll(servicePrivate->prefs);
		if (sets != NULL) {
			CFIndex		set_index;
			CFIndex		set_count;

			set_count = CFArrayGetCount(sets);
			for (set_index = 0; set_index < set_count; set_index++) {
				CFIndex		service_index;
				Boolean		isDup		= FALSE;
				Boolean		isMember	= FALSE;
				CFIndex		service_count;
				CFArrayRef      services;
				SCNetworkSetRef set		= CFArrayGetValueAtIndex(sets, set_index);

				services = SCNetworkSetCopyServices(set);

				service_count = CFArrayGetCount(services);
				for (service_index = 0; service_index < service_count; service_index++) {
					CFStringRef		otherID;
					CFStringRef		otherName;
					SCNetworkServiceRef     otherService;

					otherService = CFArrayGetValueAtIndex(services, service_index);

					otherID = SCNetworkServiceGetServiceID(otherService);
					if (CFEqual(servicePrivate->serviceID, otherID)) {
						// if the service is a member of this set
						isMember = TRUE;
						continue;
					}

					otherName = SCNetworkServiceGetName(otherService);
					if ((otherName != NULL) && CFEqual(name, otherName)) {
						isDup = TRUE;
						continue;
					}
				}

				CFRelease(services);

				if (isMember && isDup) {
					/*
					 * if this service is a member of the set and
					 * the "name" is not unique.
					 */
					CFRelease(sets);
					_SCErrorSet(kSCStatusKeyExists);
					return FALSE;
				}
			}

			CFRelease(sets);
		}
	}
#endif  /* PREVENT_DUPLICATE_SERVICE_NAMES */

	path = SCPreferencesPathKeyCreateNetworkServiceEntity(NULL,				// allocator
							      servicePrivate->serviceID,	// service
							      NULL);				// entity
	entity = SCPreferencesPathGetValue(servicePrivate->prefs, path);
	if ((entity == NULL) && (name != NULL)) {
		entity = CFDictionaryCreate(NULL,
					    NULL,
					    NULL,
					    0,
					    &kCFTypeDictionaryKeyCallBacks,
					    &kCFTypeDictionaryValueCallBacks);
	}
	if (isA_CFDictionary(entity)) {
		CFMutableDictionaryRef	newEntity;

		newEntity = CFDictionaryCreateMutableCopy(NULL, 0, entity);
		if (name != NULL) {
			CFDictionarySetValue(newEntity, kSCPropUserDefinedName, name);
		} else {
			CFDictionaryRemoveValue(newEntity, kSCPropUserDefinedName);
		}
		ok = SCPreferencesPathSetValue(servicePrivate->prefs, path, newEntity);
		CFRelease(newEntity);
	}
	CFRelease(path);

	return ok;
}
