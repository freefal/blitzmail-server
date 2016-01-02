#include "not.h"
#include <mach/message.h>
#include <mach/mach_types.h>
#include <mach/mig_errors.h>
#include <mach/msg_type.h>
#if	!defined(KERNEL) && !defined(MIG_NO_STRINGS)
#include <strings.h>
#endif
/* LINTLIBRARY */

extern port_t mig_get_reply_port();
extern void mig_dealloc_reply_port();

#ifndef	mig_internal
#define	mig_internal	static
#endif

#ifndef	TypeCheck
#define	TypeCheck 1
#endif

#ifndef	UseExternRCSId
#ifdef	hc
#define	UseExternRCSId		1
#endif
#endif

#ifndef	UseStaticMsgType
#if	!defined(hc) || defined(__STDC__)
#define	UseStaticMsgType	1
#endif
#endif

#define msg_request_port	msg_remote_port
#define msg_reply_port		msg_local_port

mig_external void init_not
#if	(defined(__STDC__) || defined(c_plusplus))
	(port_t rep_port)
#else
	(rep_port)
	port_t rep_port;
#endif
{
#ifdef	lint
	rep_port++;
#endif
}

/* Routine notify */
mig_external kern_return_t notify
#if	(defined(__STDC__) || defined(c_plusplus))
(
	port_t reg_port,
	int uid,
	int typ,
	int id,
	notifydat data,
	int len,
	boolean_t sticky
)
#else
	(reg_port, uid, typ, id, data, len, sticky)
	port_t reg_port;
	int uid;
	int typ;
	int id;
	notifydat data;
	int len;
	boolean_t sticky;
