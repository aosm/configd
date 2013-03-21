/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */


#ifndef	_PPP_MSG_H
#define	_PPP_MSG_H

#include <sys/types.h>

/* local socket path */
#define PPP_PATH 	"/var/run/pppconfd\0"


/* PPP message paquets */
struct ppp_msg_hdr {
    u_int32_t 		m_type; 	// type of the message
    u_int32_t 		m_result; 	// error code of notification message
    u_int32_t 		m_cookie;	// user param
    u_int32_t 		m_link;		// link for this message
    u_int32_t 		m_len;		// len of the following data
};

struct ppp_msg {
    u_int32_t 		m_type; 	// type of the message
    u_int32_t 		m_result; 	// error code of notification message
    u_int32_t 		m_cookie;	// user param, or error num for event
    u_int32_t 		m_link;		// link for this message
    u_int32_t 		m_len;		// len of the following data
    u_char 		m_data[1];	// msg data sent or received
};



/* codes for ppp messages */
enum {
    /* API client commands */
    PPP_VERSION = 1,
    PPP_STATUS,
    PPP_CONNECT,
    PPP_DISCONNECT = 5,
    PPP_GETOPTION,
    PPP_SETOPTION,
    PPP_ENABLE_EVENT,
    PPP_DISABLE_EVENT,
    PPP_EVENT,
    PPP_GETNBLINKS,
    PPP_GETLINKBYINDEX

};

// struct for an option
struct ppp_opt_hdr {
    u_int32_t 		o_type;
};

struct ppp_opt {
    u_int32_t 		o_type;
    u_char 		o_data[1];
};


/* codes for options management */
enum {

    PPP_OPT_DEV_NAME = 1,		// string
    PPP_OPT_DEV_SPEED,			// 4 bytes
    PPP_OPT_DEV_CONNECTSCRIPT,		// string

    PPP_OPT_COMM_IDLETIMER,		// 4 bytes
    PPP_OPT_COMM_REMOTEADDR,		// string

    PPP_OPT_AUTH_PROTO,			// 4 bytes
    PPP_OPT_AUTH_NAME,			// string
    PPP_OPT_AUTH_PASSWD,		// string

    PPP_OPT_LCP_HDRCOMP,		// 4 bytes
    PPP_OPT_LCP_MRU,			// 4 bytes
    PPP_OPT_LCP_MTU,			// 4 bytes
    PPP_OPT_LCP_RCACCM,			// 4 bytes
    PPP_OPT_LCP_TXACCM,			// 4 bytes

    PPP_OPT_IPCP_HDRCOMP,		// 4 bytes
    PPP_OPT_IPCP_LOCALADDR,		// 4 bytes
    PPP_OPT_IPCP_REMOTEADDR,		// 4 bytes

    PPP_OPT_LOGFILE,			// string
    PPP_OPT_RESERVED,			// 4 bytes
    PPP_OPT_REMINDERTIMER,		// 4 bytes (not implemented)
    PPP_OPT_ALERTENABLE,		// 4 bytes (not implemented)

    PPP_OPT_LCP_ECHO,			// struct ppp_opt_echo

    PPP_OPT_COMM_CONNECTDELAY,		// 4 bytes
    PPP_OPT_COMM_SESSIONTIMER,		// 4 bytes
    PPP_OPT_COMM_TERMINALMODE,		// 4 bytes
    PPP_OPT_COMM_TERMINALSCRIPT,	// string. Additionnal connection script, once modem is connected
    PPP_OPT_DEV_CAPS,			// struct ppp_caps...

    PPP_OPT_IPCP_USESERVERDNS,		// 4 bytes
    PPP_OPT_COMM_CONNECTSPEED,		// 4 bytes, actual connection speed
    PPP_OPT_SERVICEID			// string, name of the associated service in the store

};

// options values

