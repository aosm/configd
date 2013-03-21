/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
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
 * August 4, 2004		Allan Nathanson <ajn@apple.com>
 * - added network configuration (prefs) support
 *
 * September 25, 2002		Allan Nathanson <ajn@apple.com>
 * - added command line history & editing
 *
 * July 9, 2001			Allan Nathanson <ajn@apple.com>
 * - added "-r" option for checking network reachability
 * - added "-w" option to check/wait for the presence of a
 *   dynamic store key.
 *
 * June 1, 2001			Allan Nathanson <ajn@apple.com>
 * - public API conversion
 *
 * November 9, 2000		Allan Nathanson <ajn@apple.com>
 * - initial revision
 */

#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sysexits.h>

#ifdef	DEBUG
#include <mach/mach.h>
#include <mach/mach_error.h>
#endif	/* DEBUG */

#include "scutil.h"
#include "commands.h"
#include "dictionary.h"
#include "net.h"
#include "prefs.h"
#include "session.h"
#include "tests.h"

#include "SCDynamicStoreInternal.h"


#define LINE_LENGTH 256


__private_extern__ InputRef		currentInput	= NULL;
__private_extern__ int			nesting		= 0;
__private_extern__ CFRunLoopRef		notifyRl	= NULL;
__private_extern__ CFRunLoopSourceRef	notifyRls	= NULL;
__private_extern__ SCPreferencesRef	prefs		= NULL;
__private_extern__ SCDynamicStoreRef	store		= NULL;
__private_extern__ CFPropertyListRef	value		= NULL;
__private_extern__ CFMutableArrayRef	watchedKeys	= NULL;
__private_extern__ CFMutableArrayRef	watchedPatterns	= NULL;

static const struct option longopts[] = {
//	{ "debug",		no_argument,		NULL,	'd'	},
//	{ "verbose",		no_argument,		NULL,	'v'	},
//	{ "SPI",		no_argument,		NULL,	'p'	},
//	{ "check-reachability",	required_argument,	NULL,	'r'	},
//	{ "timeout",		required_argument,	NULL,	't'	},
//	{ "wait-key",		required_argument,	NULL,	'w'	},
	{ "dns",		no_argument,		NULL,	0	},
	{ "get",		required_argument,	NULL,	0	},
	{ "help",		no_argument,		NULL,	'?'	},
	{ "net",		no_argument,		NULL,	0	},
	{ "proxy",		no_argument,		NULL,	0	},
	{ "set",		required_argument,	NULL,	0	},
	{ NULL,			0,			NULL,	0	}
};


static char *
getLine(char *buf, int len, InputRef src)
{
	int	n;

	if (src->el) {
		int		count;
		const char	*line;

		line = el_gets(src->el, &count);
		if (line == NULL)
			return NULL;

		strncpy(buf, line, len);
	} else {
		if (fgets(buf, len, src->fp) == NULL)
			return NULL;
	}

	n = strlen(buf);
	if (buf[n-1] == '\n') {
		/* the entire line fit in the buffer, remove the newline */
		buf[n-1] = '\0';
	} else if (!src->el) {
		/* eat the remainder of the line */
		do {
			n = fgetc(src->fp);
		} while ((n != '\n') && (n != EOF));
	}

	if (src->h) {
		HistEvent	ev;

		history(src->h, &ev, H_ENTER, buf);
	}

	return buf;
}


static char *
getString(char **line)
{
	char *s, *e, c, *string;
	int i, isQuoted = 0, escaped = 0;

	if (*line == NULL) return NULL;
	if (**line == '\0') return NULL;

	/* Skip leading white space */
	while (isspace(**line)) *line += 1;

	/* Grab the next string */
	s = *line;
	if (*s == '\0') {
		return NULL;				/* no string available */
	} else if (*s == '"') {
		isQuoted = 1;				/* it's a quoted string */
		s++;
	}

	for (e = s; (c = *e) != '\0'; e++) {
		if (isQuoted && (c == '"'))
			break;				/* end of quoted string */
		if (c == '\\') {
			e++;
			if (*e == '\0')
				break;			/* if premature end-of-string */
			if ((*e == '"') || isspace(*e))
				escaped++;		/* if escaped quote or white space */
		}
		if (!isQuoted && isspace(c))
			break;				/* end of non-quoted string */
	}

	string = malloc(e - s - escaped + 1);

	for (i = 0; s < e; s++) {
		string[i] = *s;
		if (!((s[0] == '\\') && ((s[1] == '"') || isspace(s[1])))) i++;
	}
	string[i] = '\0';

	if (isQuoted)
		e++;					/* move past end of quoted string */

	*line = e;
	return string;
}