#endif
{
	typedef struct {
		msg_header_t Head;
		msg_type_t uidType;
		int uid;
		msg_type_t typType;
		int typ;
		msg_type_t idType;
		int id;
		msg_type_t dataType;
		notifydat data;
		char dataPad[2];
		msg_type_t lenType;
		int len;
		msg_type_t stickyType;
		boolean_t sticky;
	} Request;

	typedef struct {
		msg_header_t Head;
		msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	union {
		Request In;
		Reply Out;
	} Mess;

	register Request *InP = &Mess.In;
	register Reply *OutP = &Mess.Out;

	msg_return_t msg_result;

#if	TypeCheck
	boolean_t msg_simple;
#endif	TypeCheck

	unsigned int msg_size = (sizeof(Request));

#if	UseStaticMsgType
	static msg_type_t uidType = {
		/* msg_type_name = */		MSG_TYPE_INTEGER_32,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	UseStaticMsgType

#if	UseStaticMsgType
	static msg_type_t typType = {
		/* msg_type_name = */		MSG_TYPE_INTEGER_32,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	UseStaticMsgType

#if	UseStaticMsgType
	static msg_type_t idType = {
		/* msg_type_name = */		MSG_TYPE_INTEGER_32,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	UseStaticMsgType

#if	UseStaticMsgType
	static msg_type_t dataType = {
		/* msg_type_name = */		MSG_TYPE_CHAR,
		/* msg_type_size = */		8,
		/* msg_type_number = */		578,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	UseStaticMsgType

#if	UseStaticMsgType
	static msg_type_t lenType = {
		/* msg_type_name = */		MSG_TYPE_INTEGER_32,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	UseStaticMsgType

#if	UseStaticMsgType
	static msg_type_t stickyType = {
		/* msg_type_name = */		MSG_TYPE_BOOLEAN,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	UseStaticMsgType

#if	TypeCheck && UseStaticMsgType
	static msg_type_t RetCodeCheck = {
		/* msg_type_name = */		MSG_TYPE_INTEGER_32,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	/* TypeCheck && UseStaticMsgType */

#if	UseStaticMsgType
	InP->uidType = uidType;
#else	UseStaticMsgType
	InP->uidType.msg_type_name = MSG_TYPE_INTEGER_32;
	InP->uidType.msg_type_size = 32;
	InP->uidType.msg_type_number = 1;
	InP->uidType.msg_type_inline = TRUE;
	InP->uidType.msg_type_longform = FALSE;
	InP->uidType.msg_type_deallocate = FALSE;
	InP->uidType.msg_type_unused = 0;
#endif	UseStaticMsgType

	InP->uid /* uid */ = /* uid */ uid;

#if	UseStaticMsgType
	InP->typType = typType;
#else	UseStaticMsgType
	InP->typType.msg_type_name = MSG_TYPE_INTEGER_32;
	InP->typType.msg_type_size = 32;
	InP->typType.msg_type_number = 1;
	InP->typType.msg_type_inline = TRUE;
	InP->typType.msg_type_longform = FALSE;
	InP->typType.msg_type_deallocate = FALSE;
	InP->typType.msg_type_unused = 0;
#endif	UseStaticMsgType

	InP->typ /* typ */ = /* typ */ typ;

#if	UseStaticMsgType
	InP->idType = idType;
#else	UseStaticMsgType
	InP->idType.msg_type_name = MSG_TYPE_INTEGER_32;
	InP->idType.msg_type_size = 32;
	InP->idType.msg_type_number = 1;
	InP->idType.msg_type_inline = TRUE;
	InP->idType.msg_type_longform = FALSE;
	InP->idType.msg_type_deallocate = FALSE;
	InP->idType.msg_type_unused = 0;
#endif	UseStaticMsgType

	InP->id /* id */ = /* id */ id;

#if	UseStaticMsgType
	InP->dataType = dataType;
#else	UseStaticMsgType
	InP->dataType.msg_type_name = MSG_TYPE_CHAR;
	InP->dataType.msg_type_size = 8;
	InP->dataType.msg_type_number = 578;
	InP->dataType.msg_type_inline = TRUE;
	InP->dataType.msg_type_longform = FALSE;
	InP->dataType.msg_type_deallocate = FALSE;
	InP->dataType.msg_type_unused = 0;
#endif	UseStaticMsgType

	{ typedef struct { char data[578]; } *sp; * (sp) InP->data /* data */ = * (sp) /* data */ data; }

#if	UseStaticMsgType
	InP->lenType = lenType;
#else	UseStaticMsgType
	InP->lenType.msg_type_name = MSG_TYPE_INTEGER_32;
	InP->lenType.msg_type_size = 32;
	InP->lenType.msg_type_number = 1;
	InP->lenType.msg_type_inline = TRUE;
	InP->lenType.msg_type_longform = FALSE;
	InP->lenType.msg_type_deallocate = FALSE;
	InP->lenType.msg_type_unused = 0;
#endif	UseStaticMsgType

	InP->len /* len */ = /* len */ len;

#if	UseStaticMsgType
	InP->stickyType = stickyType;
#else	UseStaticMsgType
	InP->stickyType.msg_type_name = MSG_TYPE_BOOLEAN;
	InP->stickyType.msg_type_size = 32;
	InP->stickyType.msg_type_number = 1;
	InP->stickyType.msg_type_inline = TRUE;
	InP->stickyType.msg_type_longform = FALSE;
	InP->stickyType.msg_type_deallocate = FALSE;
	InP->stickyType.msg_type_unused = 0;
#endif	UseStaticMsgType

	InP->sticky /* sticky */ = /* sticky */ sticky;

	InP->Head.msg_simple = TRUE;
	InP->Head.msg_size = msg_size;
	InP->Head.msg_type = MSG_TYPE_NORMAL | MSG_TYPE_RPC;
	InP->Head.msg_request_port = reg_port;
	InP->Head.msg_reply_port = mig_get_reply_port();
	InP->Head.msg_id = 100;

	msg_result = msg_rpc(&InP->Head, RCV_TIMEOUT, sizeof(Reply), 0, 60000);
	if (msg_result != RPC_SUCCESS) {
		if ((msg_result == RCV_INVALID_PORT) ||
		    (msg_result == RCV_TIMED_OUT))
			mig_dealloc_reply_port();
		return msg_result;
	}

#if	TypeCheck
	msg_size = OutP->Head.msg_size;
	msg_simple = OutP->Head.msg_simple;
#endif	TypeCheck

	if (OutP->Head.msg_id != 200)
		return MIG_REPLY_MISMATCH;

#if	TypeCheck
	if (((msg_size != (sizeof(Reply))) || (msg_simple != TRUE)) &&
	    ((msg_size != sizeof(death_pill_t)) ||
	     (msg_simple != TRUE) ||
	     (OutP->RetCode == KERN_SUCCESS)))
		return MIG_TYPE_ERROR;
#endif	TypeCheck

#if	TypeCheck
#if	UseStaticMsgType
	if (* (int *) &OutP->RetCodeType != * (int *) &RetCodeCheck)
#else	UseStaticMsgType
	if ((OutP->RetCodeType.msg_type_inline != TRUE) ||
	    (OutP->RetCodeType.msg_type_longform != FALSE) ||
	    (OutP->RetCodeType.msg_type_name != MSG_TYPE_INTEGER_32) ||
	    (OutP->RetCodeType.msg_type_number != 1) ||
	    (OutP->RetCodeType.msg_type_size != 32))
#endif	UseStaticMsgType
		return MIG_TYPE_ERROR;
#endif	TypeCheck

	if (OutP->RetCode != KERN_SUCCESS)
		return OutP->RetCode;

	return OutP->RetCode;
}

/* Routine notify_clear */
mig_external kern_return_t notify_clear
#if	(defined(__STDC__) || defined(c_plusplus))
(
	port_t reg_port,
	int uid,
	int typ
)
#else
	(reg_port, uid, typ)
	port_t reg_port;
	int uid;
	int typ;
#endif
{
	typedef struct {
		msg_header_t Head;
		msg_type_t uidType;
		int uid;
		msg_type_t typType;
		int typ;
	} Request;

	typedef struct {
		msg_header_t Head;
		msg_type_t RetCodeType;
		kern_return_t RetCode;
	} Reply;

	union {
		Request In;
		Reply Out;
	} Mess;

	register Request *InP = &Mess.In;
	register Reply *OutP = &Mess.Out;

	msg_return_t msg_result;

#if	TypeCheck
	boolean_t msg_simple;
#endif	TypeCheck

	unsigned int msg_size = (sizeof(Request));

#if	UseStaticMsgType
	static msg_type_t uidType = {
		/* msg_type_name = */		MSG_TYPE_INTEGER_32,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	UseStaticMsgType

#if	UseStaticMsgType
	static msg_type_t typType = {
		/* msg_type_name = */		MSG_TYPE_INTEGER_32,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	UseStaticMsgType

#if	TypeCheck && UseStaticMsgType
	static msg_type_t RetCodeCheck = {
		/* msg_type_name = */		MSG_TYPE_INTEGER_32,
		/* msg_type_size = */		32,
		/* msg_type_number = */		1,
		/* msg_type_inline = */		TRUE,
		/* msg_type_longform = */	FALSE,
		/* msg_type_deallocate = */	FALSE,
		/* msg_type_unused = */	0
	};
#endif	/* TypeCheck && UseStaticMsgType */

#if	UseStaticMsgType
	InP->uidType = uidType;
#else	UseStaticMsgType
	InP->uidType.msg_type_name = MSG_TYPE_INTEGER_32;
	InP->uidType.msg_type_size = 32;
	InP->uidType.msg_type_number = 1;
	InP->uidType.msg_type_inline = TRUE;
	InP->uidType.msg_type_longform = FALSE;
	InP->uidType.msg_type_deallocate = FALSE;
	InP->uidType.msg_type_unused = 0;
#endif	UseStaticMsgType

	InP->uid /* uid */ = /* uid */ uid;

#if	UseStaticMsgType
	InP->typType = typType;
#else	UseStaticMsgType
	InP->typType.msg_type_name = MSG_TYPE_INTEGER_32;
	InP->typType.msg_type_size = 32;
	InP->typType.msg_type_number = 1;
	InP->typType.msg_type_inline = TRUE;
	InP->typType.msg_type_longform = FALSE;
	InP->typType.msg_type_deallocate = FALSE;
	InP->typType.msg_type_unused = 0;
#endif	UseStaticMsgType

	InP->typ /* typ */ = /* typ */ typ;

	InP->Head.msg_simple = TRUE;
	InP->Head.msg_size = msg_size;
	InP->Head.msg_type = MSG_TYPE_NORMAL | MSG_TYPE_RPC;
	InP->Head.msg_request_port = reg_port;
	InP->Head.msg_reply_port = mig_get_reply_port();
	InP->Head.msg_id = 101;

	msg_result = msg_rpc(&InP->Head, RCV_TIMEOUT, sizeof(Reply), 0, 60000);
	if (msg_result != RPC_SUCCESS) {
		if ((msg_result == RCV_INVALID_PORT) ||
		    (msg_result == RCV_TIMED_OUT))
			mig_dealloc_reply_port();
		return msg_result;
	}

#if	TypeCheck
	msg_size = OutP->Head.msg_size;
	msg_simple = OutP->Head.msg_simple;
#endif	TypeCheck

	if (OutP->Head.msg_id != 201)
		return MIG_REPLY_MISMATCH;

#if	TypeCheck
	if (((msg_size != (sizeof(Reply))) || (msg_simple != TRUE)) &&
	    ((msg_size != sizeof(death_pill_t)) ||
	     (msg_simple != TRUE) ||
	     (OutP->RetCode == KERN_SUCCESS)))
		return MIG_TYPE_ERROR;
#endif	TypeCheck

#if	TypeCheck
#if	UseStaticMsgType
	if (* (int *) &OutP->RetCodeType != * (int *) &RetCodeCheck)
#else	UseStaticMsgType
	if ((OutP->RetCodeType.msg_type_inline != TRUE) ||
	    (OutP->RetCodeType.msg_type_longform != FALSE) ||
	    (OutP->RetCodeType.msg_type_name != MSG_TYPE_INTEGER_32) ||
	    (OutP->RetCodeType.msg_type_number != 1) ||
	    (OutP->RetCodeType.msg_type_size != 32))
#endif	UseStaticMsgType
		return MIG_TYPE_ERROR;
#endif	TypeCheck

	if (OutP->RetCode != KERN_SUCCESS)
		return OutP->RetCode;

	return OutP->RetCode;
}
