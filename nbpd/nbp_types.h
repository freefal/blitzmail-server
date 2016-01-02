/* definitions for clients of the NBP server 

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

*/

#define NBPMAX          32      /* max NBP name element length */

typedef char nbpstr[NBPMAX+1];  /* long enough for NBP name element */

#define NBP_SERVER_NAME	"NBP-Server"
