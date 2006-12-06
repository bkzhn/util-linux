/* This program is derived from 4.3 BSD software and is
   subject to the copyright notice below.
   
   The port to HP-UX has been motivated by the incapability
   of 'rlogin'/'rlogind' as per HP-UX 6.5 (and 7.0) to transfer window sizes.
   
   Changes:
   
   - General HP-UX portation. Use of facilities not available
     in HP-UX (e.g. setpriority) has been eliminated.
     Utmp/wtmp handling has been ported.
   
   - The program uses BSD command line options to be used
     in connection with e.g. 'rlogind' i.e. 'new login'.
   
   - HP features left out:          logging of bad login attempts in /etc/btmp,
                                    they are sent to syslog
   
				    password expiry
   
				    '*' as login shell, add it if you need it

   - BSD features left out:         quota checks
   	                            password expiry
				    analysis of terminal type (tset feature)
   
   - BSD features thrown in:        Security logging to syslogd.
                                    This requires you to have a (ported) syslog
				    system -- 7.0 comes with syslog
   
				    'Lastlog' feature.
   
   - A lot of nitty gritty details has been adjusted in favour of
     HP-UX, e.g. /etc/securetty, default paths and the environment
     variables assigned by 'login'.
   
   - We do *nothing* to setup/alter tty state, under HP-UX this is
     to be done by getty/rlogind/telnetd/some one else.
   
   Michael Glad (glad@daimi.dk)
   Computer Science Department
   Aarhus University
   Denmark
   
   1990-07-04
   
   1991-09-24 glad@daimi.aau.dk: HP-UX 8.0 port:
   - now explictly sets non-blocking mode on descriptors
   - strcasecmp is now part of HP-UX

   1992-02-05 poe@daimi.aau.dk: Ported the stuff to Linux 0.12
   From 1992 till now (1997) this code for Linux has been maintained at
   ftp.daimi.aau.dk:/pub/linux/poe/
 */

/*
 * Copyright (c) 1980, 1987, 1988 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef lint
char copyright[] =
  "@(#) Copyright (c) 1980, 1987, 1988 The Regents of the University of California.\n\
 All rights reserved.\n";
#endif /* not lint */

#ifndef lint
static char sccsid[] = "@(#)login.c	5.40 (Berkeley) 5/9/89";
#endif /* not lint */

/*
 * login [ name ]
 * login -h hostname	(for telnetd, etc.)
 * login -f name	(for pre-authenticated login: datakit, xterm, etc.)
 */

#define _GNU_SOURCE		/* to get definition of snprintf */

/* #define TESTING */

#ifdef TESTING
#include "param.h"
#else
#include <sys/param.h>
#endif

#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>
#include <memory.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <termios.h>
#include <string.h>
#define index strchr
#define rindex strrchr
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <utmp.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syslog.h>
#include <sys/sysmacros.h>
#include <netdb.h>

#ifdef __linux__
#  include <sys/sysmacros.h>
#  include <linux/major.h>
#endif

#ifdef TESTING
#  include "utmp.h"
#else
#  include <utmp.h>
#endif

#ifdef SHADOW_PWD
#  include <shadow.h>
#endif

#ifdef USE_PAM
#  include <security/pam_appl.h>
#  include <security/pam_misc.h>
#  define PAM_MAX_LOGIN_TRIES	3
#  define PAM_FAIL_CHECK if (retcode != PAM_SUCCESS) { \
       fprintf(stderr,"\n%s\n",pam_strerror(retcode)); \
       syslog(LOG_ERR,"%s",pam_strerror(retcode)); \
       pam_end(pamh, retcode); exit(1); \
   }
#  define PAM_END { retcode = pam_close_session(pamh,0); \
		    pam_end(pamh,retcode); }
#  define PAM_END { \
       retcode = pam_close_session(pamh,0); pam_end(pamh,retcode); \
}
#endif

#ifndef __linux__
#  include <tzfile.h>
#endif
#include <lastlog.h>

#define SLEEP_EXIT_TIMEOUT 5

#if 0
/* from before we had a lastlog.h file in linux */
struct  lastlog
{ long ll_time;
  char ll_line[12];
  char ll_host[16];
};
#endif

#include "pathnames.h"

#define P_(s) ()
void opentty P_((const char *tty));
void getloginname P_((void));
void timedout P_((void));
int rootterm P_((char *ttyn));
void motd P_((void));
void sigint P_((void));
void checknologin P_((void));
void dolastlog P_((int quiet));
void badlogin P_((const char *name));
char *stypeof P_((char *ttyid));
void checktty P_((char *user, char *tty, struct passwd *pwd));
void sleepexit P_((int eval));
#ifdef CRYPTOCARD
int cryptocard P_((void));
#endif
#undef P_

#ifdef	KERBEROS
#include <kerberos/krb.h>
#include <sys/termios.h>
char	realm[REALM_SZ];
int	kerror = KSUCCESS, notickets = 1;
#endif

