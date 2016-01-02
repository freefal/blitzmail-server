/*  BlitzMail Server -- configuration

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/config.c,v 3.6 98/10/21 15:59:45 davidg Exp Locker: davidg $	
    
    Read configuration file (at startup).
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <sys/dir.h>
#include <netdb.h>
#include <netinet/in.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "client.h"
#include "queue.h"
#include "t_dnd.h"
#include "smtp.h"
#include "ddp.h"

extern struct sockaddr_in gwaddr;		/* gw address */

void read_config() {

    t_file	*f;
    char	buf[MAX_STR];
    char	cmd[MAX_STR];
    char	dndhost[MAX_STR]; /* dnd host name (lcase) */
    char	*p, *q;
    long	hh = 0;		/* time to do exp check (default = midnight) */
    long	mm = 0;
    long	ss = 0;
    int		i;
    int		dndstat;
    long	l;
    int		dndresolver = -1; /* m_dndresolver setting; -1 if undefined */
    struct hostent *host;	/* host entry for dndhost */
    
    /* allocate server name array, leaving room for -1st entry */
    m_server = mallocf((HOST_MAX+1+1) * sizeof(char *));
    ++m_server;
    
    /* default values (if not overridden by file) */
    u_max = 175;
    u_worry = 150;
    u_timeout = 60;
    dft_expire = DFT_EXPIRE;
    dft_auditexpire = DFT_AUDITEXPIRE;
    dft_trashexpire = DFT_TRASHEXPIRE;
    pubml_fs = PUBML_FS;
    cleanout_grace = DFT_CLEANOUT_GRACE;

    smtp_max = 20;
    smtp_timeout = 20;
    
    m_thisserv = -1;
    
    m_binhexencls = FALSE;	/* enclosure binhexing defaults off */
    m_recvbinhex = TRUE;	/* but defaults *on* when receiving */
    m_smtp_disclaimer = NULL;	/* no disclaimer unless specified */
    
    if ((f = t_fopen(CONFIG_FNAME, O_RDONLY, 0)) == NULL) {
	t_perror1("read_config: cannot open ", CONFIG_FNAME);
	return;
    }
    
    while (t_gets(buf, sizeof(buf),f) != NULL) {
	if ((p = index(buf, ';')) != NULL)
	    *p = 0;			/* nail comments */

    	if (*buf)
	    for (p = buf + strlen(buf) - 1; isascii(*p) && isspace(*p); *p-- = 0)
		;			/* nail trailing spaces */
	  	     
	for (p = buf; isascii(*p) && isspace(*p); ++p)
	    ;				/* skip leading spaces */
	    
	if (!*p)
	    continue;			/* ignore blank lines */
	    	    
	p = strwcpy(cmd, p);		/* get first word */
	
	while(isascii(*p) && isspace(*p)) /* trim arg */
	    ++p;
	
	if (strcasecmp(cmd, "ALIAS") == 0) { /* hostname alias */
	    if (m_aliascnt < HOST_MAX) {
		m_alias[m_aliascnt] = mallocf(strlen(p) + 1);
		strcpy(m_alias[m_aliascnt], p);
		++m_aliascnt;
	    } else
		t_errprint("Config error: too many aliases");
	}
	else if (strcasecmp(cmd, "ATGATEWAY") == 0) {
	    gwaddr.sin_addr.s_addr = inet_addr(p);
	}
	else if (strcasecmp(cmd, "ATNET") == 0) {
	    strtonum(p, &l);
	    if (l > 0xffff || l == 0)	/* 16 bits; 0 reserved */
		t_errprint_l("Config error: Invalid ATNET: %ld", l);
	    else
		my_atnet = l;
	}
	else if (strcasecmp(cmd, "ATZONE") == 0) {
	    p = strqcpy(my_atzone, p);	/* zone name (optionally quoted) */
	    if (strlen(my_atzone) > ZONENAME_MAX) {
	    	t_errprint("Config error: ATZONE name too long!");
		strcpy(my_atzone, "");
	    }
	}
	else if (strcasecmp(cmd, "BINHEXENCLS") == 0) {
	    m_binhexencls = TRUE;
	}
	else if (strcasecmp(cmd, "NORECVBINHEX") == 0) {
	    m_recvbinhex = FALSE;
	}
	else if (strcasecmp(cmd, "CLEANOUT_GRACE") == 0) {
	    p = strtonum(p, &cleanout_grace);	/* grace period for cleanout cmd */
	}
	else if (strcasecmp(cmd, "DOMAIN") == 0) {
	    if (m_domaincnt < HOST_MAX) {
		if (*p != '.' || strlen(p) < 2) {
		    t_errprint("Config error: DOMAIN must begin with '.'");
		} else {
		    m_domain[m_domaincnt] = mallocf(strlen(p) + 1);
		    strcpy(m_domain[m_domaincnt], p);
		    ++m_domaincnt;
		}
	    } else
		t_errprint("Config error: too many domain aliases");
	}

	/* hostname that means "use preferred address" */
	else if (strcasecmp(cmd, "DNDHOST") == 0) {
	    if (m_dndhostcnt < HOST_MAX) {
		m_dndhost[m_dndhostcnt] = mallocf(strlen(p) + 1);
		strcpy(m_dndhost[m_dndhostcnt], p);
		++m_dndhostcnt;
	    } else
		t_errprint("Config error: too many DND host aliases");
	}
	
	/* are we to act as DND resolver? */
	else if (strcasecmp(cmd, "DNDRESOLVER") == 0) {
	    if (strcasecmp(p, "ON") == 0)
	    	dndresolver = TRUE;
	    else if (strcasecmp(p, "OFF") == 0)
	    	dndresolver = FALSE;
	    else
	    	t_errprint("Config error: ON or OFF expected after DNDRESOLVER");
	}
	
	/* host where actual DND server is located */
	else if (strcasecmp(cmd, "DNDSERVER") == 0) {
	    m_dndserver = mallocf(strlen(p) + 1);
	    strcpy(m_dndserver, p);
	}
	
	else if (strcasecmp(cmd, "EXPDATE") == 0) {
	    f_expdate = mallocf(strlen(p) + 1);
	    strcpy(f_expdate, p);
	}

	else if (strcasecmp(cmd, "EXPLOG") == 0) {
	    f_explog = mallocf(strlen(p) + 1);
	    strcpy(f_explog, p);
	}

	else if (strcasecmp(cmd, "EXPTIME") == 0) {
	    p = strtonum(p, &hh);	/* hh[:mm[:ss]] */
	    if (*p++ == ':') {
		p = strtonum(p, &mm);
		if (*p++ == ':')
		    strtonum(p, &ss);
		exp_time = hh*60*60 + mm*60 + ss;
	    }
	}
			
	else if (strcasecmp(cmd, "FS") == 0) { /* filesystem */
	    if (m_filesys_count < FILESYS_MAX) {
		m_filesys[m_filesys_count] = mallocf(strlen(p) + 1);
		strcpy(m_filesys[m_filesys_count], p);
		++m_filesys_count;
	    } else
		t_errprint("Config error: too many filesystems");
	}    
	
	/* primary hostname */
	else if (strcasecmp(cmd, "HOSTNAME") == 0) {
	    m_hostname = mallocf(strlen(p) + 1);
	    strcpy(m_hostname, p);
	    /* also enter it into alias table */
	    if (m_aliascnt < HOST_MAX) {	
		m_alias[m_aliascnt] = mallocf(strlen(p) + 1);
		strcpy(m_alias[m_aliascnt], p);
		++m_aliascnt;
	    } else
		t_errprint("Config error: too many aliases");
	}
	
	else if (strcasecmp(cmd, "INITIALMESS") == 0) {
	    f_initialmess = mallocf(strlen(p) + 1);
	    strcpy(f_initialmess, p);	    
	}
	
	else if (strcasecmp(cmd, "NOAPPLETALK") == 0) {
	    m_noappletalk = TRUE;
	}
	else if (strcasecmp(cmd, "NOTIFYTAB") == 0) {
	    f_notifytab = mallocf(strlen(p) + 1);
	    strcpy(f_notifytab, p);
	}
	
	else if (strcasecmp(cmd, "STICKYTAB") == 0) {
	    f_stickytab = mallocf(strlen(p) + 1);
	    strcpy(f_stickytab, p);
	}
	
	else if (strcasecmp(cmd, "SERVER") == 0) { /* peer server */
	    if (m_servcount < HOST_MAX) {
		if ((q = index(p, ' ')) && strcasecmp(q+1, "LOCAL") == 0) {
		    m_thisserv = m_servcount;	/* this one is us */
		    *q = 0;		/* chop the option */
		}
		m_server[m_servcount] = mallocf(strlen(p) + 1);
		strcpy(m_server[m_servcount], p);
		++m_servcount;
	    } else
		t_errprint("Config error: too many peer servers");
		
	    /* also enter this name as an alias of our primary hostname */
	    if (m_aliascnt < HOST_MAX) {
		m_alias[m_aliascnt] = mallocf(strlen(p) + 1);
		strcpy(m_alias[m_aliascnt], p);
		++m_aliascnt;
	    } else
		t_errprint("Config error: too many aliases");
	    
	}

	else if (strcasecmp(cmd, "SMTPDISCLAIMER") == 0) {
	    m_smtp_disclaimer = mallocf(strlen(p) + 1);
	    strcpy(m_smtp_disclaimer, p);
	}

	else if (strcasecmp(cmd, "SMTPDISCONNECT") == 0) {
	    m_smtpdisconnect = TRUE;	/* close SMTP session after each message */
	}

        else if (strcasecmp(cmd, "SMTPFILTER") == 0) { 
            f_smtpfilter = mallocf(strlen(p) + 1);
            strcpy(f_smtpfilter, p);
	}

	else if (strcasecmp(cmd, "SMTPHOST") == 0) {
	    m_server[SMTP_HOSTNUM] = mallocf(strlen(p) + 1);
	    strcpy(m_server[SMTP_HOSTNUM], p);
	}
	
	else if (strcasecmp(cmd, "SMTPMAX") == 0) {
	    p = strtonum(p, &smtp_max);	/* absolute max users */
	}		

	else if (strcasecmp(cmd, "SMTPTIMEOUT") == 0) {
	    p = strtonum(p, &smtp_timeout); /* idle connection timeout (minutes) */
	}

	else if (strcasecmp(cmd, "STOLOG") == 0) {
	    f_stolog = mallocf(strlen(p) + 1);
	    strcpy(f_stolog, p);
	}
	
	else if (strcasecmp(cmd, "USERMAX") == 0) {
	    p = strtonum(p, &u_max);	/* absolute max users */
	}		

	else if (strcasecmp(cmd, "USERTIMEOUT") == 0) {
	    p = strtonum(p, &u_timeout); /* idle connection timeout (minutes) */
	}

	else if (strcasecmp(cmd, "USERWORRY") == 0) {
	    p = strtonum(p, &u_worry);	/* threshold for more agressive timeouts */
	}	

	/* host allowed to transfer users to us */
	else if (strcasecmp(cmd, "XFEROK") == 0) {
	    if (m_xferokcnt < HOST_MAX) {
		m_xferok[m_xferokcnt] = mallocf(strlen(p) + 1);
		strcpy(m_xferok[m_xferokcnt], p);
		++m_xferokcnt;
	    } else
		t_errprint("Config error: too many XFEROK hosts");
	}
		
	else if (strcasecmp(cmd, "BOXPIG") == 0) {
	    p = strtonum(p, &boxpig);	/* "excessive" mailbox size */
	}	

	else if (strcasecmp(cmd, "MESSMAXLEN") == 0) {
	    p = strtonum(p, &mess_max_len); /* limit on message size */
	    mess_max_len *= 1024;	   	/* units of 1k */
	}

	else if (strcasecmp(cmd, "DFTAUDITEXPIRE") == 0) {
	    dft_auditexpire = mallocf(strlen(p) + 1);
	    strcpy(dft_auditexpire, p);		/* default expiration (days or months) */
	}	

	else if (strcasecmp(cmd, "DFTTRASHEXPIRE") == 0) {
	    dft_trashexpire = mallocf(strlen(p) + 1);
	    strcpy(dft_trashexpire, p);		/* default expiration (days or months) */
	}	

	else if (strcasecmp(cmd, "DFTEXPIRE") == 0) {
	    p = strtonum(p, &dft_expire);	/* default expiration (months) */
	}	

	else if (strcasecmp(cmd, "DNDEXPWARN") == 0) {
	    p = strtonum(p, &dndexp_warn);	/* dnd expiration warning (days) */
	}	

	else if (strcasecmp(cmd, "XFERMESS") == 0) {
	    f_xfermess = mallocf(strlen(p) + 1);
	    strcpy(f_xfermess, p);	    
	}
	else if (strcasecmp(cmd, "DEPORTMESS") == 0) {
	    f_deportmess = mallocf(strlen(p) + 1);
	    strcpy(f_deportmess, p);	    
	}
	
	/* header lines client is allowed to specify */
	else if (strcasecmp(cmd, "OKHEAD") == 0) {
	    if (p[strlen(p)-1] != ':') {
	    	t_errprint("Config error: OKHEAD field name must end with ':'");
	    } else if (m_okheadcnt < HOST_MAX) {
		m_okhead[m_okheadcnt] = mallocf(strlen(p) + 1);
		strcpy(m_okhead[m_okheadcnt], p);
		++m_okheadcnt;
	    } else
		t_errprint("Config error: too many OKHEAD lines");
	}
		
	else if (strcasecmp(cmd, "LOGFILE") == 0) {
	    f_logfile = mallocf(strlen(p) + 1);
	    strcpy(f_logfile, p);
	}  
	else if (strcasecmp(cmd, "WARNING") == 0) {
	    f_warning = mallocf(strlen(p) + 1);
	    strcpy(f_warning, p);
	}  

	else if (strcasecmp(cmd, "MESSID") == 0) {
	    f_messid = mallocf(strlen(p) + 1);
	    strcpy(f_messid, p);
	}  
	else if (strcasecmp(cmd, "PRIVNAME") == 0) {
	    priv_name = mallocf(strlen(p) + 1);
	    strcpy(priv_name, p);
	}  	
	else if (strcasecmp(cmd, "PRIVPW") == 0) {
	    priv_pw = mallocf(strlen(p) + 1);
	    strcpy(priv_pw, p);
	}  
	else if (strcasecmp(cmd, "PUBMLFS") == 0) {
	    pubml_fs = fs_match(p);
	} else if (strcasecmp(cmd, "SPOOLFS") == 0) {
	    m_spoolfs_name = mallocf(strlen(p) + 1);
	    strcpy(m_spoolfs_name, p);	/* name of spool filesystem */
	} else {
	    t_errprint_s("Warning: unknown config command \"%s\" ignored.", cmd);
	} 
    }
    
    t_fclose(f);

    if (m_dndhostcnt == 0) {		/* hostname that resolves to preferred addr */
	for (i = 0; i < m_domaincnt; ++i) {	/* defaults to our domain name(s)... */
	    m_dndhost[i] = mallocf(strlen(m_domain[i]) + 1);	
	    strcpy(m_dndhost[i], m_domain[i]+1); /* ...(w/o leading dot) */
	}
	m_dndhostcnt = m_domaincnt;
    }
    
    if (!m_dndserver) {			/* default DND server if not configured */
	m_dndserver = DFT_DNDSERVER;
    }
    
    /* put sentinels at end of all the lists */
    m_domain[m_domaincnt] = NULL;
    m_dndhost[m_dndhostcnt] = NULL;
    m_alias[m_aliascnt] = NULL;
    m_server[m_servcount] = NULL;
    m_filesys[m_filesys_count] = NULL;
    
    if (m_thisserv == -1) {
	t_errprint("Fatal config error:  no server defined as LOCAL");
	exit(1);
    }
    
    if (!m_spoolfs_name) {
    	t_errprint("Fatal config error:  no spool filesystem defined");
	exit(1);
    }
 
    if (m_filesys_count == 0) {
	t_errprint("Fatal config error:  no mailbox filesystems defined");
	exit(1);
    }
       
    if (!m_hostname) {
    	t_errprint("Fatal config error:  hostname not defined");
	exit(1);
    }
    
    if (m_domaincnt == 0) {
	t_errprint("Fatal config error:  domain not defined");
	exit(1);
    }
        
    if (!f_messid) {
    	t_errprint("Fatal config error:  messid filename not defined");
	exit(1);
    }    

    if (!f_expdate) {
    	t_errprint("Fatal config error:  expdate filename not defined");
	exit(1);
    }    

    if (!f_explog) {
    	t_errprint("Fatal config error:  explog filename not defined");
	exit(1);
    }    
    if (!priv_name) {
    	t_errprint("Fatal config error:  dnd name not defined");
	exit(1);
    }    
    
    if (!priv_pw) {
    	t_errprint("Fatal config error:  dnd pw not defined");
	exit(1);
    }    
         
    if (m_okheadcnt == 0) {		/* default legal client header lines */
    	m_okhead[0] = "MIME-Version:";
	m_okhead[1] = "Content-";	/* Note: matches any Content-* */
	m_okhead[2] = NULL;
	m_okheadcnt = 2;
    }
	
    for (;;) {				/* turn our DND name into uid */
	pubml_uid = name_to_uid(priv_name, &dndstat);
	if (pubml_uid >= 0)		/* got it! */
	    break;
	/* log both recoverable (DND down) and unrecoverable (bad name) errors */
	if (dndstat < 0) {
	    t_errprint_s("%s DND server down; retrying...\n", m_dndserver);
	} else {
	    t_errprint_s("Unable to resolve our DND identity ('%s')!", priv_name);
	    t_errprint_l("    DND error status = %d", dndstat);
	}
	sleep(30);			/* don't spin too fast */
    }
    
    if (pubml_fs < 0) {
     	t_errprint("Fatal config error: invalid pubml_fs");
	exit(1);   	
    }
        
    if (!f_logfile) {
    	t_errprint("Note:  logfile not defined; no logging will occur");
    }     
    
    /* compare to see if spool fs is same as one of the permanent ones */
    m_spool_filesys = fs_match(m_spoolfs_name);
    
    /* construct fqdn of local server */
    m_fullservname = mallocf(strlen(m_server[m_thisserv]) + strlen(m_domain[0]) + 1);
    strcpy(m_fullservname, m_server[m_thisserv]);
    strip_domain(m_fullservname);	/* make sure we don't get domain twice */
    strcat(m_fullservname, m_domain[0]);
    
    /* Determine whether DNDHOST is a real host, or a MX reference to us */
    while (dndresolver == -1) {		/* if no explicit configuration */
	lcase(dndhost, m_dndhost[0]);	/* canonicalize for easy /etc/hosts match */
	sem_seize(&herrno_sem);		/* get access to host routines */

	host = gethostbyname(dndhost);	/* try to get IP address */
	if (host)
	    dndresolver = FALSE;	/* DNDHOST exits; we aren't only resolver */
	else {
	    if (h_errno != TRY_AGAIN) /* hard failure... */
		dndresolver = TRUE;	/* we should act as resolver */
	    else
		sleep(5);		/* TRY_AGAIN -- pause & retry */
	}
	sem_release(&herrno_sem);
    }
    m_dndresolver = dndresolver;	/* save whatever choice we got */
}

