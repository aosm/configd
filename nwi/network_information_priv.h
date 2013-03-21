/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
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

#ifndef _NETWORK_INFORMATION_PRIV_H_
#define _NETWORK_INFORMATION_PRIV_H_

#include <net/if.h>
#include <stdint.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "network_information.h"

__private_extern__
sa_family_t nwi_af_list[2];

#define NWI_IFSTATE_FLAGS_NOT_IN_LIST	0x8

typedef uint32_t        Rank;

typedef struct _nwi_ifstate {
	char ifname[IFNAMSIZ];
	uint64_t flags;
	nwi_ifstate_t af_alias;
	Rank rank;
	int af;
	union {
		struct in_addr iaddr;
		struct in6_addr iaddr6;
	};
	const char* diff_ch;
} nwi_ifstate;

/*
 * nwi_state
 *
 *+---------------------------------------------+
 *| generation_count                            |
 *|                                             |
 *----------------------------------------------+
 *| size                                        |
 *|                                             |
 *|---------------------------------------------+
 *| ipv4_count                                  |
 *|                                             |
 *|---------------------------------------------+
 *| ipv6_count                                  |
 *|                                             |
 *|---------------------------------------------+
 *| ipv6_start                                  |-------+
 *|                                             |       |
 *|---------------------------------------------+       |ipv6_start stores the index of the start of the v6 list.
 *| ref                                         |       |
 *|                                             |       |
 *|---------------------------------------------+       |
 *| IPv4 nwi_ifstates                           |       |
 *|                                             |<------|-------+
 *|       ...                                   |       |       |
 *|---------------------------------------------+       |       |
 *| Sentinel nwi_ifstates                       |       |       |
 *| 	flags =NWI_IFSTATE_FLAGS_RANK_NEVER)    |       |       | af_alias points to the same ifstate in the
 *|                                             |       |       | opposite (v4 -> v6 and vice versa) af list.
 *|---------------------------------------------+       |       |
 *| IPv6 nwi_ifstates                           |<------+       |
 *|                                             |<--------------+
 *|       ...                                   |
 *|---------------------------------------------+
 *| Sentinel nwi_ifstates                       |
 *|       flags =NWI_IFSTATE_FLAGS_RANK_NEVER)  |
 *|                                             |
 *|---------------------------------------------+
 *
 */
typedef struct _nwi_state {
	uint64_t generation_count;
	uint32_t size;
	uint32_t ipv4_count;
	uint32_t ipv6_count;
	uint32_t ipv6_start;
	uint32_t ref;
	nwi_ifstate nwi_ifstates[0];
} nwi_state;

static __inline__ int
uint32_cmp(uint32_t a, uint32_t b)
{
	int		ret;
	
	if (a == b) {
		ret = 0;
	}
	else if (a < b) {
		ret = -1;
	}
	else {
		ret = 1;
	}
	return (ret);
}

static __inline__ int
RankCompare(Rank a, Rank b)
{
	return (uint32_cmp(a, b));
}

/*
 * Function: nwi_state_get_ifstate_count
 * Purpose:
 *   Return the number of ifstate elements for the specified address family
 *   'af'. 'af' is either AF_INET or AF_INET6.
 *
 *   Returns zero if there are no elements.
 */
static __inline__
int
nwi_state_get_ifstate_count(nwi_state_t state, int af)
{
	return (af == AF_INET)?state->ipv4_count:state->ipv6_count;
}

/*
 *   The ifstate list is sorted in order of decreasing priority, with the
 *   highest priority element appearing at index zero.
 *
 *   If 'idx' is outside of the bounds of the corresponding array, returns NULL.
 */
static __inline__
nwi_ifstate_t
nwi_state_get_ifstate_with_index(nwi_state_t state, int af, int idx)
{
	nwi_ifstate_t nwi_ifstate = NULL;
	int i_idx = idx;

	if (idx > nwi_state_get_ifstate_count(state, af)) {
		return (nwi_ifstate);
	}

	if (af == AF_INET6) {
		i_idx = idx + state->ipv6_start;
	}

	return &state->nwi_ifstates[i_idx];
}

/*
 * Function: nwi_state_get_ifstate_with_name
 * Purpose:
 *   Return the ifstate for the specified ifstate for the specified address
 *   family 'af'. 'af' is either AF_INET or AF_INET6.
 *
 *   Returns NULL if no such information exists.
 */
static __inline__
nwi_ifstate_t
nwi_state_get_ifstate_with_name(nwi_state_t state,
				 int af, const char * name)
{
	int idx = 0;
	int count;
	nwi_ifstate_t ifstate = NULL;

	if (state == NULL) {
		return ifstate;
	}

	count = (af == AF_INET)
	?state->ipv4_count:state->ipv6_count;


	while (idx < count) {
		ifstate = nwi_state_get_ifstate_with_index(state, af, idx);
		if (ifstate == NULL) {
			break;
		}
		if (strcmp(name,
			   nwi_ifstate_get_ifname(ifstate)) == 0) {
			return (ifstate);
		}
		idx++;
	}
	return (NULL);
}

__private_extern__
nwi_state_t
nwi_state_new(nwi_state_t old_state, int elems);

__private_extern__
nwi_state_t
nwi_state_copy_priv(nwi_state_t old_state);

__private_extern__
void
nwi_insert_ifstate(nwi_state_t state, const char* ifname, int af,
		   uint64_t flags, Rank rank,
		   void * ifa);

__private_extern__
void
nwi_state_clear(nwi_state_t state, int af);

__private_extern__
void
nwi_state_set_last(nwi_state_t state, int af);

__private_extern__
nwi_state_t
nwi_state_diff(nwi_state_t old_state, nwi_state_t new_state);

__private_extern__
void *
nwi_ifstate_get_address(nwi_ifstate_t ifstate);

__private_extern__
const char *
nwi_ifstate_get_diff_str(nwi_ifstate_t ifstate);

__private_extern__
_Bool
_nwi_state_store(nwi_state_t state);

__private_extern__
nwi_state_t
_nwi_state_copy(void);

__private_extern__
void
_nwi_state_dump(int level, nwi_state_t state);

#endif