#ifdef USE_TTY_GROUP
#  define TTY_MODE 0620
#else
#  define TTY_MODE 0600
#endif

#define	TTYGRPNAME	"tty"		/* name of group to own ttys */
/**#  define TTYGRPNAME      "other" **/

#ifndef MAXPATHLEN
#  define MAXPATHLEN 1024
#endif

/*
 * This bounds the time given to login.  Not a define so it can
 * be patched on machines where it's too small.
 */
#ifndef __linux__
int	timeout = 300;
#else
int     timeout = 60;
#endif

struct	passwd *pwd;
int	failures = 1;
char	term[64], *hostname, *username, *tty;
struct  hostent hostaddress;
char	thishost[100];

#ifndef __linux__
struct	sgttyb sgttyb;
struct	tchars tc = {
    CINTR, CQUIT, CSTART, CSTOP, CEOT, CBRK
  };
struct	ltchars ltc = {
    CSUSP, CDSUSP, CRPRNT, CFLUSH, CWERASE, CLNEXT
  };
#endif

const char *months[] =
{ "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
    "Sep", "Oct", "Nov", "Dec" };

/* provided by Linus Torvalds 16-Feb-93 */
void 
opentty(const char * tty)
{
    int i;
    int fd = open(tty, O_RDWR);
    
    for (i = 0 ; i < fd ; i++)
      close(i);
    for (i = 0 ; i < 3 ; i++)
      dup2(fd, i);
    if (fd >= 3)
      close(fd);
}

/* true if the filedescriptor fd is a console tty, very Linux specific */
static int
consoletty(int fd)
{
#ifdef __linux__
    struct stat stb;

    if ((fstat(fd, &stb) >= 0) 
	&& (major(stb.st_rdev) == TTY_MAJOR)
	&& (minor(stb.st_rdev) < 64)) {
	return 1;
    }
#endif
    return 0;
}


