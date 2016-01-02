/*  Mach BlitzMail Server -- cty definitions

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/cty.h,v 3.4 98/04/27 13:51:24 davidg Exp Locker: davidg $
    
*/

#define	CTYPORT	"blitzctl"		/* /etc/services name of port to listen on */

struct ctystate {			/* state variables for cty connection */
    t_file		conn;		/* the connection */
    struct sockaddr_in remoteaddr;	/* client's address/port */
    boolean_t		done;		/* closing down? */
    boolean_t		deport_fwd;	/* set forwarding address on deport? */
    boolean_t		deport_cleanout;/* discard messages on deport? */
    char		comline[MAX_STR]; /* command line */
    char		name[MAX_STR];  /* user name */
};

		
typedef struct ctystate ctystate;

boolean_t		cleanout_going;	/* cleanout in progress? */

any_t ctylisten (any_t zot);
