/*
	Encryption definitions.

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/cryptutil.h,v 3.0 97/01/27 16:54:13 davidg Exp $
	
*/
#define PW_LEN		8		/* length of password & random # */
#define CRYPTPW_LEN	24		/* length of encrypted octal strings */


void dnd_encrypt(u_char *in, u_char *key, u_char *out, boolean_t edflag);
void fromoctal(char *in, unsigned char out[PW_LEN]);
void tooctal(unsigned char in[PW_LEN], char *out);
void pad_pw(char *pw);