int
main(argc, argv)
     int argc;
     char **argv;
{
    extern int errno, optind;
    extern char *optarg, **environ;
    struct group *gr;
    register int ch;
    register char *p;
    int ask, fflag, hflag, pflag, cnt;
    int quietlog, passwd_req, ioctlval;
    char *domain, *salt, *ttyn, *pp;
    char tbuf[MAXPATHLEN + 2], tname[sizeof(_PATH_TTY) + 10];
    char *ctime(), *ttyname(), *stypeof();
    time_t time();
    void timedout();
    char *termenv;
    char vcsn[20], vcsan[20];
    char * childArgv[10];
    char * buff;
    int childArgc = 0;
    int error = 0;
#ifdef USE_PAM
    int retcode;
    pam_handle_t *pamh = NULL;
    struct pam_conv conv = { misc_conv, NULL };
    pid_t childPid;
    int childStatus;
    void * oldSigHandler;
#endif


#ifdef __linux__
    char tmp[100];
    /* Just as arbitrary as mountain time: */
    /* (void)setenv("TZ", "MET-1DST",0); */
#endif
    
    signal(SIGALRM, timedout);
    alarm((unsigned int)timeout);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    
    setpriority(PRIO_PROCESS, 0, 0);
#ifdef HAVE_QUOTA
    quota(Q_SETUID, 0, 0, 0);
#endif
    
    /*
     * -p is used by getty to tell login not to destroy the environment
     * -f is used to skip a second login authentication 
     * -h is used by other servers to pass the name of the remote
     *    host to login so that it may be placed in utmp and wtmp
     */
    gethostname(tbuf, sizeof(tbuf));
    strncpy(thishost, tbuf, sizeof(thishost)-1);
    thishost[sizeof(thishost)-1] = 0;
    domain = index(tbuf, '.');
    
    username = tty = hostname = NULL;
    fflag = hflag = pflag = 0;
    passwd_req = 1;
    while ((ch = getopt(argc, argv, "fh:p")) != EOF)
      switch (ch) {
	case 'f':
	  fflag = 1;
	  break;
	  
	case 'h':
	  if (getuid()) {
	      fprintf(stderr,
		      "login: -h for super-user only.\n");
	      exit(1);
	  }
	  hflag = 1;
	  if (domain && (p = index(optarg, '.')) &&
	      strcasecmp(p, domain) == 0)
	    *p = 0;
	  hostname = optarg;
	  { 
	      struct hostent *he = gethostbyname(hostname);
	      if (he) {
		  memcpy(&hostaddress, he, sizeof(hostaddress));
	      } else {
		  memset(&hostaddress, 0, sizeof(hostaddress));
	      }
	  }
	  break;
	  
	case 'p':
	  pflag = 1;
	  break;

	case '?':
	default:
	  fprintf(stderr,
		  "usage: login [-fp] [username]\n");
	  exit(1);
      }
    argc -= optind;
    argv += optind;
    if (*argv) {
	username = *argv;
	ask = 0;
    } else
      ask = 1;
    
#ifndef __linux__
    ioctlval = 0;
    ioctl(0, TIOCLSET, &ioctlval);
    ioctl(0, TIOCNXCL, 0);
    fcntl(0, F_SETFL, ioctlval);
    ioctl(0, TIOCGETP, &sgttyb);
    sgttyb.sg_erase = CERASE;
    sgttyb.sg_kill = CKILL;
    ioctl(0, TIOCSLTC, &ltc);
    ioctl(0, TIOCSETC, &tc);
    ioctl(0, TIOCSETP, &sgttyb);
    
    /*
     * Be sure that we're in
     * blocking mode!!!
     * This is really for HPUX
     */
    ioctlval = 0;
    ioctl(0, FIOSNBIO, &ioctlval);
#endif
    
    for (cnt = getdtablesize(); cnt > 2; cnt--)
      close(cnt);
    
    ttyn = ttyname(0);
    if (ttyn == NULL || *ttyn == '\0') {
	snprintf(tname, sizeof(tname), "%s??", _PATH_TTY);
	ttyn = tname;
    }

    /* find names of Virtual Console devices, for later mode change */
    {
	char *p = ttyn;
	/* find number of tty */
	while (*p && !isdigit(*p)) p++;

	strcpy(vcsn, "/dev/vcs"); strcat(vcsn, p);
	strcpy(vcsan, "/dev/vcsa"); strcat(vcsan, p);
    }

    setpgrp();
    
    {
	struct termios tt, ttt;
	
	tcgetattr(0, &tt);
	ttt = tt;
	ttt.c_cflag &= ~HUPCL;
	
	if((chown(ttyn, 0, 0) == 0) && (chmod(ttyn, 0622) == 0)) {
	    tcsetattr(0,TCSAFLUSH,&ttt);
	    signal(SIGHUP, SIG_IGN); /* so vhangup() wont kill us */
	    vhangup();
	    signal(SIGHUP, SIG_DFL);
	}
	
	setsid();
	
	/* re-open stdin,stdout,stderr after vhangup() closed them */
	/* if it did, after 0.99.5 it doesn't! */
	opentty(ttyn);
	tcsetattr(0,TCSAFLUSH,&tt);
    }
    
    if ((tty = rindex(ttyn, '/')))
      ++tty;
    else
      tty = ttyn;
    
    openlog("login", LOG_ODELAY, LOG_AUTHPRIV);

#ifdef USE_PAM
    /* username is initialized to NULL
       and if specified on the command line it is set.
       Therefore, we are safe not setting it to anything
    */

    retcode = pam_start("login",username, &conv, &pamh);
    if(retcode != PAM_SUCCESS) {
	fprintf(stderr,"login: PAM Failure, aborting: %s\n",
	pam_strerror(retcode));
	syslog(LOG_ERR,"Couldn't initialize PAM: %s", pam_strerror(retcode));
	exit(99);
    }
    /* hostname & tty are either set to NULL or their correct values,
       depending on how much we know */
    retcode = pam_set_item(pamh, PAM_RHOST, hostname);
    PAM_FAIL_CHECK;
    retcode = pam_set_item(pamh, PAM_TTY, tty);
    PAM_FAIL_CHECK;
    /* if fflag == 1, then the user has already been authenticated */
    if (fflag && (getuid() == 0))
	passwd_req = 0;
    else
	passwd_req = 1;

    if(passwd_req == 1) {
	int failcount=0;

	/* there may be better ways to deal with some of these
	   conditions, but at least this way I don't think we'll
	   be giving away information... */
	/* Perhaps someday we can trust that all PAM modules will
	   pay attention to failure count and get rid of MAX_LOGIN_TRIES? */

	retcode = pam_authenticate(pamh, 0);
	while((failcount++ < PAM_MAX_LOGIN_TRIES) &&
	      ((retcode == PAM_AUTH_ERR) ||
	       (retcode == PAM_USER_UNKNOWN) ||
	       (retcode == PAM_CRED_INSUFFICIENT) ||
	       (retcode == PAM_AUTHINFO_UNAVAIL))) {
	    pam_get_item(pamh, PAM_USER, (const void **) &username);
	    syslog(LOG_NOTICE,"FAILED LOGIN %d FROM %s FOR %s, %s",
	    failcount, hostname,username,pam_strerror(retcode));
	    fprintf(stderr,"Login incorrect\n\n");
	    pam_set_item(pamh,PAM_USER,NULL);
	    retcode = pam_authenticate(pamh, 0);
	}

	if (retcode != PAM_SUCCESS) {
	    pam_get_item(pamh, PAM_USER, (const void **) &username);

	    if (retcode == PAM_MAXTRIES) 
		syslog(LOG_NOTICE,"TOO MANY LOGIN TRIES (%d) FROM %s FOR "
			"%s, %s", failcount, hostname, username,
			 pam_strerror(retcode));
	    else
		syslog(LOG_NOTICE,"FAILED LOGIN SESSION FROM %s FOR %s, %s",
			hostname, username, pam_strerror(retcode));

	    fprintf(stderr,"\nLogin incorrect\n");
	    pam_end(pamh, retcode);
	    exit(0);
	}

	retcode = pam_acct_mgmt(pamh, 0);

	if(retcode == PAM_AUTHTOKEN_REQD) {
	    retcode = pam_chauthtok(pamh, PAM_CHANGE_EXPIRED_AUTHTOK);
	}

	PAM_FAIL_CHECK;
    }

    /* Grab the user information out of the password file for future usage
       First get the username that we are actually using, though.
    */
    retcode = pam_get_item(pamh, PAM_USER, (const void **) &username);
    setpwent();
    pwd = getpwnam(username);
    if (pwd) initgroups(username, pwd->pw_gid);

    retcode = pam_setcred(pamh, PAM_CRED_ESTABLISH);
    PAM_FAIL_CHECK;

    retcode = pam_open_session(pamh, 0);
    PAM_FAIL_CHECK;

#else /* ! USE_PAM */

    for (cnt = 0;; ask = 1) {
	ioctlval = 0;
#  ifndef __linux__
	ioctl(0, TIOCSETD, &ioctlval);
#  endif
	
	if (ask) {
	    fflag = 0;
	    getloginname();
	}
	
	/* Dirty patch to fix a gigantic security hole when using 
	   yellow pages. This problem should be solved by the
	   libraries, and not by programs, but this must be fixed
	   urgently! If the first char of the username is '+', we 
	   avoid login success.
	   Feb 95 <alvaro@etsit.upm.es> */
	
	if (username[0] == '+') {
	    puts("Illegal username");
	    badlogin(username);
	    sleepexit(1);
	}
	
	/* (void)strcpy(tbuf, username); why was this here? */
	if ((pwd = getpwnam(username))) {
#  ifdef SHADOW_PWD
	    struct spwd *sp;
	    
	    if ((sp = getspnam(username)))
	      pwd->pw_passwd = sp->sp_pwdp;
#  endif
	    salt = pwd->pw_passwd;
	} else
	  salt = "xx";
	
	if (pwd) {
	    initgroups(username, pwd->pw_gid);
	    checktty(username, tty, pwd); /* in checktty.c */
	}
	
	/* if user not super-user, check for disabled logins */
	if (pwd == NULL || pwd->pw_uid)
	  checknologin();
	
	/*
	 * Disallow automatic login to root; if not invoked by
	 * root, disallow if the uid's differ.
	 */
	if (fflag && pwd) {
	    int uid = getuid();
	    
	    passwd_req = pwd->pw_uid == 0 ||
	      (uid && uid != pwd->pw_uid);
	}
	
	/*
	 * If trying to log in as root, but with insecure terminal,
	 * refuse the login attempt.
	 */
	if (pwd && pwd->pw_uid == 0 && !rootterm(tty)) {
	    fprintf(stderr,
		    "%s login refused on this terminal.\n",
		    pwd->pw_name);
	    
	    if (hostname)
	      syslog(LOG_NOTICE,
		     "LOGIN %s REFUSED FROM %s ON TTY %s",
		     pwd->pw_name, hostname, tty);
	    else
	      syslog(LOG_NOTICE,
		     "LOGIN %s REFUSED ON TTY %s",
		     pwd->pw_name, tty);
	    continue;
	}
	
	/*
	 * If no pre-authentication and a password exists
	 * for this user, prompt for one and verify it.
	 */
	if (!passwd_req || (pwd && !*pwd->pw_passwd))
	  break;
	
	setpriority(PRIO_PROCESS, 0, -4);
	pp = getpass("Password: ");
	
#  ifdef CRYPTOCARD
	if (strncmp(pp, "CRYPTO", 6) == 0) {
	    if (pwd && cryptocard()) break;
	}
#  endif /* CRYPTOCARD */
	
	p = crypt(pp, salt);
	setpriority(PRIO_PROCESS, 0, 0);

#  ifdef KERBEROS
	/*
	 * If not present in pw file, act as we normally would.
	 * If we aren't Kerberos-authenticated, try the normal
	 * pw file for a password.  If that's ok, log the user
	 * in without issueing any tickets.
	 */
	
	if (pwd && !krb_get_lrealm(realm,1)) {
	    /*
	     * get TGT for local realm; be careful about uid's
	     * here for ticket file ownership
	     */
	    setreuid(geteuid(),pwd->pw_uid);
	    kerror = krb_get_pw_in_tkt(pwd->pw_name, "", realm,
				       "krbtgt", realm, DEFAULT_TKT_LIFE, pp);
	    setuid(0);
	    if (kerror == INTK_OK) {
		memset(pp, 0, strlen(pp));
		notickets = 0;	/* user got ticket */
		break;
	    }
	}
#  endif /* KERBEROS */
	memset(pp, 0, strlen(pp));
	if (pwd && !strcmp(p, pwd->pw_passwd))
	  break;
	
	printf("Login incorrect\n");
	badlogin(username); /* log ALL bad logins */
	failures++;
	
	/* we allow 10 tries, but after 3 we start backing off */
	if (++cnt > 3) {
	    if (cnt >= 10) {
		sleepexit(1);
	    }
	    sleep((unsigned int)((cnt - 3) * 5));
	}
    }
#endif /* !USE_PAM */
    
    /* committed to login -- turn off timeout */
    alarm((unsigned int)0);
    
#ifdef HAVE_QUOTA
    if (quota(Q_SETUID, pwd->pw_uid, 0, 0) < 0 && errno != EINVAL) {
	switch(errno) {
	  case EUSERS:
	    fprintf(stderr,
		    "Too many users logged on already.\nTry again later.\n");
	    break;
	  case EPROCLIM:
	    fprintf(stderr,
		    "You have too many processes running.\n");
	    break;
	  default:
	    perror("quota (Q_SETUID)");
	}
	sleepexit(0);
    }
#endif
    
    /* paranoia... */
#ifdef SHADOW_PWD
    endspent();
#endif
    endpwent();
    
    /* This requires some explanation: As root we may not be able to
       read the directory of the user if it is on an NFS mounted
       filesystem. We temporarily set our effective uid to the user-uid
       making sure that we keep root privs. in the real uid. 
       
       A portable solution would require a fork(), but we rely on Linux
       having the BSD setreuid() */
    
    {
	char tmpstr[MAXPATHLEN];
	uid_t ruid = getuid();
	gid_t egid = getegid();
	
	snprintf(tmpstr, sizeof(tmpstr), 
		 "%s/%s", pwd->pw_dir, _PATH_HUSHLOGIN);
	
	setregid(-1, pwd->pw_gid);
	setreuid(0, pwd->pw_uid);
	quietlog = (access(tmpstr, R_OK) == 0);
	setuid(0); /* setreuid doesn't do it alone! */
	setreuid(ruid, 0);
	setregid(-1, egid);
    }
    
#ifndef __linux__
#  ifdef KERBEROS
    if (notickets && !quietlog)
      printf("Warning: no Kerberos tickets issued\n");
#  endif
    
#  ifndef USE_PAM			/* PAM does all of this for us */
#    define TWOWEEKS	(14*24*60*60)
    if (pwd->pw_change || pwd->pw_expire) {
	struct timeval tp;

	gettimeofday(&tp, (struct timezone *)NULL);

	if (pwd->pw_change) {
	    if (tp.tv_sec >= pwd->pw_change) {
		printf("Sorry -- your password has expired.\n");
		sleepexit(1);
	    }
	    else if (tp.tv_sec - pwd->pw_change < TWOWEEKS && !quietlog) {
		struct tm *ttp;
		ttp = localtime(&pwd->pw_change);
		printf("Warning: your password expires on %s %d, %d\n",
		       months[ttp->tm_mon], ttp->tm_mday,
		       TM_YEAR_BASE + ttp->tm_year);
	    }
	}

	if (pwd->pw_expire) {
	    if (tp.tv_sec >= pwd->pw_expire) {
		printf("Sorry -- your account has expired.\n");
		sleepexit(1);
	    }
	    else if (tp.tv_sec - pwd->pw_expire < TWOWEEKS && !quietlog) {
		struct tm *ttp;
		ttp = localtime(&pwd->pw_expire);
		printf("Warning: your account expires on %s %d, %d\n",
		       months[ttp->tm_mon], ttp->tm_mday,
		       TM_YEAR_BASE + ttp->tm_year);
	    }
	}
    }
#  endif /* !USE_PAM */
    
    /* nothing else left to fail -- really log in */
    {
	struct utmp utmp;
	
	memset((char *)&utmp, 0, sizeof(utmp));
	time(&utmp.ut_time);
	strncpy(utmp.ut_name, username, sizeof(utmp.ut_name));
	/* ut_name may legally be non-null-terminated */
	if (hostname) {
	    strncpy(utmp.ut_host, hostname, sizeof(utmp.ut_host));
	    utmp.ut_host[sizeof(utmp.ut_host)-1] = 0;
	}
	strncpy(utmp.ut_line, tty, sizeof(utmp.ut_line));
	utmp.ut_line[sizeof(utmp.ut_line)-1] = 0;
	login(&utmp);
    }
#else
    /* for linux, write entries in utmp and wtmp */
    {
	struct utmp ut;
	int wtmp;
	struct utmp *utp;
	time_t t;
	pid_t mypid = getpid();
	
	utmpname(_PATH_UTMP);
	setutent();
	while ((utp = getutent()) 
	       && !(utp->ut_pid == mypid)) /* nothing */;
	
	if (utp) {
	    memcpy(&ut, utp, sizeof(ut));
	} else {
	    /* some gettys/telnetds don't initialize utmp... */
	    memset(&ut, 0, sizeof(ut));
	}
	/* endutent(); superfluous, error for glibc */
	
	if (ut.ut_id[0] == 0)
	  strncpy(ut.ut_id, ttyn + 8, sizeof(ut.ut_id));
	
	strncpy(ut.ut_user, username, sizeof(ut.ut_user));
	strncpy(ut.ut_line, ttyn + 5, sizeof(ut.ut_line));
	ut.ut_line[sizeof(ut.ut_line)-1] = 0;
	time(&t);
	ut.ut_time = t;		/* ut_time is not always a time_t */
				/* (we might test #ifdef _HAVE_UT_TV ) */
	ut.ut_type = USER_PROCESS;
	ut.ut_pid = mypid;
	if (hostname) {
	    strncpy(ut.ut_host, hostname, sizeof(ut.ut_host));
	    ut.ut_host[sizeof(ut.ut_host)-1] = 0;
	    if (hostaddress.h_addr_list)
	      memcpy(&ut.ut_addr, hostaddress.h_addr_list[0],
		     sizeof(ut.ut_addr));
	}
	
	pututline(&ut);
	endutent();
	
	{ 
	    int lf;
	    if ((lf = open(_PATH_WTMPLOCK, O_CREAT|O_WRONLY, 0660)) >= 0) {
		flock(lf, LOCK_EX);
		if ((wtmp = open(_PATH_WTMP, O_APPEND|O_WRONLY)) >= 0) {
		    write(wtmp, (char *)&ut, sizeof(ut));
		    close(wtmp);
		}
		flock(lf, LOCK_UN);
		close(lf);
	    }
	}
    }
#endif
    
    dolastlog(quietlog);
    
#ifndef __linux__
    if (!hflag) {					/* XXX */
	static struct winsize win = { 0, 0, 0, 0 };
	
	ioctl(0, TIOCSWINSZ, &win);
    }
#endif
    chown(ttyn, pwd->pw_uid,
	  (gr = getgrnam(TTYGRPNAME)) ? gr->gr_gid : pwd->pw_gid);
    chmod(ttyn, TTY_MODE);

    /* if tty is one of the VC's then change owner and mode of the 
       special /dev/vcs devices as well */
    if (consoletty(0)) {
	chown(vcsn, pwd->pw_uid, (gr ? gr->gr_gid : pwd->pw_gid));
	chown(vcsan, pwd->pw_uid, (gr ? gr->gr_gid : pwd->pw_gid));
	chmod(vcsn, TTY_MODE);
	chmod(vcsan, TTY_MODE);
    }

    setgid(pwd->pw_gid);
    
#ifdef HAVE_QUOTA
    quota(Q_DOWARN, pwd->pw_uid, (dev_t)-1, 0);
#endif
    
    if (*pwd->pw_shell == '\0')
      pwd->pw_shell = _PATH_BSHELL;
#ifndef __linux__
    /* turn on new line discipline for the csh */
    else if (!strcmp(pwd->pw_shell, _PATH_CSHELL)) {
	ioctlval = NTTYDISC;
	ioctl(0, TIOCSETD, &ioctlval);
    }
#endif
    
    /* preserve TERM even without -p flag */
    {
	char *ep;
	
	if(!((ep = getenv("TERM")) && (termenv = strdup(ep))))
	  termenv = "dumb";
    }
    
    /* destroy environment unless user has requested preservation */
    if (!pflag)
      {
          environ = (char**)malloc(sizeof(char*));
	  memset(environ, 0, sizeof(char*));
      }
    
#ifndef __linux__
    setenv("HOME", pwd->pw_dir, 1);
    setenv("SHELL", pwd->pw_shell, 1);
    if (term[0] == '\0') {
	strncpy(term, stypeof(tty), sizeof(term));
	term[sizeof(term)-1] = 0;
    }
    setenv("TERM", term, 0);
    setenv("USER", pwd->pw_name, 1);
    setenv("PATH", _PATH_DEFPATH, 0);
#else
    setenv("HOME", pwd->pw_dir, 0);      /* legal to override */
    if(pwd->pw_uid)
      setenv("PATH", _PATH_DEFPATH, 1);
    else
      setenv("PATH", _PATH_DEFPATH_ROOT, 1);
    
    setenv("SHELL", pwd->pw_shell, 1);
    setenv("TERM", termenv, 1);
    
    /* mailx will give a funny error msg if you forget this one */
    snprintf(tmp, sizeof(tmp), "%s/%s", _PATH_MAILDIR, pwd->pw_name);
    setenv("MAIL",tmp,0);
    
    /* LOGNAME is not documented in login(1) but
       HP-UX 6.5 does it. We'll not allow modifying it.
       */
    setenv("LOGNAME", pwd->pw_name, 1);
#endif

#ifdef USE_PAM
    {
	int i;
	const char * const * env;

	env = (const char * const *)pam_getenvlist(pamh);

	if (env != NULL) {
	    for (i=0; env[i++]; ) {
		putenv(env[i-1]);
		/* D(("env[%d] = %s", i-1,env[i-1])); */
	    }
	}
    }
#endif
    
    if (tty[sizeof("tty")-1] == 'S')
      syslog(LOG_INFO, "DIALUP AT %s BY %s", tty, pwd->pw_name);
    
    /* allow tracking of good logins.
       -steve philp (sphilp@mail.alliance.net) */
    
    if (pwd->pw_uid == 0) {
	if (hostname)
	  syslog(LOG_NOTICE, "ROOT LOGIN ON %s FROM %s",
		 tty, hostname);
	else
	  syslog(LOG_NOTICE, "ROOT LOGIN ON %s", tty);
    } else {
	if (hostname) 
	  syslog(LOG_INFO, "LOGIN ON %s BY %s FROM %s", tty, 
		 pwd->pw_name, hostname);
	else 
	  syslog(LOG_INFO, "LOGIN ON %s BY %s", tty, 
		 pwd->pw_name);
    }
    
    if (!quietlog) {
	struct stat st;
	
	motd();
	snprintf(tbuf, sizeof(tbuf),
		 "%s/%s", _PATH_MAILDIR, pwd->pw_name);
	if (stat(tbuf, &st) == 0 && st.st_size != 0)
	  printf("You have %smail.\n",
		 (st.st_mtime > st.st_atime) ? "new " : "");
    }
    
    signal(SIGALRM, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGHUP, SIG_DFL);
    
    /* discard permissions last so can't get killed and drop core */
    if(setuid(pwd->pw_uid) < 0 && pwd->pw_uid) {
	syslog(LOG_ALERT, "setuid() failed");
#ifdef USE_PAM
	PAM_END;
#endif
	exit(1);
    }
    
    /* wait until here to change directory! */
    if (chdir(pwd->pw_dir) < 0) {
	printf("No directory %s!\n", pwd->pw_dir);
	if (chdir("/")) {
#ifdef USE_PAM
	  PAM_END;
#endif
	  exit(0);
	}
	pwd->pw_dir = "/";
	printf("Logging in with home = \"/\".\n");
    }
    
    /* if the shell field has a space: treat it like a shell script */
    if (strchr(pwd->pw_shell, ' ')) {
	buff = malloc(strlen(pwd->pw_shell) + 6);

	if (!buff) {
	    fprintf(stderr, "login: no memory for shell script.\n");
	    exit(0);
	}

	strcpy(buff, "exec ");
	strcat(buff, pwd->pw_shell);
	childArgv[childArgc++] = "/bin/sh";
	childArgv[childArgc++] = "-sh";
	childArgv[childArgc++] = "-c";
	childArgv[childArgc++] = buff;
    } else {
	tbuf[0] = '-';
	strncpy(tbuf + 1, ((p = rindex(pwd->pw_shell, '/')) ?
			   p + 1 : pwd->pw_shell),
		sizeof(tbuf)-1);
	tbuf[sizeof(tbuf)-1] = 0;
	
	childArgv[childArgc++] = pwd->pw_shell;
	childArgv[childArgc++] = tbuf;
    }

    childArgv[childArgc++] = NULL;

#ifndef USE_PAM 
    execvp(childArgv[0], childArgv + 1);
    error = 1;
#else /* USE_PAM */
    oldSigHandler = signal(SIGINT, SIG_IGN);
    childPid = fork();
    if (childPid < 0) {
	/* error in fork() */
	fprintf(stderr,"login: failure forking: %s", strerror(errno));
	PAM_END;
	exit(0);
    } else if (childPid) {
	/* parent */
	wait(&childStatus);
	signal(SIGINT, oldSigHandler);
	PAM_END;

	if (!WIFEXITED(&childStatus)) error = 1;
    } else {
	/* child */
	execvp(childArgv[0], childArgv + 1);
	exit(1);
    }
#endif /* USE_PAM */

    if (error) {
	if (!strcmp(childArgv[0], "/bin/sh")) 
	    fprintf(stderr, "login: couldn't exec shell script: %s.\n",
		    strerror(errno));
	else 
	    fprintf(stderr, "login: no shell: %s.\n", strerror(errno));
    }

    exit(0);
}

