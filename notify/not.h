#ifndef	_not
#define	_not

/* Module not */

#include <mach/kern_return.h>
#if	(defined(__STDC__) || defined(c_plusplus)) || defined(LINTLIBRARY)
#include <mach/port.h>
#include <mach/message.h>
#endif

#ifndef	mig_external
#define mig_external extern
#endif

mig_external void init_not
#if	(defined(__STDC__) || defined(c_plusplus))
    (port_t rep_port);
#else
    ();
#endif
#include <mach/std_types.h>
#include "../port.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../ddp.h"
#include "not_types.h"

/* Routine notify */
mig_external kern_return_t notify
#if	defined(LINTLIBRARY)
    (reg_port, uid, typ, id, data, len, sticky)
	port_t reg_port;
	int uid;
	int typ;
	int id;
	notifydat data;
	int len;
	boolean_t sticky;
{ return notify(reg_port, uid, typ, id, data, len, sticky); }
#else
#if	(defined(__STDC__) || defined(c_plusplus))
(
	port_t reg_port,
	int uid,
	int typ,
	int id,
	notifydat data,
	int len,
	boolean_t sticky
);
#else
    ();
#endif
#endif

/* Routine notify_clear */
mig_external kern_return_t notify_clear
#if	defined(LINTLIBRARY)
    (reg_port, uid, typ)
	port_t reg_port;
	int uid;
	int typ;
{ return notify_clear(reg_port, uid, typ); }
#else
#if	(defined(__STDC__) || defined(c_plusplus))
(
	port_t reg_port,
	int uid,
	int typ
);
#else
    ();
#endif
#endif

#endif	/* _not */