__private_extern__
Boolean
process_line(InputRef src)
{
	char	*arg;
	int	argc			= 0;
	char	**argv			= NULL;
	int	i;
	char	line[LINE_LENGTH];
	char	*s			= line;

	// if end-of-file, exit
	if (getLine(line, sizeof(line), src) == NULL)
		return FALSE;

	if (nesting > 0) {
		SCPrint(TRUE, stdout, CFSTR("%d> %s\n"), nesting, line);
	}

	// break up the input line
	while ((arg = getString(&s)) != NULL) {
		if (argc == 0)
			argv = (char **)malloc(2 * sizeof(char *));
		else
			argv = (char **)reallocf(argv, ((argc + 2) * sizeof(char *)));
		argv[argc++] = arg;
	}

	if (argc == 0) {
		return TRUE;		// if no arguments
	}

	/* process the command */
	if (*argv[0] != '#') {
		argv[argc] = NULL;	// just in case...
		currentInput = src;
		do_command(argc, argv);
	}

	/* free the arguments */
	for (i = 0; i < argc; i++) {
		free(argv[i]);
	}
	free(argv);

	return !termRequested;
}


static void
usage(const char *command)
{
	SCPrint(TRUE, stderr, CFSTR("usage: %s\n"), command);
	SCPrint(TRUE, stderr, CFSTR("\tinteractive access to the dynamic store.\n"));
	SCPrint(TRUE, stderr, CFSTR("\n"));
	SCPrint(TRUE, stderr, CFSTR("   or: %s -r nodename\n"), command);
	SCPrint(TRUE, stderr, CFSTR("   or: %s -r address\n"), command);
	SCPrint(TRUE, stderr, CFSTR("   or: %s -r local-address remote-address\n"), command);
	SCPrint(TRUE, stderr, CFSTR("\tcheck reachability of node, address, or address pair.\n"));
	SCPrint(TRUE, stderr, CFSTR("\n"));
	SCPrint(TRUE, stderr, CFSTR("   or: %s -w dynamic-store-key [ -t timeout ]\n"), command);
	SCPrint(TRUE, stderr, CFSTR("\t-w\twait for presense of dynamic store key\n"));
	SCPrint(TRUE, stderr, CFSTR("\t-t\ttime to wait for key\n"));
	SCPrint(TRUE, stderr, CFSTR("\n"));
	SCPrint(TRUE, stderr, CFSTR("   or: %s --get pref\n"), command);
	SCPrint(TRUE, stderr, CFSTR("   or: %s --set pref [newval]\n"), command);
	SCPrint(TRUE, stderr, CFSTR("\tpref\tdisplay (or set) the specified preference.  Valid preferences\n"));
	SCPrint(TRUE, stderr, CFSTR("\t\tinclude:\n"));
	SCPrint(TRUE, stderr, CFSTR("\t\t\tComputerName, LocalHostName\n"));
	SCPrint(TRUE, stderr, CFSTR("\tnewval\tNew preference value to be set.  If not specified,\n"));
	SCPrint(TRUE, stderr, CFSTR("\t\tthe new value will be read from standard input.\n"));
	SCPrint(TRUE, stderr, CFSTR("\n"));
	SCPrint(TRUE, stderr, CFSTR("   or: %s --dns\n"), command);
	SCPrint(TRUE, stderr, CFSTR("\tshow DNS configuration.\n"));
	SCPrint(TRUE, stderr, CFSTR("\n"));
	SCPrint(TRUE, stderr, CFSTR("   or: %s --proxy\n"), command);
	SCPrint(TRUE, stderr, CFSTR("\tshow \"proxy\" configuration.\n"));

	if (getenv("ENABLE_EXPERIMENTAL_SCUTIL_COMMANDS")) {
		SCPrint(TRUE, stderr, CFSTR("\n"));
		SCPrint(TRUE, stderr, CFSTR("   or: %s --net\n"), command);
		SCPrint(TRUE, stderr, CFSTR("\tmanage network configuration.\n"));
	}

	exit (EX_USAGE);
}


static char *
prompt(EditLine *el)
{
	return "> ";
}