void
getloginname()
{
    register int ch;
    register char *p;
    static char nbuf[UT_NAMESIZE + 1];
    int cnt, cnt2;
    
    cnt2 = 0;
    for (;;) {
	cnt = 0;
	printf("\n%s login: ", thishost); fflush(stdout);
	for (p = nbuf; (ch = getchar()) != '\n'; ) {
	    if (ch == EOF) {
		badlogin("EOF");
		exit(0);
	    }
	    if (p < nbuf + UT_NAMESIZE)
	      *p++ = ch;
	    
	    cnt++;
	    if (cnt > UT_NAMESIZE + 20) {
		fprintf(stderr, "login name much too long.\n");
		badlogin("NAME too long");
		exit(0);
	    }
	}
	if (p > nbuf)
	  if (nbuf[0] == '-')
	    fprintf(stderr,
		    "login names may not start with '-'.\n");
	  else {
	      *p = '\0';
	      username = nbuf;
	      break;
	  }
	
	cnt2++;
	if (cnt2 > 50) {
	    fprintf(stderr, "too many bare linefeeds.\n");
	    badlogin("EXCESSIVE linefeeds");
	    exit(0);
	}
    }
}

void
timedout()
{
    struct termio ti;
    
    fprintf(stderr, "Login timed out after %d seconds\n", timeout);
    
    /* reset echo */
    ioctl(0, TCGETA, &ti);
    ti.c_lflag |= ECHO;
    ioctl(0, TCSETA, &ti);
    exit(0);
}

