/*
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    NBP packet format.
*/
struct nbphdr {                 /* NBP packet layout */
	u_char typecount;	/* packet type // tuple count */
        u_char  id;             /* nbp id */
        };
typedef struct nbphdr nbphdr;

#define NBPHDRLEN	2	/* (note that "sizeof(nbphdr)" may not work) */

#define NBP_TYPE(x) ( ((x) >> 4) & 0xF )
#define NBP_COUNT(x) ( (x) & 0xF )
#define NBP_TYPECOUNT(x,y) ( ((x) << 4) | (y) )

/* NBP types */

#define NBP$BRRQ        1       /* broadcast request */
#define NBP$LKUP        2       /* lookup request */
#define NBP$REPLY       3       /* lookup reply */
#define NBP$FWDREQ      4       /* forward request (phase 2) */

#define DDP$NBP_REG	0xF2	/* NBP registration packet made-up protocol type */
#define NBP$REG		7	/* and command code */
	
/* Echo types */
#define	ECHO_REQUEST	1
#define	ECHO_REPLY	2

#define NBPMAX          32      /* max NBP name element length */

typedef char nbpstr[NBPMAX+1];  /* long enough for NBP name element */

/* "nameaddr" return values */
#define	NBP_OK			0
#define NBP_INVALID_ARGUMENT	-1
#define NBP_NAME_EXISTS		-2

/* Name table.  For now this is a simple linked list.  A more
   performance-conscious implementation would use a hash table.
*/

struct nbpname {
        struct 	nbpname *link;  /* link to next */
        nbpstr 	object;         /* NBP object */
        nbpstr 	type;           /* NBP type */
                              	/* (zone is static) */
        nbpstr 	ucobj;          /* pre-uppercased copies */
        nbpstr 	uctype;         /* ... */
        u_char 	sock;           /* socket owning the name */
	time_t	when;		/* time of registration */	
};

typedef struct nbpname nbpname;

nbpname *nametab;        	/* list head */
pthread_mutex_t namelock;       /* protects the list */