// PPP_LCP_OPT_HDRCOMP -- option ppp addr/ctrl compression
enum {
    PPP_LCP_HDRCOMP_NONE = 0,
    PPP_LCP_HDRCOMP_ADDR = 1,
    PPP_LCP_HDRCOMP_PROTO = 2
};

enum {
    PPP_COMM_TERM_NONE = 0,
    PPP_COMM_TERM_SCRIPT,
    PPP_COMM_TERM_WINDOW
};

enum {
    PPP_IPCP_HDRCOMP_NONE = 0,
    PPP_IPCP_HDRCOMP_VJ
};

// PPP_LCP_OPT_RCACCM -- option receive control asynchronous character map
enum {
    PPP_LCP_ACCM_NONE = 0,
    PPP_LCP_ACCM_XONXOFF = 0x000A0000,
    PPP_LCP_ACCM_ALL = 0xFFFFFFFF
};

// PPP_OPT_AUTH
enum {
    PPP_AUTH_NONE = 0,
    PPP_AUTH_PAPCHAP,
    PPP_AUTH_PAP,
    PPP_AUTH_CHAP
};

// state machine
enum {
    PPP_IDLE = 0,
    PPP_INITIALIZE,
    PPP_CONNECTLINK,
    PPP_STATERESERVED,
    PPP_ESTABLISH,
    PPP_AUTHENTICATE,
    PPP_CALLBACK,
    PPP_NETWORK,
    PPP_RUNNING,
    PPP_TERMINATE,
    PPP_DISCONNECTLINK
};

// events
enum {
    PPP_EVT_DISCONNECTED = 1,
    PPP_EVT_CONNSCRIPT_STARTED,
    PPP_EVT_CONNSCRIPT_FINISHED,
    PPP_EVT_TERMSCRIPT_STARTED,
    PPP_EVT_TERMSCRIPT_FINISHED,
    PPP_EVT_LOWERLAYER_UP,
    PPP_EVT_LOWERLAYER_DOWN,
    PPP_EVT_LCP_UP,
    PPP_EVT_LCP_DOWN,
    PPP_EVT_IPCP_UP,
    PPP_EVT_IPCP_DOWN,
    PPP_EVT_AUTH_STARTED,
    PPP_EVT_AUTH_FAILED,
    PPP_EVT_AUTH_SUCCEDED
};

struct ppp_opt_echo {		// 0 for the following value will cancel echo option
    u_int16_t 	interval;	// delay in seconds between echo requests
    u_int16_t 	failure;	// # of failure before declaring the link down
};

struct ppp_status {
    // connection stats
    u_int32_t 		status;
    union {
	struct connected {
	    u_int32_t 		timeElapsed;
	    u_int32_t 		timeRemaining;
	    // bytes stats
	    u_int32_t 		inBytes;
	    u_int32_t 		inPackets;
	    u_int32_t 		inErrors;
	    u_int32_t 		outBytes;
	    u_int32_t 		outPackets;
	    u_int32_t 		outErrors;
	} run;
	struct disconnected {
	    u_int32_t 		lastDiscCause;
	} disc;
    } s;
};

enum {
    // from 0 to 255, we use bsd error codes from errno.h

    // ppp speficic error codes
    PPP_ERR_GEN_ERROR	= 256,
    PPP_ERR_CONNSCRIPTFAILED,
    PPP_ERR_TERMSCRIPTFAILED,
    PPP_ERR_LCPFAILED,
    PPP_ERR_AUTHFAILED,
    PPP_ERR_IDLETIMEOUT,
    PPP_ERR_SESSIONTIMEOUT,
    PPP_ERR_LOOPBACK,
    PPP_ERR_PEERDEAD,
    PPP_ERR_DISCSCRIPTFAILED,

    // modem specific error codes
    PPP_ERR_MOD_NOCARRIER	= 512,
    PPP_ERR_MOD_BUSY,
    PPP_ERR_MOD_NODIALTONE,
    PPP_ERR_MOD_ERROR
};

#endif /* _PPP_MSG_H */