int
rootterm(ttyn)
     char *ttyn;
#ifndef __linux__
{
    struct ttyent *t;
    
    return((t = getttynam(ttyn)) && t->ty_status&TTY_SECURE);
}
#else
{ 
    int fd;
    char buf[100],*p;
    int cnt, more;
    
    fd = open(SECURETTY, O_RDONLY);
    if(fd < 0) return 1;
    
    /* read each line in /etc/securetty, if a line matches our ttyline
       then root is allowed to login on this tty, and we should return
       true. */
    for(;;) {
	p = buf; cnt = 100;
	while(--cnt >= 0 && (more = read(fd, p, 1)) == 1 && *p != '\n') p++;
	if(more && *p == '\n') {
	    *p = '\0';
	    if(!strcmp(buf, ttyn)) {
		close(fd);
		return 1;
	    } else
	      continue;
  	} else {
	    close(fd);
	    return 0;
  	}
    }
}
#endif

jmp_buf motdinterrupt;

void
motd()
{
    register int fd, nchars;
    void (*oldint)(), sigint();
    char tbuf[8192];
    
    if ((fd = open(_PATH_MOTDFILE, O_RDONLY, 0)) < 0)
      return;
    oldint = signal(SIGINT, sigint);
    if (setjmp(motdinterrupt) == 0)
      while ((nchars = read(fd, tbuf, sizeof(tbuf))) > 0)
	write(fileno(stdout), tbuf, nchars);
    signal(SIGINT, oldint);
    close(fd);
}