int
main(int argc, char * const argv[])
{
	Boolean			dns	= FALSE;
	char			*get	= NULL;
	Boolean			net	= FALSE;
	extern int		optind;
	int			opt;
	int			opti;
	const char		*prog	= argv[0];
	Boolean			proxy	= FALSE;
	Boolean			reach	= FALSE;
	char			*set	= NULL;
	InputRef		src;
	int			timeout	= 15;	/* default timeout (in seconds) */
	char			*wait	= NULL;
	int			xStore	= 0;	/* non dynamic store command line options */

	/* process any arguments */

	while ((opt = getopt_long(argc, argv, "dvprt:w:", longopts, &opti)) != -1)
		switch(opt) {
		case 'd':
			_sc_debug = TRUE;
			_sc_log   = FALSE;	/* enable framework logging */
			break;
		case 'v':
			_sc_verbose = TRUE;
			_sc_log     = FALSE;	/* enable framework logging */
			break;
		case 'p':
			enablePrivateAPI = TRUE;
			break;
		case 'r':
			reach = TRUE;
			xStore++;
			break;
		case 't':
			timeout = atoi(optarg);
			break;
		case 'w':
			wait = optarg;
			xStore++;
			break;
		case 0:
			if        (strcmp(longopts[opti].name, "dns") == 0) {
				dns = TRUE;
				xStore++;
			} else if (strcmp(longopts[opti].name, "get") == 0) {
				get = optarg;
				xStore++;
			} else if (strcmp(longopts[opti].name, "net") == 0) {
				net = TRUE;
				xStore++;
			} else if (strcmp(longopts[opti].name, "proxy") == 0) {
				proxy = TRUE;
				xStore++;
			} else if (strcmp(longopts[opti].name, "set") == 0) {
				set = optarg;
				xStore++;
			}
			break;
		case '?':
		default :
			usage(prog);
		}
	argc -= optind;
	argv += optind;

	if (xStore > 1) {
		// if we are attempting to process more than one type of request
		usage(prog);
	}

	/* are we checking the reachability of a host/address */
	if (reach) {
		if ((argc < 1) || (argc > 2)) {
			usage(prog);
		}
		do_checkReachability(argc, (char **)argv);
		/* NOT REACHED */
	}

	/* are we waiting on the presense of a dynamic store key */
	if (wait) {
		do_wait(wait, timeout);
		/* NOT REACHED */
	}

	/* are we looking up the DNS configuration */
	if (dns) {
		do_showDNSConfiguration(argc, (char **)argv);
		/* NOT REACHED */
	}

	/* are we looking up a preference value */
	if (get) {
		if (findPref(get) < 0) {
			usage(prog);
		}
		do_getPref(get, argc, (char **)argv);
		/* NOT REACHED */
	}

	/* are we looking up the proxy configuration */
	if (proxy) {
		do_showProxyConfiguration(argc, (char **)argv);
		/* NOT REACHED */
	}

	/* are we changing a preference value */
	if (set) {
		if (findPref(set) < 0) {
			usage(prog);
		}
		do_setPref(set, argc, (char **)argv);
		/* NOT REACHED */
	}

	if (net) {
		/* if we are going to be managing the network configuration */
		commands  = (cmdInfo *)commands_prefs;
		nCommands = nCommands_prefs;

		if (!getenv("ENABLE_EXPERIMENTAL_SCUTIL_COMMANDS")) {
			usage(prog);
		}

		do_net_init();		/* initialization */
		do_net_open(0, NULL);	/* open default prefs */
	} else {
		/* if we are going to be managing the dynamic store */
		commands  = (cmdInfo *)commands_store;
		nCommands = nCommands_store;

		do_dictInit(0, NULL);	/* start with an empty dictionary */
		do_open(0, NULL);	/* open the dynamic store */
	}

	/* allocate command input stream */
	src = (InputRef)CFAllocatorAllocate(NULL, sizeof(Input), 0);
	src->fp = stdin;
	src->el = NULL;
	src->h  = NULL;

	if (isatty(fileno(src->fp))) {
		int		editmode	= 1;
		HistEvent	ev;
		struct termios	t;

		if (tcgetattr(fileno(src->fp), &t) != -1) {
			if ((t.c_lflag & ECHO) == 0) {
				editmode = 0;
			}
		}
		src->el = el_init(prog, src->fp, stdout, stderr);
		src->h  = history_init();

		(void)history(src->h, &ev, H_SETSIZE, INT_MAX);
		el_set(src->el, EL_HIST, history, src->h);

		if (!editmode) {
			el_set(src->el, EL_EDITMODE, 0);
		}

		el_set(src->el, EL_EDITOR, "emacs");
		el_set(src->el, EL_PROMPT, prompt);

		el_source(src->el, NULL);

		if ((el_get(src->el, EL_EDITMODE, &editmode) != -1) && editmode != 0) {
			el_set(src->el, EL_SIGNAL, 1);
		} else {
			history_end(src->h);
			src->h = NULL;
			el_end(src->el);
			src->el = NULL;
		}
	}

	while (process_line(src) == TRUE) {
	       /* debug information, diagnostics */
		__showMachPortStatus();
	}

	/* close the socket, free resources */
	if (src->h)	history_end(src->h);
	if (src->el)	el_end(src->el);
	(void)fclose(src->fp);
	CFAllocatorDeallocate(NULL, src);

	exit (EX_OK);	// insure the process exit status is 0
	return 0;	// ...and make main fit the ANSI spec.
}