/* fs_match --

    Search table of filesystems that have mailboxes for the given name,
    return table index or -1.
*/

int fs_match(char *dnddata) {

    int 	i;
    
    for (i = 0; i < m_filesys_count; ++i) {
	if (strcmp(dnddata, m_filesys[i]) == 0)
	    return i;
    }
    
    return -1;

}
/* uid_to_fs --

    Consult DND to get blitzinfo field for this user; convert to fs index.
*/
int uid_to_fs(long uid, int *fs) {

    struct dndresult	*dndres = NULL;	/* results of dndlookup */
    int			stat;		/* dnd status */
    static char *farray[] = {"BLITZINFO", NULL};
    char		name[16];
    
    t_sprintf(name, "#%ld", uid);	/* construct #uid form */
    stat = t_dndlookup1(name, farray, &dndres);
            
    if (stat == DND_OK)  		/* if ok, match result */	
	*fs = fs_match(t_dndvalue(dndres, "BLITZINFO", farray));
 
    if (dndres)
	t_free(dndres);
	
    return stat;
}
/* strip_domain --

    Remove local domain name from end of address.


*/
void strip_domain(char *hostname) {

    int		i;
    int		pos;
    
    /* check against all forms of local domain name */
    for (i = 0; i < m_domaincnt; i++) {	
	pos = strlen(hostname) - strlen(m_domain[i]);
	if (pos > 0 && strcasecmp(hostname + pos, m_domain[i]) == 0) {
	    hostname[pos] = 0;		/* truncate domain name */
	    break;
	}
    }
}