void
sigint()
{
    longjmp(motdinterrupt, 1);
}

#ifndef USE_PAM				/* PAM takes care of this */
void
checknologin()
{
    register int fd, nchars;
    char tbuf[8192];
    
    if ((fd = open(_PATH_NOLOGIN, O_RDONLY, 0)) >= 0) {
	while ((nchars = read(fd, tbuf, sizeof(tbuf))) > 0)
	  write(fileno(stdout), tbuf, nchars);
	sleepexit(0);
    }
}
#endif

void
dolastlog(quiet)
     int quiet;
{
    struct lastlog ll;
    int fd;
    
    if ((fd = open(_PATH_LASTLOG, O_RDWR, 0)) >= 0) {
	lseek(fd, (off_t)pwd->pw_uid * sizeof(ll), L_SET);
	if (!quiet) {
	    if (read(fd, (char *)&ll, sizeof(ll)) == sizeof(ll) &&
		ll.ll_time != 0) {
		printf("Last login: %.*s ",
		       24-5, (char *)ctime(&ll.ll_time));
		
		if (*ll.ll_host != '\0')
		  printf("from %.*s\n",
			 (int)sizeof(ll.ll_host), ll.ll_host);
		else
		  printf("on %.*s\n",
			 (int)sizeof(ll.ll_line), ll.ll_line);
	    }
	    lseek(fd, (off_t)pwd->pw_uid * sizeof(ll), L_SET);
	}
	memset((char *)&ll, 0, sizeof(ll));
	time(&ll.ll_time);
	strncpy(ll.ll_line, tty, sizeof(ll.ll_line));
	ll.ll_line[sizeof(ll.ll_line)-1] = 0;
	if (hostname) {
	    strncpy(ll.ll_host, hostname, sizeof(ll.ll_host));
	    ll.ll_host[sizeof(ll.ll_host)-1] = 0;
	}
	write(fd, (char *)&ll, sizeof(ll));
	close(fd);
    }
}

void
badlogin(const char *name)
{
    if (hostname)
      syslog(LOG_NOTICE, "%d LOGIN FAILURE%s FROM %s, %s",
	     failures, failures > 1 ? "S" : "", hostname, name);
    else
      syslog(LOG_NOTICE, "%d LOGIN FAILURE%s ON %s, %s",
	     failures, failures > 1 ? "S" : "", tty, name);
}

#undef	UNKNOWN
#define	UNKNOWN	"su"

#ifndef __linux__
char *
stypeof(ttyid)
     char *ttyid;
{
    struct ttyent *t;

    return(ttyid && (t = getttynam(ttyid)) ? t->ty_type : UNKNOWN);
}
#endif 

/* should not be called from PAM code... Why? */
void
sleepexit(eval)
     int eval;
{
    sleep(SLEEP_EXIT_TIMEOUT);
    exit(eval);
}
