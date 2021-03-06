/*
 * ntp_control.c - respond to mode 6 control messages.
 *		   Provides service to ntpq and others.
 */

#include "config.h"

#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <openssl/evp.h>	/* provides OpenSSL digest API */

#include "ntpd.h"
#include "ntp_io.h"
#include "ntp_refclock.h"
#include "ntp_control.h"
#include "ntp_calendar.h"
#include "ntp_stdlib.h"
#include "ntp_config.h"
#include "ntp_assert.h"
#include "ntp_leapsec.h"
#include "lib_strbuf.h"
#include "ntp_syscall.h"

/* undefine to suppress random tags and get fixed emission order */
#define USE_RANDOMIZE_RESPONSES

/*
 * Structure to hold request procedure information
 */

struct ctl_proc {
	short control_code;		/* defined request code */
#define NO_REQUEST	(-1)
	u_short flags;			/* flags word */
	/* Only one flag.  Authentication required or not. */
#define NOAUTH	0
#define AUTH	1
	void (*handler) (struct recvbuf *, int); /* handle request */
};


/*
 * Request processing routines
 */
static	void	ctl_error	(uint8_t);
#ifdef REFCLOCK
static	u_short ctlclkstatus	(struct refclockstat *);
#endif
static	void	ctl_flushpkt	(uint8_t);
static	void	ctl_putdata	(const char *, unsigned int, bool);
static	void	ctl_putstr	(const char *, const char *, size_t);
static	void	ctl_putdblf	(const char *, int, int, double);
#define	ctl_putdbl(tag, d)	ctl_putdblf(tag, 1, 3, d)
#define	ctl_putdbl6(tag, d)	ctl_putdblf(tag, 1, 6, d)
#define	ctl_putsfp(tag, sfp)	ctl_putdblf(tag, 0, -1, \
					    FPTOD(sfp))
static	void	ctl_putuint	(const char *, u_long);
static	void	ctl_puthex	(const char *, u_long);
static	void	ctl_putint	(const char *, long);
static	void	ctl_putts	(const char *, l_fp *);
static	void	ctl_putadr	(const char *, uint32_t,
				 sockaddr_u *);
static	void	ctl_putrefid	(const char *, uint32_t);
static	void	ctl_putarray	(const char *, double *, int);
static	void	ctl_putsys	(int);
static	void	ctl_putpeer	(int, struct peer *);
static	void	ctl_putfs	(const char *, tstamp_t);
#ifdef REFCLOCK
static	void	ctl_putclock	(int, struct refclockstat *, int);
#endif	/* REFCLOCK */
static	const struct ctl_var *ctl_getitem(const struct ctl_var *,
					  char **);
static	u_short	count_var	(const struct ctl_var *);
static	void	control_unspec	(struct recvbuf *, int);
static	void	read_status	(struct recvbuf *, int);
static	void	read_sysvars	(void);
static	void	read_peervars	(void);
static	void	read_variables	(struct recvbuf *, int);
static	void	write_variables (struct recvbuf *, int);
static	void	read_clockstatus(struct recvbuf *, int);
static	void	write_clockstatus(struct recvbuf *, int);
static	void	configure	(struct recvbuf *, int);
static	void	send_mru_entry	(mon_entry *, int);
#ifdef USE_RANDOMIZE_RESPONSES
static	void	send_random_tag_value(int);
#endif /* USE_RANDOMIZE_RESPONSES */
static	void	read_mru_list	(struct recvbuf *, int);
static	void	send_ifstats_entry(endpt *, u_int);
static	void	read_ifstats	(struct recvbuf *);
static	void	sockaddrs_from_restrict_u(sockaddr_u *,	sockaddr_u *,
					  restrict_u *, int);
static	void	send_restrict_entry(restrict_u *, int, u_int);
static	void	send_restrict_list(restrict_u *, int, u_int *);
static	void	read_addr_restrictions(struct recvbuf *);
static	void	read_ordlist	(struct recvbuf *, int);
static	uint32_t	derive_nonce	(sockaddr_u *, uint32_t, uint32_t);
static	void	generate_nonce	(struct recvbuf *, char *, size_t);
static	int	validate_nonce	(const char *, struct recvbuf *);
static	void	req_nonce	(struct recvbuf *, int);

static const struct ctl_proc control_codes[] = {
	{ CTL_OP_UNSPEC,		NOAUTH,	control_unspec },
	{ CTL_OP_READSTAT,		NOAUTH,	read_status },
	{ CTL_OP_READVAR,		NOAUTH,	read_variables },
	{ CTL_OP_WRITEVAR,		AUTH,	write_variables },
	{ CTL_OP_READCLOCK,		NOAUTH,	read_clockstatus },
	{ CTL_OP_WRITECLOCK,		NOAUTH,	write_clockstatus },
	{ CTL_OP_CONFIGURE,		AUTH,	configure },
	{ CTL_OP_READ_MRU,		NOAUTH,	read_mru_list },
	{ CTL_OP_READ_ORDLIST_A,	AUTH,	read_ordlist },
	{ CTL_OP_REQ_NONCE,		NOAUTH,	req_nonce },
	{ NO_REQUEST,			0,	NULL }
};

/*
 * System variables we understand
 */
#define	CS_LEAP			1
#define	CS_STRATUM		2
#define	CS_PRECISION		3
#define	CS_ROOTDELAY		4
#define	CS_ROOTDISPERSION	5
#define	CS_REFID		6
#define	CS_REFTIME		7
#define	CS_POLL			8
#define	CS_PEERID		9
#define	CS_OFFSET		10
#define	CS_DRIFT		11
#define	CS_JITTER		12
#define	CS_ERROR		13
#define	CS_CLOCK		14
#define	CS_PROCESSOR		15
#define	CS_SYSTEM		16
#define	CS_VERSION		17
#define	CS_STABIL		18
#define	CS_VARLIST		19
#define	CS_TAI			20
#define	CS_LEAPTAB		21
#define	CS_LEAPEND		22
#define	CS_RATE			23
#define	CS_MRU_ENABLED		24
#define	CS_MRU_DEPTH		25
#define	CS_MRU_DEEPEST		26
#define	CS_MRU_MINDEPTH		27
#define	CS_MRU_MAXAGE		28
#define	CS_MRU_MINAGE		29
#define	CS_MRU_MAXDEPTH		30
#define	CS_MRU_MEM		31
#define	CS_MRU_MAXMEM		32
#define	CS_SS_UPTIME		33
#define	CS_SS_RESET		34
#define	CS_SS_RECEIVED		35
#define	CS_SS_THISVER		36
#define	CS_SS_OLDVER		37
#define	CS_SS_BADFORMAT		38
#define	CS_SS_BADAUTH		39
#define	CS_SS_DECLINED		40
#define	CS_SS_RESTRICTED	41
#define	CS_SS_LIMITED		42
#define	CS_SS_KODSENT		43
#define	CS_SS_PROCESSED		44
#define	CS_PEERADR		45
#define	CS_PEERMODE		46
#define	CS_AUTHDELAY		47
#define	CS_AUTHKEYS		48
#define	CS_AUTHFREEK		49
#define	CS_AUTHKLOOKUPS		50
#define	CS_AUTHKNOTFOUND	51
#define	CS_AUTHKUNCACHED	52
#define	CS_AUTHKEXPIRED		53
#define	CS_AUTHENCRYPTS		54
#define	CS_AUTHDECRYPTS		55
#define	CS_AUTHRESET		56
#define	CS_K_OFFSET		57
#define	CS_K_FREQ		58
#define	CS_K_MAXERR		59
#define	CS_K_ESTERR		60
#define	CS_K_STFLAGS		61
#define	CS_K_TIMECONST		62
#define	CS_K_PRECISION		63
#define	CS_K_FREQTOL		64
#define	CS_K_PPS_FREQ		65
#define	CS_K_PPS_STABIL		66
#define	CS_K_PPS_JITTER		67
#define	CS_K_PPS_CALIBDUR	68
#define	CS_K_PPS_CALIBS		69
#define	CS_K_PPS_CALIBERRS	70
#define	CS_K_PPS_JITEXC		71
#define	CS_K_PPS_STBEXC		72
#define	CS_KERN_FIRST		CS_K_OFFSET
#define	CS_KERN_LAST		CS_K_PPS_STBEXC
#define	CS_IOSTATS_RESET	73
#define	CS_TOTAL_RBUF		74
#define	CS_FREE_RBUF		75
#define	CS_USED_RBUF		76
#define	CS_RBUF_LOWATER		77
#define	CS_IO_DROPPED		78
#define	CS_IO_IGNORED		79
#define	CS_IO_RECEIVED		80
#define	CS_IO_SENT		81
#define	CS_IO_SENDFAILED	82
#define	CS_IO_WAKEUPS		83
#define	CS_IO_GOODWAKEUPS	84
#define	CS_TIMERSTATS_RESET	85
#define	CS_TIMER_OVERRUNS	86
#define	CS_TIMER_XMTS		87
#define	CS_FUZZ			88
#define	CS_WANDER_THRESH	89
#define CS_MRU_EXISTS		90
#define CS_MRU_NEW		91
#define CS_MRU_RECYCLEOLD	92
#define CS_MRU_RECYCLEFULL	93
#define CS_MRU_NONE		94
#define CS_MRU_OLDEST_AGE	95
#define	CS_LEAPSMEARINTV	96
#define	CS_LEAPSMEAROFFS	97
#define	CS_TICK                 98
#define	CS_MAXCODE		CS_TICK

/*
 * Peer variables we understand
 */
#define	CP_CONFIG		1
#define	CP_AUTHENABLE		2
#define	CP_AUTHENTIC		3
#define	CP_SRCADR		4
#define	CP_SRCPORT		5
#define	CP_DSTADR		6
#define	CP_DSTPORT		7
#define	CP_LEAP			8
#define	CP_HMODE		9
#define	CP_STRATUM		10
#define	CP_PPOLL		11
#define	CP_HPOLL		12
#define	CP_PRECISION		13
#define	CP_ROOTDELAY		14
#define	CP_ROOTDISPERSION	15
#define	CP_REFID		16
#define	CP_REFTIME		17
#define	CP_ORG			18
#define	CP_REC			19
#define	CP_XMT			20
#define	CP_REACH		21
#define	CP_UNREACH		22
#define	CP_TIMER		23
#define	CP_DELAY		24
#define	CP_OFFSET		25
#define	CP_JITTER		26
#define	CP_DISPERSION		27
#define	CP_KEYID		28
#define	CP_FILTDELAY		29
#define	CP_FILTOFFSET		30
#define	CP_PMODE		31
#define	CP_RECEIVED		32
#define	CP_SENT			33
#define	CP_FILTERROR		34
#define	CP_FLASH		35
#define	CP_TTL			36
#define	CP_VARLIST		37
#define	CP_IN			38
#define	CP_OUT			39
#define	CP_RATE			40
#define	CP_BIAS			41
#define	CP_SRCHOST		42
#define	CP_TIMEREC		43
#define	CP_TIMEREACH		44
#define	CP_BADAUTH		45
#define	CP_BOGUSORG		46
#define	CP_OLDPKT		47
#define	CP_SELDISP		48
#define	CP_SELBROKEN		49
#define	CP_CANDIDATE		50
#define	CP_MAXCODE		CP_CANDIDATE

/*
 * Clock variables we understand
 */
#define	CC_NAME		1
#define	CC_TIMECODE	2
#define	CC_POLL		3
#define	CC_NOREPLY	4
#define	CC_BADFORMAT	5
#define	CC_BADDATA	6
#define	CC_FUDGETIME1	7
#define	CC_FUDGETIME2	8
#define	CC_FUDGEVAL1	9
#define	CC_FUDGEVAL2	10
#define	CC_FLAGS	11
#define	CC_DEVICE	12
#define	CC_VARLIST	13
#define	CC_MAXCODE	CC_VARLIST

/*
 * System variable values. The array can be indexed by the variable
 * index to find the textual name.
 */
static const struct ctl_var sys_var[] = {
	{ 0,		PADDING, "" },		/* 0 */
	{ CS_LEAP,	RW, "leap" },		/* 1 */
	{ CS_STRATUM,	RO, "stratum" },	/* 2 */
	{ CS_PRECISION, RO, "precision" },	/* 3 */
	{ CS_ROOTDELAY, RO, "rootdelay" },	/* 4 */
	{ CS_ROOTDISPERSION, RO, "rootdisp" },	/* 5 */
	{ CS_REFID,	RO, "refid" },		/* 6 */
	{ CS_REFTIME,	RO, "reftime" },	/* 7 */
	{ CS_POLL,	RO, "tc" },		/* 8 */
	{ CS_PEERID,	RO, "peer" },		/* 9 */
	{ CS_OFFSET,	RO, "offset" },		/* 10 */
	{ CS_DRIFT,	RO, "frequency" },	/* 11 */
	{ CS_JITTER,	RO, "sys_jitter" },	/* 12 */
	{ CS_ERROR,	RO, "clk_jitter" },	/* 13 */
	{ CS_CLOCK,	RO, "clock" },		/* 14 */
	{ CS_PROCESSOR, RO, "processor" },	/* 15 */
	{ CS_SYSTEM,	RO, "system" },		/* 16 */
	{ CS_VERSION,	RO, "version" },	/* 17 */
	{ CS_STABIL,	RO, "clk_wander" },	/* 18 */
	{ CS_VARLIST,	RO, "sys_var_list" },	/* 19 */
	{ CS_TAI,	RO, "tai" },		/* 20 */
	{ CS_LEAPTAB,	RO, "leapsec" },	/* 21 */
	{ CS_LEAPEND,	RO, "expire" },		/* 22 */
	{ CS_RATE,	RO, "mintc" },		/* 23 */
	{ CS_MRU_ENABLED,	RO, "mru_enabled" },	/* 24 */
	{ CS_MRU_DEPTH,		RO, "mru_depth" },	/* 25 */
	{ CS_MRU_DEEPEST,	RO, "mru_deepest" },	/* 26 */
	{ CS_MRU_MINDEPTH,	RO, "mru_mindepth" },	/* 27 */
	{ CS_MRU_MAXAGE,	RO, "mru_maxage" },	/* 28 */
	{ CS_MRU_MINAGE,	RO, "mru_minage" },     /* 29 */
	{ CS_MRU_MAXDEPTH,	RO, "mru_maxdepth" },	/* 30 */
	{ CS_MRU_MEM,		RO, "mru_mem" },	/* 31 */
	{ CS_MRU_MAXMEM,	RO, "mru_maxmem" },	/* 32 */
	{ CS_SS_UPTIME,		RO, "ss_uptime" },	/* 33 */
	{ CS_SS_RESET,		RO, "ss_reset" },	/* 34 */
	{ CS_SS_RECEIVED,	RO, "ss_received" },	/* 35 */
	{ CS_SS_THISVER,	RO, "ss_thisver" },	/* 36 */
	{ CS_SS_OLDVER,		RO, "ss_oldver" },	/* 37 */
	{ CS_SS_BADFORMAT,	RO, "ss_badformat" },	/* 38 */
	{ CS_SS_BADAUTH,	RO, "ss_badauth" },	/* 39 */
	{ CS_SS_DECLINED,	RO, "ss_declined" },	/* 40 */
	{ CS_SS_RESTRICTED,	RO, "ss_restricted" },	/* 41 */
	{ CS_SS_LIMITED,	RO, "ss_limited" },	/* 42 */
	{ CS_SS_KODSENT,	RO, "ss_kodsent" },	/* 43 */
	{ CS_SS_PROCESSED,	RO, "ss_processed" },	/* 44 */
	{ CS_PEERADR,		RO, "peeradr" },	/* 45 */
	{ CS_PEERMODE,		RO, "peermode" },	/* 46 */
	{ CS_AUTHDELAY,		RO, "authdelay" },	/* 47 */
	{ CS_AUTHKEYS,		RO, "authkeys" },	/* 48 */
	{ CS_AUTHFREEK,		RO, "authfreek" },	/* 49 */
	{ CS_AUTHKLOOKUPS,	RO, "authklookups" },	/* 50 */
	{ CS_AUTHKNOTFOUND,	RO, "authknotfound" },	/* 51 */
	{ CS_AUTHKUNCACHED,	RO, "authkuncached" },	/* 52 */
	{ CS_AUTHKEXPIRED,	RO, "authkexpired" },	/* 53 */
	{ CS_AUTHENCRYPTS,	RO, "authencrypts" },	/* 54 */
	{ CS_AUTHDECRYPTS,	RO, "authdecrypts" },	/* 55 */
	{ CS_AUTHRESET,		RO, "authreset" },	/* 56 */
	{ CS_K_OFFSET,		RO, "koffset" },	/* 57 */
	{ CS_K_FREQ,		RO, "kfreq" },		/* 58 */
	{ CS_K_MAXERR,		RO, "kmaxerr" },	/* 59 */
	{ CS_K_ESTERR,		RO, "kesterr" },	/* 60 */
	{ CS_K_STFLAGS,		RO, "kstflags" },	/* 61 */
	{ CS_K_TIMECONST,	RO, "ktimeconst" },	/* 62 */
	{ CS_K_PRECISION,	RO, "kprecis" },	/* 63 */
	{ CS_K_FREQTOL,		RO, "kfreqtol" },	/* 64 */
	{ CS_K_PPS_FREQ,	RO, "kppsfreq" },	/* 65 */
	{ CS_K_PPS_STABIL,	RO, "kppsstab" },	/* 66 */
	{ CS_K_PPS_JITTER,	RO, "kppsjitter" },	/* 67 */
	{ CS_K_PPS_CALIBDUR,	RO, "kppscalibdur" },	/* 68 */
	{ CS_K_PPS_CALIBS,	RO, "kppscalibs" },	/* 69 */
	{ CS_K_PPS_CALIBERRS,	RO, "kppscaliberrs" },	/* 70 */
	{ CS_K_PPS_JITEXC,	RO, "kppsjitexc" },	/* 71 */
	{ CS_K_PPS_STBEXC,	RO, "kppsstbexc" },	/* 72 */
	{ CS_IOSTATS_RESET,	RO, "iostats_reset" },	/* 73 */
	{ CS_TOTAL_RBUF,	RO, "total_rbuf" },	/* 74 */
	{ CS_FREE_RBUF,		RO, "free_rbuf" },	/* 75 */
	{ CS_USED_RBUF,		RO, "used_rbuf" },	/* 76 */
	{ CS_RBUF_LOWATER,	RO, "rbuf_lowater" },	/* 77 */
	{ CS_IO_DROPPED,	RO, "io_dropped" },	/* 78 */
	{ CS_IO_IGNORED,	RO, "io_ignored" },	/* 79 */
	{ CS_IO_RECEIVED,	RO, "io_received" },	/* 80 */
	{ CS_IO_SENT,		RO, "io_sent" },	/* 81 */
	{ CS_IO_SENDFAILED,	RO, "io_sendfailed" },	/* 82 */
	{ CS_IO_WAKEUPS,	RO, "io_wakeups" },	/* 83 */
	{ CS_IO_GOODWAKEUPS,	RO, "io_goodwakeups" },	/* 84 */
	{ CS_TIMERSTATS_RESET,	RO, "timerstats_reset" },/* 85 */
	{ CS_TIMER_OVERRUNS,	RO, "timer_overruns" },	/* 86 */
	{ CS_TIMER_XMTS,	RO, "timer_xmts" },	/* 87 */
	{ CS_FUZZ,		RO, "fuzz" },		/* 88 */
	{ CS_WANDER_THRESH,	RO, "clk_wander_threshold" }, /* 89 */
	{ CS_MRU_EXISTS,	RO, "mru_exists" },	/* 90 */
	{ CS_MRU_NEW,		RO, "mru_new" },	/* 91 */
	{ CS_MRU_RECYCLEOLD,	RO, "mru_recycleold" },	/* 92 */
	{ CS_MRU_RECYCLEFULL,	RO, "mru_recyclefull" },/* 93 */
	{ CS_MRU_NONE,		RO, "mru_none" },	/* 94 */
	{ CS_MRU_OLDEST_AGE,	RO, "mru_oldest_age" },	/* 95 */
	{ CS_LEAPSMEARINTV,	RO, "leapsmearinterval" },    /* 96 */
	{ CS_LEAPSMEAROFFS,	RO, "leapsmearoffset" },      /* 97 */
	{ CS_TICK,		RO, "tick" },		/* 98 */
	{ 0,                    EOV, "" }		/* 98 */
};

static struct ctl_var *ext_sys_var = NULL;

/*
 * System variables we print by default (in fuzzball order,
 * more-or-less)
 */
static const uint8_t def_sys_var[] = {
	CS_VERSION,
	CS_PROCESSOR,
	CS_SYSTEM,
	CS_LEAP,
	CS_STRATUM,
	CS_PRECISION,
	CS_ROOTDELAY,
	CS_ROOTDISPERSION,
	CS_REFID,
	CS_REFTIME,
	CS_CLOCK,
	CS_PEERID,
	CS_POLL,
	CS_RATE,
	CS_OFFSET,
	CS_DRIFT,
	CS_JITTER,
	CS_ERROR,
	CS_STABIL,
	CS_TAI,
	CS_LEAPTAB,
	CS_LEAPEND,
	0
};


/*
 * Peer variable list
 */
static const struct ctl_var peer_var[] = {
	{ 0,		PADDING, "" },		/* 0 */
	{ CP_CONFIG,	RO, "config" },		/* 1 */
	{ CP_AUTHENABLE, RO,	"authenable" },	/* 2 */
	{ CP_AUTHENTIC, RO, "authentic" },	/* 3 */
	{ CP_SRCADR,	RO, "srcadr" },		/* 4 */
	{ CP_SRCPORT,	RO, "srcport" },	/* 5 */
	{ CP_DSTADR,	RO, "dstadr" },		/* 6 */
	{ CP_DSTPORT,	RO, "dstport" },	/* 7 */
	{ CP_LEAP,	RO, "leap" },		/* 8 */
	{ CP_HMODE,	RO, "hmode" },		/* 9 */
	{ CP_STRATUM,	RO, "stratum" },	/* 10 */
	{ CP_PPOLL,	RO, "ppoll" },		/* 11 */
	{ CP_HPOLL,	RO, "hpoll" },		/* 12 */
	{ CP_PRECISION,	RO, "precision" },	/* 13 */
	{ CP_ROOTDELAY,	RO, "rootdelay" },	/* 14 */
	{ CP_ROOTDISPERSION, RO, "rootdisp" },	/* 15 */
	{ CP_REFID,	RO, "refid" },		/* 16 */
	{ CP_REFTIME,	RO, "reftime" },	/* 17 */
        /* Placeholder. Reporting of this variable is disabled because
           leaking it creates a vulnerability */
        { CP_ORG,	RO, "org" },	        /* 18 */
	{ CP_REC,	RO, "rec" },		/* 19 */
	{ CP_XMT,	RO, "xmt" },		/* 20 */
	{ CP_REACH,	RO, "reach" },		/* 21 */
	{ CP_UNREACH,	RO, "unreach" },	/* 22 */
	{ CP_TIMER,	RO, "timer" },		/* 23 */
	{ CP_DELAY,	RO, "delay" },		/* 24 */
	{ CP_OFFSET,	RO, "offset" },		/* 25 */
	{ CP_JITTER,	RO, "jitter" },		/* 26 */
	{ CP_DISPERSION, RO, "dispersion" },	/* 27 */
	{ CP_KEYID,	RO, "keyid" },		/* 28 */
	{ CP_FILTDELAY,	RO, "filtdelay" },	/* 29 */
	{ CP_FILTOFFSET, RO, "filtoffset" },	/* 30 */
	{ CP_PMODE,	RO, "pmode" },		/* 31 */
	{ CP_RECEIVED,	RO, "received"},	/* 32 */
	{ CP_SENT,	RO, "sent" },		/* 33 */
	{ CP_FILTERROR,	RO, "filtdisp" },	/* 34 */
	{ CP_FLASH,	RO, "flash" },		/* 35 */
	{ CP_TTL,	RO, "ttl" },		/* 36 */
	{ CP_VARLIST,	RO, "peer_var_list" },	/* 37 */
	{ CP_IN,	RO, "in" },		/* 38 */
	{ CP_OUT,	RO, "out" },		/* 39 */
	{ CP_RATE,	RO, "headway" },	/* 40 */
	{ CP_BIAS,	RO, "bias" },		/* 41 */
	{ CP_SRCHOST,	RO, "srchost" },	/* 42 */
	{ CP_TIMEREC,	RO, "timerec" },	/* 43 */
	{ CP_TIMEREACH,	RO, "timereach" },	/* 44 */
	{ CP_BADAUTH,	RO, "badauth" },	/* 45 */
	{ CP_BOGUSORG,	RO, "bogusorg" },	/* 46 */
	{ CP_OLDPKT,	RO, "oldpkt" },		/* 47 */
	{ CP_SELDISP,	RO, "seldisp" },	/* 48 */
	{ CP_SELBROKEN,	RO, "selbroken" },	/* 49 */
	{ CP_CANDIDATE, RO, "candidate" },	/* 50 */
	{ 0,		EOV, "" }		/* 50/58 */
};


/*
 * Peer variables we print by default
 */
static const uint8_t def_peer_var[] = {
	CP_SRCADR,
	CP_SRCPORT,
	CP_SRCHOST,
	CP_DSTADR,
	CP_DSTPORT,
	CP_OUT,
	CP_IN,
	CP_LEAP,
	CP_STRATUM,
	CP_PRECISION,
	CP_ROOTDELAY,
	CP_ROOTDISPERSION,
	CP_REFID,
	CP_REFTIME,
	CP_REC,
	CP_REACH,
	CP_UNREACH,
	CP_HMODE,
	CP_PMODE,
	CP_HPOLL,
	CP_PPOLL,
	CP_RATE,
	CP_FLASH,
	CP_KEYID,
	CP_TTL,
	CP_OFFSET,
	CP_DELAY,
	CP_DISPERSION,
	CP_JITTER,
	CP_XMT,
	CP_BIAS,
	CP_FILTDELAY,
	CP_FILTOFFSET,
	CP_FILTERROR,
	0
};


#ifdef REFCLOCK
/*
 * Clock variable list
 */
static const struct ctl_var clock_var[] = {
	{ 0,		PADDING, "" },		/* 0 */
	{ CC_NAME,	RO, "name" },		/* 1 */
	{ CC_TIMECODE,	RO, "timecode" },	/* 2 */
	{ CC_POLL,	RO, "poll" },		/* 3 */
	{ CC_NOREPLY,	RO, "noreply" },	/* 4 */
	{ CC_BADFORMAT, RO, "badformat" },	/* 5 */
	{ CC_BADDATA,	RO, "baddata" },	/* 6 */
	{ CC_FUDGETIME1, RO, "fudgetime1" },	/* 7 */
	{ CC_FUDGETIME2, RO, "fudgetime2" },	/* 8 */
	{ CC_FUDGEVAL1, RO, "stratum" },	/* 9 */
	{ CC_FUDGEVAL2, RO, "refid" },		/* 10 */
	{ CC_FLAGS,	RO, "flags" },		/* 11 */
	{ CC_DEVICE,	RO, "device" },		/* 12 */
	{ CC_VARLIST,	RO, "clock_var_list" },	/* 13 */
	{ 0,		EOV, ""  }		/* 14 */
};


/*
 * Clock variables printed by default
 */
static const uint8_t def_clock_var[] = {
	CC_DEVICE,
	CC_NAME,
	CC_TIMECODE,
	CC_POLL,
	CC_NOREPLY,
	CC_BADFORMAT,
	CC_BADDATA,
	CC_FUDGETIME1,
	CC_FUDGETIME2,
	CC_FUDGEVAL1,
	CC_FUDGEVAL2,
	CC_FLAGS,
	0
};
#endif

/*
 * MRU string constants shared by send_mru_entry() and read_mru_list().
 */
static const char addr_fmt[] =		"addr.%d";
static const char last_fmt[] =		"last.%d";

/*
 * System and processor definitions.
 */
# include <sys/utsname.h>
static struct utsname utsnamebuf;

/*
 * Keyid used for authenticating write requests.
 */
keyid_t ctl_auth_keyid;

/*
 * We keep track of the last error reported by the system internally
 */
static	uint8_t ctl_sys_last_event;
static	uint8_t ctl_sys_num_events;


/*
 * Statistic counters to keep track of requests and responses.
 */
u_long ctltimereset;		/* time stats reset */
u_long numctlreq;		/* number of requests we've received */
u_long numctlbadpkts;		/* number of bad control packets */
u_long numctlresponses;		/* number of resp packets sent with data */
u_long numctlfrags;		/* number of fragments sent */
u_long numctlerrors;		/* number of error responses sent */
u_long numctltooshort;		/* number of too short input packets */
u_long numctlinputresp;		/* number of responses on input */
u_long numctlinputfrag;		/* number of fragments on input */
u_long numctlinputerr;		/* number of input pkts with err bit set */
u_long numctlbadoffset;		/* number of input pkts with nonzero offset */
u_long numctlbadversion;	/* number of input pkts with unknown version */
u_long numctldatatooshort;	/* data too short for count */
u_long numctlbadop;		/* bad op code found in packet */
u_long numasyncmsgs;		/* number of async messages we've sent */

/*
 * Response packet used by these routines. Also some state information
 * so that we can handle packet formatting within a common set of
 * subroutines.  Note we try to enter data in place whenever possible,
 * but the need to set the more bit correctly means we occasionally
 * use the extra buffer and copy.
 */
static struct ntp_control rpkt;
static uint8_t	res_version;
static uint8_t	res_opcode;
static associd_t res_associd;
static u_short	res_frags;	/* datagrams in this response */
static int	res_offset;	/* offset of payload in response */
static uint8_t * datapt;
static int	datalinelen;
static bool	datasent;	/* flag to avoid initial ", " */
static bool	datanotbinflag;
static sockaddr_u *rmt_addr;
static endpt *lcl_inter;

static bool	res_authenticate;
static bool	res_authokay;
static keyid_t	res_keyid;

#define MAXDATALINELEN	(72)

/*
 * Pointers for saving state when decoding request packets
 */
static	char *reqpt;
static	char *reqend;

#ifndef MIN
#define MIN(a, b) (((a) <= (b)) ? (a) : (b))
#endif

#define MILLISECONDS	1000	/* milliseconds per second -magic numbers suck */

/*
 * init_control - initialize request data
 */
void
init_control(void)
{
	uname(&utsnamebuf);

	ctl_clr_stats();

	ctl_auth_keyid = 0;
	/* these may be unused with the old trap facility gone */
	ctl_sys_last_event = EVNT_UNSPEC;
	ctl_sys_num_events = 0;

#ifdef ENABLE_CLASSIC_MODE
	/* a relic from when there were multiple nonstandard ways to set time */
#define PRESET	"settimeofday=\"clock_settime\""
	set_sys_var(PRESET, sizeof(PRESET), RO);
#undef PRESET
#endif /* ENABLE_CLASSIC_MODE */

}


/*
 * ctl_error - send an error response for the current request
 */
static void
ctl_error(
	uint8_t errcode
	)
{
	int		maclen;

	numctlerrors++;
	DPRINTF(3, ("sending control error %u\n", errcode));

	/*
	 * Fill in the fields. We assume rpkt.sequence and rpkt.associd
	 * have already been filled in.
	 */
	rpkt.r_m_e_op = (uint8_t)CTL_RESPONSE | CTL_ERROR |
			(res_opcode & CTL_OP_MASK);
	rpkt.status = htons((u_short)(errcode << 8) & 0xff00);
	rpkt.count = 0;

	/*
	 * send packet and bump counters
	 */
	if (res_authenticate) {
		maclen = authencrypt(res_keyid, (uint32_t *)&rpkt,
				     CTL_HEADER_LEN);
		sendpkt(rmt_addr, lcl_inter, &rpkt, CTL_HEADER_LEN + maclen);
	} else
		sendpkt(rmt_addr, lcl_inter, &rpkt, CTL_HEADER_LEN);
}

/*
 * process_control - process an incoming control message
 */
void
process_control(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
	struct ntp_control *pkt;
	int req_count;
	int req_data;
	const struct ctl_proc *cc;
	keyid_t *pkid;
	int properlen;
	size_t maclen;

	DPRINTF(3, ("in process_control()\n"));

	/*
	 * Save the addresses for error responses
	 */
	numctlreq++;
	rmt_addr = &rbufp->recv_srcadr;
	lcl_inter = rbufp->dstadr;
	pkt = (struct ntp_control *)&rbufp->recv_pkt;

	/*
	 * If the length is less than required for the header, or
	 * it is a response or a fragment, ignore this.
	 */
	if (rbufp->recv_length < (int)CTL_HEADER_LEN
	    || (CTL_RESPONSE | CTL_MORE | CTL_ERROR) & pkt->r_m_e_op
	    || pkt->offset != 0) {
		DPRINTF(1, ("invalid format in control packet\n"));
		if (rbufp->recv_length < (int)CTL_HEADER_LEN)
			numctltooshort++;
		if (CTL_RESPONSE & pkt->r_m_e_op)
			numctlinputresp++;
		if (CTL_MORE & pkt->r_m_e_op)
			numctlinputfrag++;
		if (CTL_ERROR & pkt->r_m_e_op)
			numctlinputerr++;
		if (pkt->offset != 0)
			numctlbadoffset++;
		return;
	}
	res_version = PKT_VERSION(pkt->li_vn_mode);
	if (res_version > NTP_VERSION || res_version < NTP_OLDVERSION) {
		DPRINTF(1, ("unknown version %d in control packet\n",
			    res_version));
		numctlbadversion++;
		return;
	}

	/*
	 * Pull enough data from the packet to make intelligent
	 * responses
	 */
	rpkt.li_vn_mode = PKT_LI_VN_MODE(sys_leap, res_version,
					 MODE_CONTROL);
	res_opcode = pkt->r_m_e_op;
	rpkt.sequence = pkt->sequence;
	rpkt.associd = pkt->associd;
	rpkt.status = 0;
	res_frags = 1;
	res_offset = 0;
	res_associd = ntohs(pkt->associd);
	res_authenticate = false;
	res_keyid = 0;
	res_authokay = false;
	req_count = (int)ntohs(pkt->count);
	datanotbinflag = false;
	datalinelen = 0;
	datasent = false;
	datapt = rpkt.data;

	if ((rbufp->recv_length & 0x3) != 0)
		DPRINTF(3, ("Control packet length %zd unrounded\n",
			    rbufp->recv_length));

	/*
	 * We're set up now. Make sure we've got at least enough
	 * incoming data space to match the count.
	 */
	req_data = rbufp->recv_length - CTL_HEADER_LEN;
	if (req_data < req_count || rbufp->recv_length & 0x3) {
		ctl_error(CERR_BADFMT);
		numctldatatooshort++;
		return;
	}

	properlen = req_count + CTL_HEADER_LEN;
	/* round up proper len to a 8 octet boundary */

	properlen = (properlen + 7) & ~7;
	maclen = rbufp->recv_length - properlen;
	if ((rbufp->recv_length & 3) == 0 &&
	    maclen >= MIN_MAC_LEN && maclen <= MAX_MAC_LEN) {
		res_authenticate = true;
		pkid = (void *)((char *)pkt + properlen);
		res_keyid = ntohl(*pkid);
		DPRINTF(3, ("recv_len %zd, properlen %d, wants auth with keyid %08x, MAC length=%zu\n",
			    rbufp->recv_length, properlen, res_keyid,
			    maclen));

		if (!authistrusted(res_keyid))
			DPRINTF(3, ("invalid keyid %08x\n", res_keyid));
		else if (authdecrypt(res_keyid, (uint32_t *)pkt,
				     rbufp->recv_length - maclen,
				     maclen)) {
			res_authokay = true;
			DPRINTF(3, ("authenticated okay\n"));
		} else {
			res_keyid = 0;
			DPRINTF(3, ("authentication failed\n"));
		}
	}

	/*
	 * Set up translate pointers
	 */
	reqpt = (char *)pkt->data;
	reqend = reqpt + req_count;

	/*
	 * Look for the opcode processor
	 */
	for (cc = control_codes; cc->control_code != NO_REQUEST; cc++) {
		if (cc->control_code == res_opcode) {
			DPRINTF(3, ("opcode %d, found command handler\n",
				    res_opcode));
			if (cc->flags == AUTH
			    && (!res_authokay
				|| res_keyid != ctl_auth_keyid)) {
				ctl_error(CERR_PERMISSION);
				return;
			}
			(cc->handler)(rbufp, restrict_mask);
			return;
		}
	}

	/*
	 * Can't find this one, return an error.
	 */
	numctlbadop++;
	ctl_error(CERR_BADOP);
	return;
}


/*
 * ctlpeerstatus - return a status word for this peer
 */
u_short
ctlpeerstatus(
	register struct peer *p
	)
{
	u_short status;

	status = p->status;
	if (FLAG_CONFIG & p->flags)
		status |= CTL_PST_CONFIG;
	if (p->keyid)
		status |= CTL_PST_AUTHENABLE;
	if (FLAG_AUTHENTIC & p->flags)
		status |= CTL_PST_AUTHENTIC;
	if (p->reach)
		status |= CTL_PST_REACH;
	if (MDF_TXONLY_MASK & p->cast_flags)
		status |= CTL_PST_BCAST;

	return CTL_PEER_STATUS(status, p->num_events, p->last_event);
}


/*
 * ctlclkstatus - return a status word for this clock
 */
#ifdef REFCLOCK
static u_short
ctlclkstatus(
	struct refclockstat *pcs
	)
{
	return CTL_PEER_STATUS(0, pcs->lastevent, pcs->currentstatus);
}
#endif


/*
 * ctlsysstatus - return the system status word
 */
u_short
ctlsysstatus(void)
{
	register uint8_t this_clock;

	this_clock = CTL_SST_TS_UNSPEC;
#ifdef REFCLOCK
	if (sys_peer != NULL) {
		if (CTL_SST_TS_UNSPEC != sys_peer->sstclktype)
			this_clock = sys_peer->sstclktype;
	}
#else /* REFCLOCK */
	if (sys_peer != 0)
		this_clock = CTL_SST_TS_NTP;
#endif /* REFCLOCK */
	return CTL_SYS_STATUS(sys_leap, this_clock, ctl_sys_num_events,
			      ctl_sys_last_event);
}


/*
 * ctl_flushpkt - write out the current packet and prepare
 *		  another if necessary.
 */
static void
ctl_flushpkt(
	uint8_t more
	)
{
	int dlen;
	int sendlen;
	int maclen;
	int totlen;
	keyid_t keyid;

	dlen = datapt - rpkt.data;
	if (!more && datanotbinflag && dlen + 2 < CTL_MAX_DATA_LEN) {
		/*
		 * Big hack, output a trailing \r\n
		 */
		*datapt++ = '\r';
		*datapt++ = '\n';
		dlen += 2;
	}
	sendlen = dlen + CTL_HEADER_LEN;

	/*
	 * Zero-fill the unused part of the packet.  This wasn't needed
	 * when the clients were all in C, for which the first NUL is
	 * a string terminator.  But Python allows NULs in strings, 
	 * which means Python mode 6 clients might actually see the trailing
	 * garbage.
	 */
	memset(rpkt.data + sendlen, '\0', sizeof(rpkt.data) - sendlen);
	
	/*
	 * Pad to a multiple of 32 bits
	 */
	while (sendlen & 0x3) {
		sendlen++;
	}

	/*
	 * Fill in the packet with the current info
	 */
	rpkt.r_m_e_op = CTL_RESPONSE | more |
			(res_opcode & CTL_OP_MASK);
	rpkt.count = htons((u_short)dlen);
	rpkt.offset = htons((u_short)res_offset);
	if (res_authenticate) {
		totlen = sendlen;
		/*
		 * If we are going to authenticate, then there
		 * is an additional requirement that the MAC
		 * begin on a 64 bit boundary.
		 */
		while (totlen & 7) {
			totlen++;
		}
		keyid = htonl(res_keyid);
		memcpy(datapt, &keyid, sizeof(keyid));
		maclen = authencrypt(res_keyid,
				     (uint32_t *)&rpkt, totlen);
		sendpkt(rmt_addr, lcl_inter, &rpkt, totlen + maclen);
	} else {
		sendpkt(rmt_addr, lcl_inter, &rpkt, sendlen);
	}
	if (more)
		numctlfrags++;
	else
		numctlresponses++;

	/*
	 * Set us up for another go around.
	 */
	res_frags++;
	res_offset += dlen;
	datapt = rpkt.data;
}


/*
 * ctl_putdata - write data into the packet, fragmenting and starting
 * another if this one is full.
 */
static void
ctl_putdata(
	const char *dp,
	unsigned int dlen,
	bool bin		/* set to true when data is binary */
	)
{
	int overhead;
	unsigned int currentlen;
	const uint8_t * dataend = &rpkt.data[CTL_MAX_DATA_LEN];

	overhead = 0;
	if (!bin) {
		datanotbinflag = true;
		overhead = 3;
		if (datasent) {
			*datapt++ = ',';
			datalinelen++;
			if ((dlen + datalinelen + 1) >= MAXDATALINELEN) {
				*datapt++ = '\r';
				*datapt++ = '\n';
				datalinelen = 0;
			} else {
				*datapt++ = ' ';
				datalinelen++;
			}
		}
	}

	/*
	 * Save room for trailing junk
	 */
	while (dlen + overhead + datapt > dataend) {
		/*
		 * Not enough room in this one, flush it out.
		 */
		currentlen = MIN(dlen, (unsigned int)(dataend - datapt));

		memcpy(datapt, dp, currentlen);

		datapt += currentlen;
		dp += currentlen;
		dlen -= currentlen;
		datalinelen += currentlen;

		ctl_flushpkt(CTL_MORE);
	}

	memcpy(datapt, dp, dlen);
	datapt += dlen;
	datalinelen += dlen;
	datasent = true;
}


/*
 * ctl_putstr - write a tagged string into the response packet
 *		in the form:
 *
 *		tag="data"
 *
 *		len is the data length excluding the NUL terminator,
 *		as in ctl_putstr("var", "value", strlen("value"));
 *		The write will be truncated if data contains  a NUL,
 *		so don't do that.
 *
 * ESR, 2016: Whoever wrote this should be *hurt*.  If the string value is 
 * empty, no "=" and no value literal is written, just the bare tag.  
 */
static void
ctl_putstr(
	const char *	tag,
	const char *	data,
	size_t		len
	)
{
	char buffer[512];
	size_t tl = strlen(tag);

	if (tl >= sizeof(buffer))
	    return;
	memcpy(buffer, tag, tl);
	if (len > 0)
	    snprintf(buffer + tl, sizeof(buffer) - tl, "=\"%s\"", data);
	ctl_putdata(buffer, (u_int)strlen(buffer), false);
}


/*
 * ctl_putunqstr - write a tagged string into the response packet
 *		   in the form:
 *
 *		   tag=data
 *
 *	len is the data length excluding the NUL terminator.
 *	data must not contain a comma or whitespace.
 */
static void
ctl_putunqstr(
	const char *	tag,
	const char *	data,
	size_t		len
	)
{
	char buffer[512];
	char *cp;
	size_t tl;

	tl = strlen(tag);
	memcpy(buffer, tag, tl);
	cp = buffer + tl;
	if (len > 0) {
		NTP_INSIST(tl + 1 + len <= sizeof(buffer));
		*cp++ = '=';
		memcpy(cp, data, len);
		cp += len;
	}
	ctl_putdata(buffer, (u_int)(cp - buffer), false);
}


/*
 * ctl_putdblf - write a tagged, signed double into the response packet
 */
static void
ctl_putdblf(
	const char *	tag,
	int		use_f,
	int		precision,
	double		d
	)
{
	char *cp;
	const char *cq;
	char buffer[200];

	cp = buffer;
	cq = tag;
	while (*cq != '\0')
		*cp++ = *cq++;
	*cp++ = '=';
	NTP_INSIST((size_t)(cp - buffer) < sizeof(buffer));
	snprintf(cp, sizeof(buffer) - (cp - buffer), use_f ? "%.*f" : "%.*g",
	    precision, d);
	cp += strlen(cp);
	ctl_putdata(buffer, (unsigned)(cp - buffer), false);
}

/*
 * ctl_putuint - write a tagged unsigned integer into the response
 */
static void
ctl_putuint(
	const char *tag,
	u_long uval
	)
{
	register char *cp;
	register const char *cq;
	char buffer[200];

	cp = buffer;
	cq = tag;
	while (*cq != '\0')
		*cp++ = *cq++;

	*cp++ = '=';
	NTP_INSIST((cp - buffer) < (int)sizeof(buffer));
	snprintf(cp, sizeof(buffer) - (cp - buffer), "%lu", uval);
	cp += strlen(cp);
	ctl_putdata(buffer, (unsigned)( cp - buffer ), false);
}

/*
 * ctl_putfs - write a decoded filestamp into the response
 */
static void
ctl_putfs(
	const char *tag,
	tstamp_t uval
	)
{
	register char *cp;
	register const char *cq;
	char buffer[200];
	struct tm tmbuf, *tm = NULL;
	time_t fstamp;

	cp = buffer;
	cq = tag;
	while (*cq != '\0')
		*cp++ = *cq++;

	*cp++ = '=';
	fstamp = uval - JAN_1970;
	tm = gmtime_r(&fstamp, &tmbuf);
	if (NULL ==  tm)
		return;
	NTP_INSIST((cp - buffer) < (int)sizeof(buffer));
	snprintf(cp, sizeof(buffer) - (cp - buffer),
		 "%04d%02d%02d%02d%02d", tm->tm_year + 1900,
		 tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
	cp += strlen(cp);
	ctl_putdata(buffer, (unsigned)( cp - buffer ), false);
}


/*
 * ctl_puthex - write a tagged unsigned integer, in hex, into the
 * response
 */
static void
ctl_puthex(
	const char *tag,
	u_long uval
	)
{
	register char *cp;
	register const char *cq;
	char buffer[200];

	cp = buffer;
	cq = tag;
	while (*cq != '\0')
		*cp++ = *cq++;

	*cp++ = '=';
	NTP_INSIST((cp - buffer) < (int)sizeof(buffer));
	snprintf(cp, sizeof(buffer) - (cp - buffer), "0x%lx", uval);
	cp += strlen(cp);
	ctl_putdata(buffer,(unsigned)( cp - buffer ), false);
}


/*
 * ctl_putint - write a tagged signed integer into the response
 */
static void
ctl_putint(
	const char *tag,
	long ival
	)
{
	register char *cp;
	register const char *cq;
	char buffer[200];

	cp = buffer;
	cq = tag;
	while (*cq != '\0')
		*cp++ = *cq++;

	*cp++ = '=';
	NTP_INSIST((cp - buffer) < (int)sizeof(buffer));
	snprintf(cp, sizeof(buffer) - (cp - buffer), "%ld", ival);
	cp += strlen(cp);
	ctl_putdata(buffer, (unsigned)( cp - buffer ), false);
}


/*
 * ctl_putts - write a tagged timestamp, in hex, into the response
 */
static void
ctl_putts(
	const char *tag,
	l_fp *ts
	)
{
	register char *cp;
	register const char *cq;
	char buffer[200];

	cp = buffer;
	cq = tag;
	while (*cq != '\0')
		*cp++ = *cq++;

	*cp++ = '=';
	NTP_INSIST((size_t)(cp - buffer) < sizeof(buffer));
	snprintf(cp, sizeof(buffer) - (cp - buffer), "0x%08x.%08x",
		 (u_int)lfpuint(*ts), (u_int)lfpfrac(*ts));
	cp += strlen(cp);
	ctl_putdata(buffer, (unsigned)( cp - buffer ), false);
}


/*
 * ctl_putadr - write an IP address into the response
 */
static void
ctl_putadr(
	const char *tag,
	uint32_t addr32,
	sockaddr_u *addr
	)
{
	register char *cp;
	register const char *cq;
	char buffer[200];

	cp = buffer;
	cq = tag;
	while (*cq != '\0')
		*cp++ = *cq++;

	*cp++ = '=';
	if (NULL == addr)
		cq = numtoa(addr32);
	else
		cq = socktoa(addr);
	NTP_INSIST((cp - buffer) < (int)sizeof(buffer));
	snprintf(cp, sizeof(buffer) - (cp - buffer), "%s", cq);
	cp += strlen(cp);
	ctl_putdata(buffer, (unsigned)(cp - buffer), false);
}


/*
 * ctl_putrefid - send a uint32_t refid as printable text
 */
static void
ctl_putrefid(
	const char *	tag,
	uint32_t		refid
	)
{
	char	output[16];
	char *	optr;
	char *	oplim;
	char *	iptr;
	char *	iplim;
	char *	past_eq = NULL;

	optr = output;
	oplim = output + sizeof(output);
	while (optr < oplim && '\0' != *tag)
		*optr++ = *tag++;
	if (optr < oplim) {
		*optr++ = '=';
		past_eq = optr;
	}
	if (!(optr < oplim))
		return;
	iptr = (char *)&refid;
	iplim = iptr + sizeof(refid);
	for ( ; optr < oplim && iptr < iplim && '\0' != *iptr;
	     iptr++, optr++)
		if (isprint((int)*iptr))
			*optr = *iptr;
		else
			*optr = '.';
	if (!(optr <= oplim))
		optr = past_eq;
	ctl_putdata(output, (u_int)(optr - output), false);
}


/*
 * ctl_putarray - write a tagged eight element double array into the response
 */
static void
ctl_putarray(
	const char *tag,
	double *arr,
	int start
	)
{
	register char *cp;
	register const char *cq;
	char buffer[200];
	int i;
	cp = buffer;
	cq = tag;
	while (*cq != '\0')
		*cp++ = *cq++;
	*cp++ = '=';
	i = start;
	do {
		if (i == 0)
			i = NTP_SHIFT;
		i--;
		NTP_INSIST((cp - buffer) < (int)sizeof(buffer));
		snprintf(cp, sizeof(buffer) - (cp - buffer),
			 " %.2f", arr[i] * 1e3);
		cp += strlen(cp);
	} while (i != start);
	ctl_putdata(buffer, (unsigned)(cp - buffer), false);
}


/*
 * ctl_putsys - output a system variable
 */
static void
ctl_putsys(
	int varid
	)
{
	l_fp tmp;
	char str[256];
	u_int u;
	double kb;
	double dtemp;
	const char *ss;
#ifdef HAVE_KERNEL_PLL
	static struct timex ntx;
	static u_long ntp_adjtime_time;

	/*
	 * CS_K_* variables depend on up-to-date output of ntp_adjtime()
	 */
	if (CS_KERN_FIRST <= varid && varid <= CS_KERN_LAST &&
	    current_time != ntp_adjtime_time) {
		ZERO(ntx);
		if (ntp_adjtime(&ntx) < 0)
			msyslog(LOG_ERR, "ntp_adjtime() for mode 6 query failed: %m");
		else
			ntp_adjtime_time = current_time;
	}
#endif	/* HAVE_KERNEL_PLL */

	switch (varid) {

	case CS_LEAP:
		ctl_putuint(sys_var[CS_LEAP].text, sys_leap);
		break;

	case CS_STRATUM:
		ctl_putuint(sys_var[CS_STRATUM].text, sys_stratum);
		break;

	case CS_PRECISION:
		ctl_putint(sys_var[CS_PRECISION].text, sys_precision);
		break;

	case CS_ROOTDELAY:
		ctl_putdbl(sys_var[CS_ROOTDELAY].text, sys_rootdelay *
			   1e3);
		break;

	case CS_ROOTDISPERSION:
		ctl_putdbl(sys_var[CS_ROOTDISPERSION].text,
			   sys_rootdisp * 1e3);
		break;

	case CS_REFID:
		if (sys_stratum > 1 && sys_stratum < STRATUM_UNSPEC)
			ctl_putadr(sys_var[varid].text, sys_refid, NULL);
		else
			ctl_putrefid(sys_var[varid].text, sys_refid);
		break;

	case CS_REFTIME:
		ctl_putts(sys_var[CS_REFTIME].text, &sys_reftime);
		break;

	case CS_POLL:
		ctl_putuint(sys_var[CS_POLL].text, sys_poll);
		break;

	case CS_PEERID:
		if (sys_peer == NULL)
			ctl_putuint(sys_var[CS_PEERID].text, 0);
		else
			ctl_putuint(sys_var[CS_PEERID].text,
				    sys_peer->associd);
		break;

	case CS_PEERADR:
		if (sys_peer != NULL && sys_peer->dstadr != NULL)
			ss = sockporttoa(&sys_peer->srcadr);
		else
			ss = "0.0.0.0:0";
		ctl_putunqstr(sys_var[CS_PEERADR].text, ss, strlen(ss));
		break;

	case CS_PEERMODE:
		u = (sys_peer != NULL)
			? sys_peer->hmode
			: MODE_UNSPEC;
		ctl_putuint(sys_var[CS_PEERMODE].text, u);
		break;

	case CS_OFFSET:
		ctl_putdbl6(sys_var[CS_OFFSET].text, last_offset * 1e3);
		break;

	case CS_DRIFT:
                /* a.k.a frequency.  (s/s), reported as us/s a.k.a. ppm */
		ctl_putdbl6(sys_var[CS_DRIFT].text, drift_comp * 1e6);
		break;

	case CS_JITTER:
		ctl_putdbl6(sys_var[CS_JITTER].text, sys_jitter * 1e3);
		break;

	case CS_ERROR:
		/* a.k.a clk_jitter (s).  output as ms */
		ctl_putdbl6(sys_var[CS_ERROR].text, clock_jitter * 1e3);
		break;

	case CS_CLOCK:
		get_systime(&tmp);
		ctl_putts(sys_var[CS_CLOCK].text, &tmp);
		break;

	case CS_PROCESSOR:
		ctl_putstr(sys_var[CS_PROCESSOR].text,
			   utsnamebuf.machine, strlen(utsnamebuf.machine));
		break;

	case CS_SYSTEM:
		snprintf(str, sizeof(str), "%s/%s", utsnamebuf.sysname,
			 utsnamebuf.release);
		ctl_putstr(sys_var[CS_SYSTEM].text, str, strlen(str));
		break;

	case CS_VERSION:
		ss = ntpd_version();
		ctl_putstr(sys_var[CS_VERSION].text, ss, strlen(ss));
		break;

	case CS_STABIL:
		/* a.k.a clk_wander (s/s), output as us/s */
		ctl_putdbl6(sys_var[CS_STABIL].text, clock_stability *
			   1e6);
		break;

	case CS_VARLIST:
	{
		char buf[CTL_MAX_DATA_LEN];
		//buffPointer, firstElementPointer, buffEndPointer
		char *buffp, *buffend;
		bool firstVarName;
		const char *ss1;
		int len;
		const struct ctl_var *k;

		buffp = buf;
		buffend = buf + sizeof(buf);
		if (buffp + strlen(sys_var[CS_VARLIST].text) + 4 > buffend)
			break;	/* really long var name */

		snprintf(buffp, sizeof(buf), "%s=\"",sys_var[CS_VARLIST].text);
		buffp += strlen(buffp);
		firstVarName = true;
		for (k = sys_var; !(k->flags & EOV); k++) {
			if (k->flags & PADDING)
				continue;
			len = strlen(k->text);
			if (buffp + len + 1 >= buffend)
				break;
			if (!firstVarName)
				*buffp++ = ',';
			else
				firstVarName = false;
			memcpy(buffp, k->text, len);
			buffp += len;
		}

		for (k = ext_sys_var; k && !(k->flags & EOV); k++) {
			if (k->flags & PADDING)
				continue;
			if (NULL == k->text)
				continue;
			ss1 = strchr(k->text, '=');
			if (NULL == ss1)
				len = strlen(k->text);
			else
				len = ss1 - k->text;
			if (buffp + len + 1 >= buffend)
				break;
			if (firstVarName) {
				*buffp++ = ',';
				firstVarName = false;
			}
			memcpy(buffp, k->text,(unsigned)len);
			buffp += len;
		}
		if (buffp + 2 >= buffend)
			break;

		*buffp++ = '"';
		*buffp = '\0';

		ctl_putdata(buf, (unsigned)( buffp - buf ), false);
		break;
	}

	case CS_TAI:
		if (sys_tai > 0)
			ctl_putuint(sys_var[CS_TAI].text, sys_tai);
		break;

	case CS_LEAPTAB:
	{
		leap_signature_t lsig;
		leapsec_getsig(&lsig);
		if (lsig.ttime > 0)
			ctl_putfs(sys_var[CS_LEAPTAB].text, lsig.ttime);
		break;
	}

	case CS_LEAPEND:
	{
		leap_signature_t lsig;
		leapsec_getsig(&lsig);
		if (lsig.etime > 0)
			ctl_putfs(sys_var[CS_LEAPEND].text, lsig.etime);
		break;
	}

#ifdef ENABLE_LEAP_SMEAR
	case CS_LEAPSMEARINTV:
		if (leap_smear_intv > 0)
			ctl_putuint(sys_var[CS_LEAPSMEARINTV].text, leap_smear_intv);
		break;

	case CS_LEAPSMEAROFFS:
		if (leap_smear_intv > 0)
			ctl_putdbl(sys_var[CS_LEAPSMEAROFFS].text,
				   leap_smear.doffset * 1e3);
		break;
#endif	/* ENABLE_LEAP_SMEAR */

	case CS_RATE:
		ctl_putuint(sys_var[CS_RATE].text, ntp_minpoll);
		break;

	case CS_MRU_ENABLED:
		ctl_puthex(sys_var[varid].text, mon_enabled);
		break;

	case CS_MRU_DEPTH:
		ctl_putuint(sys_var[varid].text, mru_entries);
		break;

	case CS_MRU_MEM:
		kb = mru_entries * (sizeof(mon_entry) / 1024.);
		u = (u_int)kb;
		if (kb - u >= 0.5)
			u++;
		ctl_putuint(sys_var[varid].text, u);
		break;

	case CS_MRU_DEEPEST:
		ctl_putuint(sys_var[varid].text, mru_peakentries);
		break;

	case CS_MRU_MINDEPTH:
		ctl_putuint(sys_var[varid].text, mru_mindepth);
		break;

	case CS_MRU_MAXAGE:
		ctl_putint(sys_var[varid].text, mru_maxage);
		break;

	case CS_MRU_MINAGE:
		ctl_putint(sys_var[varid].text, mru_minage);
		break;

	case CS_MRU_MAXDEPTH:
		ctl_putuint(sys_var[varid].text, mru_maxdepth);
		break;

	case CS_MRU_MAXMEM:
		kb = mru_maxdepth * (sizeof(mon_entry) / 1024.);
		u = (u_int)kb;
		if (kb - u >= 0.5)
			u++;
		ctl_putuint(sys_var[varid].text, u);
		break;

	case CS_MRU_EXISTS:
		ctl_putuint(sys_var[varid].text, mru_exists);
		break;

	case CS_MRU_NEW:
		ctl_putuint(sys_var[varid].text, mru_new);
		break;

	case CS_MRU_RECYCLEOLD:
		ctl_putuint(sys_var[varid].text, mru_recycleold);
		break;

	case CS_MRU_RECYCLEFULL:
		ctl_putuint(sys_var[varid].text, mru_recyclefull);
		break;

	case CS_MRU_NONE:
		ctl_putuint(sys_var[varid].text, mru_none);
		break;

	case CS_MRU_OLDEST_AGE: {
		l_fp now;
		get_systime(&now);
		ctl_putuint(sys_var[varid].text, mon_get_oldest_age(now));
		break;
		}

	case CS_SS_UPTIME:
		ctl_putuint(sys_var[varid].text, current_time);
		break;

	case CS_SS_RESET:
		ctl_putuint(sys_var[varid].text,
			    current_time - sys_stattime);
		break;

	case CS_SS_RECEIVED:
		ctl_putuint(sys_var[varid].text, sys_received);
		break;

	case CS_SS_THISVER:
		ctl_putuint(sys_var[varid].text, sys_newversion);
		break;

	case CS_SS_OLDVER:
		ctl_putuint(sys_var[varid].text, sys_oldversion);
		break;

	case CS_SS_BADFORMAT:
		ctl_putuint(sys_var[varid].text, sys_badlength);
		break;

	case CS_SS_BADAUTH:
		ctl_putuint(sys_var[varid].text, sys_badauth);
		break;

	case CS_SS_DECLINED:
		ctl_putuint(sys_var[varid].text, sys_declined);
		break;

	case CS_SS_RESTRICTED:
		ctl_putuint(sys_var[varid].text, sys_restricted);
		break;

	case CS_SS_LIMITED:
		ctl_putuint(sys_var[varid].text, sys_limitrejected);
		break;

	case CS_SS_KODSENT:
		ctl_putuint(sys_var[varid].text, sys_kodsent);
		break;

	case CS_SS_PROCESSED:
		ctl_putuint(sys_var[varid].text, sys_processed);
		break;

	case CS_AUTHDELAY:
		dtemp = lfptod(sys_authdelay);
		ctl_putdbl(sys_var[varid].text, dtemp * 1e3);
		break;

	case CS_AUTHKEYS:
		ctl_putuint(sys_var[varid].text, authnumkeys);
		break;

	case CS_AUTHFREEK:
		ctl_putuint(sys_var[varid].text, authnumfreekeys);
		break;

	case CS_AUTHKLOOKUPS:
		ctl_putuint(sys_var[varid].text, authkeylookups);
		break;

	case CS_AUTHKNOTFOUND:
		ctl_putuint(sys_var[varid].text, authkeynotfound);
		break;

	case CS_AUTHKUNCACHED:
		ctl_putuint(sys_var[varid].text, authkeyuncached);
		break;

	case CS_AUTHKEXPIRED:
	    /* historical relic - autokey used to expire keys */
		ctl_putuint(sys_var[varid].text, 0);
		break;

	case CS_AUTHENCRYPTS:
		ctl_putuint(sys_var[varid].text, authencryptions);
		break;

	case CS_AUTHDECRYPTS:
		ctl_putuint(sys_var[varid].text, authdecryptions);
		break;

	case CS_AUTHRESET:
		ctl_putuint(sys_var[varid].text,
			    current_time - auth_timereset);
		break;

		/*
		 * CTL_IF_KERNLOOP() puts a zero if the kernel loop is
		 * unavailable, otherwise calls putfunc with args.
		 */
#ifndef HAVE_KERNEL_PLL
# define	CTL_IF_KERNLOOP(putfunc, args)	\
		ctl_putint(sys_var[varid].text, 0)
#else
# define	CTL_IF_KERNLOOP(putfunc, args)	\
		putfunc args
#endif

		/*
		 * CTL_IF_KERNPPS() puts a zero if either the kernel
		 * loop is unavailable, or kernel hard PPS is not
		 * active, otherwise calls putfunc with args.
		 */
#ifndef HAVE_KERNEL_PLL
# define	CTL_IF_KERNPPS(putfunc, args)	\
		ctl_putint(sys_var[varid].text, 0)
#else
# define	CTL_IF_KERNPPS(putfunc, args)			\
		if (0 == ntx.shift)				\
			ctl_putint(sys_var[varid].text, 0);	\
		else						\
			putfunc args	/* no trailing ; */
#endif

	case CS_K_OFFSET:
		CTL_IF_KERNLOOP(
			ctl_putdblf,
			(sys_var[varid].text, 0, -1,
			 ntp_error_in_seconds(ntx.offset)*MILLISECONDS)
		);
		break;

	case CS_K_FREQ:
		CTL_IF_KERNLOOP(
			ctl_putsfp,
			(sys_var[varid].text, ntx.freq)
		);
		break;

	case CS_K_MAXERR:
		CTL_IF_KERNLOOP(
			ctl_putdblf,
			(sys_var[varid].text, 0, 6,
			 ntp_error_in_seconds(ntx.maxerror)*MILLISECONDS)
		);
		break;

	case CS_K_ESTERR:
		CTL_IF_KERNLOOP(
			ctl_putdblf,
			(sys_var[varid].text, 0, 6,
			 ntp_error_in_seconds(ntx.esterror)*MILLISECONDS)
		);
		break;

	case CS_K_STFLAGS:
#ifndef HAVE_KERNEL_PLL
		ss = "";
#else
		ss = k_st_flags(ntx.status);
#endif
		ctl_putstr(sys_var[varid].text, ss, strlen(ss));
		break;

	case CS_K_TIMECONST:
		CTL_IF_KERNLOOP(
			ctl_putint,
			(sys_var[varid].text, ntx.constant)
		);
		break;

	case CS_K_PRECISION:
		CTL_IF_KERNLOOP(
			ctl_putdblf,
			(sys_var[varid].text, 0, 6,
			 ntp_error_in_seconds(ntx.precision)*MILLISECONDS)
		);
		break;

	case CS_K_FREQTOL:
		CTL_IF_KERNLOOP(
			ctl_putsfp,
			(sys_var[varid].text, ntx.tolerance)
		);
		break;

	case CS_K_PPS_FREQ:
		CTL_IF_KERNPPS(
			ctl_putsfp,
			(sys_var[varid].text, ntx.ppsfreq)
		);
		break;

	case CS_K_PPS_STABIL:
		CTL_IF_KERNPPS(
			ctl_putsfp,
			(sys_var[varid].text, ntx.stabil)
		);
		break;

	case CS_K_PPS_JITTER:
		CTL_IF_KERNPPS(
			ctl_putdbl,
			(sys_var[varid].text,
			 ntp_error_in_seconds(ntx.jitter)*MILLISECONDS)
		);
		break;

	case CS_K_PPS_CALIBDUR:
		CTL_IF_KERNPPS(
			ctl_putint,
			(sys_var[varid].text, 1 << ntx.shift)
		);
		break;

	case CS_K_PPS_CALIBS:
		CTL_IF_KERNPPS(
			ctl_putint,
			(sys_var[varid].text, ntx.calcnt)
		);
		break;

	case CS_K_PPS_CALIBERRS:
		CTL_IF_KERNPPS(
			ctl_putint,
			(sys_var[varid].text, ntx.errcnt)
		);
		break;

	case CS_K_PPS_JITEXC:
		CTL_IF_KERNPPS(
			ctl_putint,
			(sys_var[varid].text, ntx.jitcnt)
		);
		break;

	case CS_K_PPS_STBEXC:
		CTL_IF_KERNPPS(
			ctl_putint,
			(sys_var[varid].text, ntx.stbcnt)
		);
		break;

	case CS_IOSTATS_RESET:
		ctl_putuint(sys_var[varid].text,
			    current_time - io_timereset);
		break;

	case CS_TOTAL_RBUF:
		ctl_putuint(sys_var[varid].text, total_recvbuffs());
		break;

	case CS_FREE_RBUF:
		ctl_putuint(sys_var[varid].text, free_recvbuffs());
		break;

	case CS_USED_RBUF:
		ctl_putuint(sys_var[varid].text, full_recvbuffs());
		break;

	case CS_RBUF_LOWATER:
		ctl_putuint(sys_var[varid].text, lowater_additions());
		break;

	case CS_IO_DROPPED:
		ctl_putuint(sys_var[varid].text, packets_dropped);
		break;

	case CS_IO_IGNORED:
		ctl_putuint(sys_var[varid].text, packets_ignored);
		break;

	case CS_IO_RECEIVED:
		ctl_putuint(sys_var[varid].text, packets_received);
		break;

	case CS_IO_SENT:
		ctl_putuint(sys_var[varid].text, packets_sent);
		break;

	case CS_IO_SENDFAILED:
		ctl_putuint(sys_var[varid].text, packets_notsent);
		break;

	case CS_IO_WAKEUPS:
		ctl_putuint(sys_var[varid].text, handler_calls);
		break;

	case CS_IO_GOODWAKEUPS:
		ctl_putuint(sys_var[varid].text, handler_pkts);
		break;

	case CS_TIMERSTATS_RESET:
		ctl_putuint(sys_var[varid].text,
			    current_time - timer_timereset);
		break;

	case CS_TIMER_OVERRUNS:
		ctl_putuint(sys_var[varid].text, alarm_overflow);
		break;

	case CS_TIMER_XMTS:
		ctl_putuint(sys_var[varid].text, timer_xmtcalls);
		break;

	case CS_FUZZ:
		/* a.k.a. fuzz (s), output in ms */
		ctl_putdbl6(sys_var[varid].text, sys_fuzz * 1e3);
		break;
	case CS_WANDER_THRESH:
		ctl_putdbl(sys_var[varid].text, wander_threshold * 1e6);
		break;
	case CS_TICK:
		/* a.k.a. sys_tick (s), output in ms */
		ctl_putdbl6(sys_var[varid].text, sys_tick * 1e3);
		break;
	}
}


/*
 * ctl_putpeer - output a peer variable
 */
static void
ctl_putpeer(
	int id,
	struct peer *p
	)
{
	char buf[CTL_MAX_DATA_LEN];
	char *s;
	char *t;
	char *be;
	int i;
	const struct ctl_var *k;

	switch (id) {

	case CP_CONFIG:
		ctl_putuint(peer_var[id].text,
			    !(FLAG_PREEMPT & p->flags));
		break;

	case CP_AUTHENABLE:
		ctl_putuint(peer_var[id].text, !(p->keyid));
		break;

	case CP_AUTHENTIC:
		ctl_putuint(peer_var[id].text,
			    !!(FLAG_AUTHENTIC & p->flags));
		break;

	case CP_SRCADR:
		ctl_putadr(peer_var[id].text, 0, &p->srcadr);
		break;

	case CP_SRCPORT:
		ctl_putuint(peer_var[id].text, SRCPORT(&p->srcadr));
		break;

	case CP_SRCHOST:
		if (p->hostname != NULL)
			ctl_putstr(peer_var[id].text, p->hostname,
				   strlen(p->hostname));
#ifdef REFCLOCK
		if (p->procptr != NULL) {
		    char buf1[NI_MAXHOST];
		    strlcpy(buf1, refclock_name(p), sizeof(buf1));
		    ctl_putstr(peer_var[id].text, buf1, strlen(buf1));
		}
#endif /* REFCLOCK */
		break;

	case CP_DSTADR:
		ctl_putadr(peer_var[id].text, 0,
			   (p->dstadr != NULL)
				? &p->dstadr->sin
				: NULL);
		break;

	case CP_DSTPORT:
		ctl_putuint(peer_var[id].text,
			    (p->dstadr != NULL)
				? SRCPORT(&p->dstadr->sin)
				: 0);
		break;

	case CP_IN:
		if (p->r21 > 0.)
			ctl_putdbl(peer_var[id].text, p->r21 / 1e3);
		break;

	case CP_OUT:
		if (p->r34 > 0.)
			ctl_putdbl(peer_var[id].text, p->r34 / 1e3);
		break;

	case CP_RATE:
		ctl_putuint(peer_var[id].text, p->throttle);
		break;

	case CP_LEAP:
		ctl_putuint(peer_var[id].text, p->leap);
		break;

	case CP_HMODE:
		ctl_putuint(peer_var[id].text, p->hmode);
		break;

	case CP_STRATUM:
		ctl_putuint(peer_var[id].text, p->stratum);
		break;

	case CP_PPOLL:
		ctl_putuint(peer_var[id].text, p->ppoll);
		break;

	case CP_HPOLL:
		ctl_putuint(peer_var[id].text, p->hpoll);
		break;

	case CP_PRECISION:
		ctl_putint(peer_var[id].text, p->precision);
		break;

	case CP_ROOTDELAY:
		ctl_putdbl(peer_var[id].text, p->rootdelay * 1e3);
		break;

	case CP_ROOTDISPERSION:
		ctl_putdbl(peer_var[id].text, p->rootdisp * 1e3);
		break;

	case CP_REFID:
#ifdef REFCLOCK
		if (p->flags & FLAG_REFCLOCK) {
			ctl_putrefid(peer_var[id].text, p->refid);
			break;
		}
#endif
		if (p->stratum > 1 && p->stratum < STRATUM_UNSPEC)
			ctl_putadr(peer_var[id].text, p->refid,
				   NULL);
		else
			ctl_putrefid(peer_var[id].text, p->refid);
		break;

	case CP_REFTIME:
		ctl_putts(peer_var[id].text, &p->reftime);
		break;

	case CP_REC:
		ctl_putts(peer_var[id].text, &p->dst);
		break;

	case CP_XMT:
		ctl_putts(peer_var[id].text, &p->xmt);
		break;

	case CP_BIAS:
		if (p->bias != 0.)
			ctl_putdbl(peer_var[id].text, p->bias * 1e3);
		break;

	case CP_REACH:
		ctl_puthex(peer_var[id].text, p->reach);
		break;

	case CP_FLASH:
		ctl_puthex(peer_var[id].text, p->flash);
		break;

	case CP_TTL:
#ifdef REFCLOCK
		if (p->flags & FLAG_REFCLOCK) {
			ctl_putuint(peer_var[id].text, p->ttl);
			break;
		}
#endif
		break;

	case CP_UNREACH:
		ctl_putuint(peer_var[id].text, p->unreach);
		break;

	case CP_TIMER:
		ctl_putuint(peer_var[id].text,
			    p->nextdate - current_time);
		break;

	case CP_DELAY:
		ctl_putdbl6(peer_var[id].text, p->delay * 1e3);
		break;

	case CP_OFFSET:
		ctl_putdbl6(peer_var[id].text, p->offset * 1e3);
		break;

	case CP_JITTER:
		ctl_putdbl6(peer_var[id].text, p->jitter * 1e3);
		break;

	case CP_DISPERSION:
		ctl_putdbl6(peer_var[id].text, p->disp * 1e3);
		break;

	case CP_KEYID:
		if (p->keyid > NTP_MAXKEY)
			ctl_puthex(peer_var[id].text, p->keyid);
		else
			ctl_putuint(peer_var[id].text, p->keyid);
		break;

	case CP_FILTDELAY:
		ctl_putarray(peer_var[id].text, p->filter_delay,
			     p->filter_nextpt);
		break;

	case CP_FILTOFFSET:
		ctl_putarray(peer_var[id].text, p->filter_offset,
			     p->filter_nextpt);
		break;

	case CP_FILTERROR:
		ctl_putarray(peer_var[id].text, p->filter_disp,
			     p->filter_nextpt);
		break;

	case CP_PMODE:
		ctl_putuint(peer_var[id].text, p->pmode);
		break;

	case CP_RECEIVED:
		ctl_putuint(peer_var[id].text, p->received);
		break;

	case CP_SENT:
		ctl_putuint(peer_var[id].text, p->sent);
		break;

	case CP_VARLIST:
		s = buf;
		be = buf + sizeof(buf);
		if (strlen(peer_var[id].text) + 4 > sizeof(buf))
			break;	/* really long var name */

		snprintf(s, sizeof(buf), "%s=\"", peer_var[id].text);
		s += strlen(s);
		t = s;
		for (k = peer_var; !(EOV & k->flags); k++) {
			if (PADDING & k->flags)
				continue;
			i = strlen(k->text);
			if (s + i + 1 >= be)
				break;
			if (s != t)
				*s++ = ',';
			memcpy(s, k->text, i);
			s += i;
		}
		if (s + 2 < be) {
			*s++ = '"';
			*s = '\0';
			ctl_putdata(buf, (u_int)(s - buf), false);
		}
		break;

	case CP_TIMEREC:
		ctl_putuint(peer_var[id].text,
			    current_time - p->timereceived);
		break;

	case CP_TIMEREACH:
		ctl_putuint(peer_var[id].text,
			    current_time - p->timereachable);
		break;

	case CP_BADAUTH:
		ctl_putuint(peer_var[id].text, p->badauth);
		break;

	case CP_BOGUSORG:
		ctl_putuint(peer_var[id].text, p->bogusorg);
		break;

	case CP_OLDPKT:
		ctl_putuint(peer_var[id].text, p->oldpkt);
		break;

	case CP_SELDISP:
		ctl_putuint(peer_var[id].text, p->seldisptoolarge);
		break;

	case CP_SELBROKEN:
		ctl_putuint(peer_var[id].text, p->selbroken);
		break;

	case CP_CANDIDATE:
		ctl_putuint(peer_var[id].text, p->status);
		break;

	default:
		break;
	}
}


#ifdef REFCLOCK
/*
 * ctl_putclock - output clock variables
 */
static void
ctl_putclock(
	int id,
	struct refclockstat *pcs,
	int mustput
	)
{
	char buf[CTL_MAX_DATA_LEN];
	char *s, *t, *be;
	const char *ss;
	int i;
	const struct ctl_var *k;

	switch (id) {

	case CC_NAME:
		if (pcs->clockname == NULL ||
		    *(pcs->clockname) == '\0') {
			if (mustput)
				ctl_putstr(clock_var[id].text,
					   "", 0);
		} else {
			ctl_putstr(clock_var[id].text,
				   pcs->clockname,
				   strlen(pcs->clockname));
		}
		break;
	case CC_TIMECODE:
		ctl_putstr(clock_var[id].text,
			   pcs->p_lastcode,
			   (unsigned)pcs->lencode);
		break;

	case CC_POLL:
		ctl_putuint(clock_var[id].text, pcs->polls);
		break;

	case CC_NOREPLY:
		ctl_putuint(clock_var[id].text,
			    pcs->noresponse);
		break;

	case CC_BADFORMAT:
		ctl_putuint(clock_var[id].text,
			    pcs->badformat);
		break;

	case CC_BADDATA:
		ctl_putuint(clock_var[id].text,
			    pcs->baddata);
		break;

	case CC_FUDGETIME1:
		if (mustput || (pcs->haveflags & CLK_HAVETIME1))
			ctl_putdbl(clock_var[id].text,
				   pcs->fudgetime1 * 1e3);
		break;

	case CC_FUDGETIME2:
		if (mustput || (pcs->haveflags & CLK_HAVETIME2))
			ctl_putdbl(clock_var[id].text,
				   pcs->fudgetime2 * 1e3);
		break;

	case CC_FUDGEVAL1:
		if (mustput || (pcs->haveflags & CLK_HAVEVAL1))
			ctl_putint(clock_var[id].text,
				   pcs->fudgeval1);
		break;

	case CC_FUDGEVAL2:
		if (mustput || (pcs->haveflags & CLK_HAVEVAL2)) {
			if (pcs->fudgeval1 > 1)
				ctl_putadr(clock_var[id].text,
					   pcs->fudgeval2, NULL);
			else
				ctl_putrefid(clock_var[id].text,
					     pcs->fudgeval2);
		}
		break;

	case CC_FLAGS:
		ctl_putuint(clock_var[id].text, pcs->flags);
		break;

	case CC_DEVICE:
		if (pcs->clockdesc == NULL ||
		    *(pcs->clockdesc) == '\0') {
			if (mustput)
				ctl_putstr(clock_var[id].text,
					   "", 0);
		} else {
			ctl_putstr(clock_var[id].text,
				   pcs->clockdesc,
				   strlen(pcs->clockdesc));
		}
		break;

	case CC_VARLIST:
		s = buf;
		be = buf + sizeof(buf);
		if (strlen(clock_var[CC_VARLIST].text) + 4 >
		    sizeof(buf))
			break;	/* really long var name */

		snprintf(s, sizeof(buf), "%s=\"",
			 clock_var[CC_VARLIST].text);
		s += strlen(s);
		t = s;

		for (k = clock_var; !(EOV & k->flags); k++) {
			if (PADDING & k->flags)
				continue;

			i = strlen(k->text);
			if (s + i + 1 >= be)
				break;

			if (s != t)
				*s++ = ',';
			memcpy(s, k->text, i);
			s += i;
		}

		for (k = pcs->kv_list; k && !(EOV & k->flags); k++) {
			if (PADDING & k->flags)
				continue;

			ss = k->text;
			if (NULL == ss)
				continue;

			while (*ss && *ss != '=')
				ss++;
			i = ss - k->text;
			if (s + i + 1 >= be)
				break;

			if (s != t)
				*s++ = ',';
			memcpy(s, k->text, (unsigned)i);
			s += i;
			*s = '\0';
		}
		if (s + 2 >= be)
			break;

		*s++ = '"';
		*s = '\0';
		ctl_putdata(buf, (unsigned)(s - buf), false);
		break;
	}
}
#endif



/*
 * ctl_getitem - get the next data item from the incoming packet
 */
static const struct ctl_var *
ctl_getitem(
	const struct ctl_var *var_list,
	char **data
	)
{
	static const struct ctl_var eol = { 0, EOV, NULL };
	static char buf[128];
	static u_long quiet_until;
	const struct ctl_var *v;
	const char *pch;
	char *cp;
	char *tp;

	/*
	 * Delete leading commas and white space
	 */
	while (reqpt < reqend && (*reqpt == ',' ||
				  isspace((unsigned char)*reqpt)))
		reqpt++;
	if (reqpt >= reqend)
		return NULL;

	if (NULL == var_list)
		return &eol;

	/*
	 * Look for a first character match on the tag.  If we find
	 * one, see if it is a full match.
	 */
	v = var_list;
	cp = reqpt;
	for (v = var_list; !(EOV & v->flags); v++) {
		if (!(PADDING & v->flags) && *cp == *(v->text)) {
			pch = v->text;
			while ('\0' != *pch && '=' != *pch && cp < reqend
			       && *cp == *pch) {
				cp++;
				pch++;
			}
			if ('\0' == *pch || '=' == *pch) {
				while (cp < reqend && isspace((uint8_t)*cp))
					cp++;
				if (cp == reqend || ',' == *cp) {
					buf[0] = '\0';
					*data = buf;
					if (cp < reqend)
						cp++;
					reqpt = cp;
					return v;
				}
				if ('=' == *cp) {
					cp++;
					tp = buf;
					while (cp < reqend && isspace((uint8_t)*cp))
						cp++;
					while (cp < reqend && *cp != ',') {
						*tp++ = *cp++;
						if ((size_t)(tp - buf) >= sizeof(buf)) {
							ctl_error(CERR_BADFMT);
							numctlbadpkts++;
							NLOG(NLOG_SYSEVENT)
								if (quiet_until <= current_time) {
									quiet_until = current_time + 300;
									msyslog(LOG_WARNING,
"Possible 'ntpdx' exploit from %s#%u (possibly spoofed)", socktoa(rmt_addr), SRCPORT(rmt_addr));
								}
							return NULL;
						}
					}
					if (cp < reqend)
						cp++;
					*tp-- = '\0';
					while (tp >= buf && isspace((uint8_t)*tp))
						*tp-- = '\0';
					reqpt = cp;
					*data = buf;
					return v;
				}
			}
			cp = reqpt;
		}
	}
	return v;
}


/*
 * control_unspec - response to an unspecified op-code
 */
/*ARGSUSED*/
static void
control_unspec(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
	struct peer *peer;

	UNUSED_ARG(rbufp);
	UNUSED_ARG(restrict_mask);

	/*
	 * What is an appropriate response to an unspecified op-code?
	 * I return no errors and no data, unless a specified association
	 * doesn't exist.
	 */
	if (res_associd) {
		peer = findpeerbyassoc(res_associd);
		if (NULL == peer) {
			ctl_error(CERR_BADASSOC);
			return;
		}
		rpkt.status = htons(ctlpeerstatus(peer));
	} else
		rpkt.status = htons(ctlsysstatus());
	ctl_flushpkt(0);
}


/*
 * read_status - return either a list of associd's, or a particular
 * peer's status.
 */
/*ARGSUSED*/
static void
read_status(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
	struct peer *peer;
	const uint8_t *cp;
	size_t n;
	/* a_st holds association ID, status pairs alternating */
	u_short a_st[CTL_MAX_DATA_LEN / sizeof(u_short)];

	UNUSED_ARG(rbufp);
	UNUSED_ARG(restrict_mask);
#ifdef DEBUG
	if (debug > 2)
		printf("read_status: ID %d\n", res_associd);
#endif
	/*
	 * Two choices here. If the specified association ID is
	 * zero we return all known association ID's.  Otherwise
	 * we return a bunch of stuff about the particular peer.
	 */
	if (res_associd) {
		peer = findpeerbyassoc(res_associd);
		if (NULL == peer) {
			ctl_error(CERR_BADASSOC);
			return;
		}
		rpkt.status = htons(ctlpeerstatus(peer));
		if (res_authokay)
			peer->num_events = 0;
		/*
		 * For now, output everything we know about the
		 * peer. May be more selective later.
		 */
		for (cp = def_peer_var; *cp != 0; cp++)
			ctl_putpeer((int)*cp, peer);
		ctl_flushpkt(0);
		return;
	}
	n = 0;
	rpkt.status = htons(ctlsysstatus());
	for (peer = peer_list; peer != NULL; peer = peer->p_link) {
		a_st[n++] = htons(peer->associd);
		a_st[n++] = htons(ctlpeerstatus(peer));
		/* two entries each loop iteration, so n + 1 */
		if (n + 1 >= COUNTOF(a_st)) {
			ctl_putdata((void *)a_st, n * sizeof(a_st[0]),
				    true);
			n = 0;
		}
	}
	if (n)
		ctl_putdata((void *)a_st, n * sizeof(a_st[0]), true);
	ctl_flushpkt(0);
}


/*
 * read_peervars - half of read_variables() implementation
 */
static void
read_peervars(void)
{
	const struct ctl_var *v;
	struct peer *peer;
	const uint8_t *cp;
	size_t i;
	char *	valuep;
	bool	wants[CP_MAXCODE + 1];
	bool	gotvar;

	/*
	 * Wants info for a particular peer. See if we know
	 * the guy.
	 */
	peer = findpeerbyassoc(res_associd);
	if (NULL == peer) {
		ctl_error(CERR_BADASSOC);
		return;
	}
	rpkt.status = htons(ctlpeerstatus(peer));
	if (res_authokay)
		peer->num_events = 0;
	ZERO(wants);
	gotvar = false;
	while (NULL != (v = ctl_getitem(peer_var, &valuep))) {
		if (v->flags & EOV) {
			ctl_error(CERR_UNKNOWNVAR);
			return;
		}
		NTP_INSIST(v->code < COUNTOF(wants));
		wants[v->code] = 1;
		gotvar = true;
	}
	if (gotvar) {
		for (i = 1; i < COUNTOF(wants); i++)
			if (wants[i])
				ctl_putpeer(i, peer);
	} else
		for (cp = def_peer_var; *cp != 0; cp++)
			ctl_putpeer((int)*cp, peer);
	ctl_flushpkt(0);
}


/*
 * read_sysvars - half of read_variables() implementation
 */
static void
read_sysvars(void)
{
	const struct ctl_var *v;
	struct ctl_var *kv;
	u_int	n;
	bool	gotvar;
	const uint8_t *cs;
	char *	valuep;
	const char * pch;
	uint8_t *wants;
	size_t	wants_count;

	/*
	 * Wants system variables. Figure out which he wants
	 * and give them to him.
	 */
	rpkt.status = htons(ctlsysstatus());
	if (res_authokay)
		ctl_sys_num_events = 0;
	wants_count = CS_MAXCODE + 1 + count_var(ext_sys_var);
	wants = emalloc_zero(wants_count);
	gotvar = false;
	while (NULL != (v = ctl_getitem(sys_var, &valuep))) {
		if (!(EOV & v->flags)) {
			NTP_INSIST(v->code < wants_count);
			wants[v->code] = 1;
			gotvar = true;
		} else {
			v = ctl_getitem(ext_sys_var, &valuep);
			if (NULL == v) {
				ctl_error(CERR_BADVALUE);
				free(wants);
				return;
			}

			if (EOV & v->flags) {
				ctl_error(CERR_UNKNOWNVAR);
				free(wants);
				return;
			}
			n = v->code + CS_MAXCODE + 1;
			NTP_INSIST(n < wants_count);
			wants[n] = 1;
			gotvar = true;
		}
	}
	if (gotvar) {
		for (n = 1; n <= CS_MAXCODE; n++)
			if (wants[n])
				ctl_putsys(n);
		for (n = 0; n + CS_MAXCODE + 1 < wants_count; n++)
			if (wants[n + CS_MAXCODE + 1]) {
				pch = ext_sys_var[n].text;
				ctl_putdata(pch, strlen(pch), false);
			}
	} else {
		for (cs = def_sys_var; *cs != 0; cs++)
			ctl_putsys((int)*cs);
		for (kv = ext_sys_var; kv && !(EOV & kv->flags); kv++)
			if (DEF & kv->flags)
				ctl_putdata(kv->text, strlen(kv->text),
					    false);
	}
	free(wants);
	ctl_flushpkt(0);
}


/*
 * read_variables - return the variables the caller asks for
 */
/*ARGSUSED*/
static void
read_variables(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
	UNUSED_ARG(rbufp);
	UNUSED_ARG(restrict_mask);

	if (res_associd)
		read_peervars();
	else
		read_sysvars();
}


/*
 * write_variables - write into variables. We only allow leap bit
 * writing this way.
 */
/*ARGSUSED*/
static void
write_variables(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
	const struct ctl_var *v;
	int ext_var;
	char *valuep;
	long val;
	size_t octets;
	char *vareqv;
	const char *t;
	char *tt;

	UNUSED_ARG(rbufp);
	UNUSED_ARG(restrict_mask);

	val = 0;
	/*
	 * If he's trying to write into a peer tell him no way
	 */
	if (res_associd != 0) {
		ctl_error(CERR_PERMISSION);
		return;
	}

	/*
	 * Set status
	 */
	rpkt.status = htons(ctlsysstatus());

	/*
	 * Look through the variables. Dump out at the first sign of
	 * trouble.
	 */
	while ((v = ctl_getitem(sys_var, &valuep)) != 0) {
		ext_var = 0;
		if (v->flags & EOV) {
			if ((v = ctl_getitem(ext_sys_var, &valuep)) !=
			    0) {
				if (v->flags & EOV) {
					ctl_error(CERR_UNKNOWNVAR);
					return;
				}
				ext_var = 1;
			} else {
				break;
			}
		}
		if (!(v->flags & CAN_WRITE)) {
			ctl_error(CERR_PERMISSION);
			return;
		}
		errno = 0;
		if (!ext_var && (*valuep == '\0'
				 || (val = strtol(valuep, NULL, 10), errno != 0))) {
			ctl_error(CERR_BADFMT);
			return;
		}
		if (!ext_var && (val & ~LEAP_NOTINSYNC) != 0) {
			ctl_error(CERR_BADVALUE);
			return;
		}

		if (ext_var) {
			octets = strlen(v->text) + strlen(valuep) + 2;
			vareqv = emalloc(octets);
			tt = vareqv;
			t = v->text;
			while (*t && *t != '=')
				*tt++ = *t++;
			*tt++ = '=';
			memcpy(tt, valuep, 1 + strlen(valuep));
			set_sys_var(vareqv, 1 + strlen(vareqv), v->flags);
			free(vareqv);
		} else {
			ctl_error(CERR_UNSPEC); /* really */
			return;
		}
	}

	/*
	 * If we got anything, do it. xxx nothing to do ***
	 */
	/*
	  if (leapind != ~0 || leapwarn != ~0) {
	  if (!leap_setleap((int)leapind, (int)leapwarn)) {
	  ctl_error(CERR_PERMISSION);
	  return;
	  }
	  }
	*/
	ctl_flushpkt(0);
}

/*
 * configure() processes ntpq :config/config-from-file, allowing
 *		generic runtime reconfiguration.
 */
static void configure(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
	size_t data_count;
	int retval;
	bool replace_nl;

	/* I haven't yet implemented changes to an existing association.
	 * Hence check if the association id is 0
	 */
	if (res_associd != 0) {
		ctl_error(CERR_BADVALUE);
		return;
	}

	if (RES_NOMODIFY & restrict_mask) {
		snprintf(remote_config.err_msg,
			 sizeof(remote_config.err_msg),
			 "runtime configuration prohibited by restrict ... nomodify");
		ctl_putdata(remote_config.err_msg,
			    strlen(remote_config.err_msg), false);
		ctl_flushpkt(0);
		NLOG(NLOG_SYSINFO)
			msyslog(LOG_NOTICE,
				"runtime config from %s rejected due to nomodify restriction",
				socktoa(&rbufp->recv_srcadr));
		sys_restricted++;
		return;
	}

	/* Initialize the remote config buffer */
	data_count = reqend - reqpt;

	if (data_count > sizeof(remote_config.buffer) - 2) {
		snprintf(remote_config.err_msg,
			 sizeof(remote_config.err_msg),
			 "runtime configuration failed: request too long");
		ctl_putdata(remote_config.err_msg,
			    strlen(remote_config.err_msg), false);
		ctl_flushpkt(0);
		msyslog(LOG_NOTICE,
			"runtime config from %s rejected: request too long",
			socktoa(&rbufp->recv_srcadr));
		return;
	}

	memcpy(remote_config.buffer, reqpt, data_count);
	if (data_count > 0
	    && '\n' != remote_config.buffer[data_count - 1])
		remote_config.buffer[data_count++] = '\n';
	remote_config.buffer[data_count] = '\0';
	remote_config.pos = 0;
	remote_config.err_pos = 0;
	remote_config.no_errors = 0;

	/* do not include terminating newline in log */
	if (data_count > 0
	    && '\n' == remote_config.buffer[data_count - 1]) {
		remote_config.buffer[data_count - 1] = '\0';
		replace_nl = true;
	} else {
		replace_nl = false;
	}

	DPRINTF(1, ("Got Remote Configuration Command: %s\n",
		remote_config.buffer));
	msyslog(LOG_NOTICE, "%s config: %s",
		socktoa(&rbufp->recv_srcadr),
		remote_config.buffer);

	if (replace_nl)
		remote_config.buffer[data_count - 1] = '\n';

	config_remotely(&rbufp->recv_srcadr);

	/*
	 * Check if errors were reported. If not, output 'Config
	 * Succeeded'.  Else output the error count.  It would be nice
	 * to output any parser error messages.
	 */
	if (0 == remote_config.no_errors) {
		retval = snprintf(remote_config.err_msg,
				  sizeof(remote_config.err_msg),
				  "Config Succeeded");
		if (retval > 0)
			remote_config.err_pos += retval;
	}

	ctl_putdata(remote_config.err_msg, remote_config.err_pos, false);
	ctl_flushpkt(0);

	DPRINTF(1, ("Reply: %s\n", remote_config.err_msg));

	if (remote_config.no_errors > 0)
		msyslog(LOG_NOTICE, "%d error in %s config",
			remote_config.no_errors,
			socktoa(&rbufp->recv_srcadr));
}


/*
 * derive_nonce - generate client-address-specific nonce value
 *		  associated with a given timestamp.
 */
static uint32_t derive_nonce(
	sockaddr_u *	addr,
	uint32_t		ts_i,
	uint32_t		ts_f
	)
{
	static uint32_t	salt[4];
	static u_long	last_salt_update;
	union d_tag {
		uint8_t	digest[EVP_MAX_MD_SIZE];
		uint32_t extract;
	}		d;
	EVP_MD_CTX	*ctx;
	u_int		len;

	while (!salt[0] || current_time - last_salt_update >= 3600) {
	    salt[0] = ntp_random();
		salt[1] = ntp_random();
		salt[2] = ntp_random();
		salt[3] = ntp_random();
		last_salt_update = current_time;
	}

	ctx = EVP_MD_CTX_create();
	EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
	EVP_DigestUpdate(ctx, salt, sizeof(salt));
	EVP_DigestUpdate(ctx, &ts_i, sizeof(ts_i));
	EVP_DigestUpdate(ctx, &ts_f, sizeof(ts_f));
	if (IS_IPV4(addr))
		EVP_DigestUpdate(ctx, &SOCK_ADDR4(addr),
			         sizeof(SOCK_ADDR4(addr)));
	else
		EVP_DigestUpdate(ctx, &SOCK_ADDR6(addr),
			         sizeof(SOCK_ADDR6(addr)));
	EVP_DigestUpdate(ctx, &NSRCPORT(addr), sizeof(NSRCPORT(addr)));
	EVP_DigestUpdate(ctx, salt, sizeof(salt));
	EVP_DigestFinal_ex(ctx, d.digest, &len);
	EVP_MD_CTX_destroy(ctx);

	return d.extract;
}


/*
 * generate_nonce - generate client-address-specific nonce string.
 */
static void generate_nonce(
	struct recvbuf *	rbufp,
	char *			nonce,
	size_t			nonce_octets
	)
{
	uint32_t derived;

	derived = derive_nonce(&rbufp->recv_srcadr,
			       lfpuint(rbufp->recv_time),
			       lfpfrac(rbufp->recv_time));
	snprintf(nonce, nonce_octets, "%08x%08x%08x",
		 lfpuint(rbufp->recv_time), lfpfrac(rbufp->recv_time), derived);
}


/*
 * validate_nonce - validate client-address-specific nonce string.
 *
 * Returns true if the local calculation of the nonce matches the
 * client-provided value and the timestamp is recent enough.
 */
static int validate_nonce(
	const char *		pnonce,
	struct recvbuf *	rbufp
	)
{
	u_int	ts_i;
	u_int	ts_f;
	l_fp	ts;
	l_fp	now_delta;
	u_int	supposed;
	u_int	derived;

	if (3 != sscanf(pnonce, "%08x%08x%08x", &ts_i, &ts_f, &supposed))
		return false;

	ts = lfpinit((uint32_t)ts_i, (uint32_t)ts_f);
	derived = derive_nonce(&rbufp->recv_srcadr, lfpuint(ts), lfpfrac(ts));
	get_systime(&now_delta);
	now_delta -= ts;

	return (supposed == derived && lfpuint(now_delta) < NONCE_TIMEOUT);
}


#ifdef USE_RANDOMIZE_RESPONSES
/*
 * send_random_tag_value - send a randomly-generated three character
 *			   tag prefix, a '.', an index, a '=' and a
 *			   random integer value.
 *
 * To try to force clients to ignore unrecognized tags in mrulist,
 * reslist, and ifstats responses, the first and last rows are spiced
 * with randomly-generated tag names with correct .# index.  Make it
 * three characters knowing that none of the currently-used subscripted
 * tags have that length, avoiding the need to test for
 * tag collision.
 */
static void
send_random_tag_value(
	int	indx
	)
{
	int	noise;
	char	buf[32];

	noise = ntp_random();
	buf[0] = 'a' + noise % 26;
	noise >>= 5;
	buf[1] = 'a' + noise % 26;
	noise >>= 5;
	buf[2] = 'a' + noise % 26;
	noise >>= 5;
	buf[3] = '.';
	snprintf(&buf[4], sizeof(buf) - 4, "%d", indx);
	ctl_putuint(buf, noise);
}
#endif /* USE_RANDOMIZE_RESPONSE */


/*
 * Send a MRU list entry in response to a "ntpq -c mrulist" operation.
 *
 * To keep clients honest about not depending on the order of values,
 * and thereby avoid being locked into ugly workarounds to maintain
 * backward compatibility later as new fields are added to the response,
 * the order is random.
 */
static void
send_mru_entry(
	mon_entry *	mon,
	int		count
	)
{
	const char first_fmt[] =	"first.%d";
	const char ct_fmt[] =		"ct.%d";
	const char mv_fmt[] =		"mv.%d";
	const char rs_fmt[] =		"rs.%d";
	char	tag[32];
	bool	sent[6]; /* 6 tag=value pairs */
	uint32_t noise;
	u_int	which = 0;
	u_int	remaining;
	const char * pch;

	remaining = COUNTOF(sent);
	ZERO(sent);
	noise = ntp_random();
	while (remaining > 0) {
#ifdef USE_RANDOMIZE_RESPONSES
	 	which = (noise & 7) % COUNTOF(sent);
#endif /* USE_RANDOMIZE_RESPONSES */
		noise >>= 3;
		while (sent[which])
			which = (which + 1) % COUNTOF(sent);

		switch (which) {

		case 0:
			snprintf(tag, sizeof(tag), addr_fmt, count);
			pch = sockporttoa(&mon->rmtadr);
			ctl_putunqstr(tag, pch, strlen(pch));
			break;

		case 1:
			snprintf(tag, sizeof(tag), last_fmt, count);
			ctl_putts(tag, &mon->last);
			break;

		case 2:
			snprintf(tag, sizeof(tag), first_fmt, count);
			ctl_putts(tag, &mon->first);
			break;

		case 3:
			snprintf(tag, sizeof(tag), ct_fmt, count);
			ctl_putint(tag, mon->count);
			break;

		case 4:
			snprintf(tag, sizeof(tag), mv_fmt, count);
			ctl_putuint(tag, mon->vn_mode);
			break;

		case 5:
			snprintf(tag, sizeof(tag), rs_fmt, count);
			ctl_puthex(tag, mon->flags);
			break;
		}
		sent[which] = true;
		remaining--;
	}
}


/*
 * read_mru_list - supports ntpq's mrulist command.
 *
 * The approach was suggested by Ry Jones.  A finite and variable number
 * of entries are retrieved per request, to avoid having responses with
 * such large numbers of packets that socket buffers are overflowed and
 * packets lost.  The entries are retrieved oldest-first, taking into
 * account that the MRU list will be changing between each request.  We
 * can expect to see duplicate entries for addresses updated in the MRU
 * list during the fetch operation.  In the end, the client can assemble
 * a close approximation of the MRU list at the point in time the last
 * response was sent by ntpd.  The only difference is it may be longer,
 * containing some number of oldest entries which have since been
 * reclaimed.  If necessary, the protocol could be extended to zap those
 * from the client snapshot at the end, but so far that doesn't seem
 * useful.
 *
 * To accommodate the changing MRU list, the starting point for requests
 * after the first request is supplied as a series of last seen
 * timestamps and associated addresses, the newest ones the client has
 * received.  As long as at least one of those entries hasn't been
 * bumped to the head of the MRU list, ntpd can pick up at that point.
 * Otherwise, the request is failed and it is up to ntpq to back up and
 * provide the next newest entry's timestamps and addresses, conceivably
 * backing up all the way to the starting point.
 *
 * input parameters:
 *	nonce=		Regurgitated nonce retrieved by the client
 *			previously using CTL_OP_REQ_NONCE, demonstrating
 *			ability to receive traffic sent to its address.
 *	frags=		Limit on datagrams (fragments) in response.  Used
 *			by newer ntpq versions instead of limit= when
 *			retrieving multiple entries.
 *	limit=		Limit on MRU entries returned.  One of frags= or
 *			limit= must be provided.
 *			limit=1 is a special case:  Instead of fetching
 *			beginning with the supplied starting point's
 *			newer neighbor, fetch the supplied entry, and
 *			in that case the #.last timestamp can be zero.
 *			This enables fetching a single entry by IP
 *			address.  When limit is not one and frags= is
 *			provided, the fragment limit controls.
 *	mincount=	(decimal) Return entries with count >= mincount.
 *	laddr=		Return entries associated with the server's IP
 *			address given.  No port specification is needed,
 *			and any supplied is ignored.
 *      recent=		Set the reporting start point to retrieve roughly
 *			a specified number of most recent entries
 *			'Roughly' because the logic cannot anticipate
 *			update volume. Use this to volume-limit the
 *			response when you are monitoring something like
 *			a pool server with a very long MRU list.
 *	resall=		0x-prefixed hex restrict bits which must all be
 *			lit for an MRU entry to be included.
 *			Has precedence over any resany=.
 *	resany=		0x-prefixed hex restrict bits, at least one of
 *			which must be list for an MRU entry to be
 *			included.
 *	last.0=		0x-prefixed hex l_fp timestamp of newest entry
 *			which client previously received.
 *	addr.0=		text of newest entry's IP address and port,
 *			IPv6 addresses in bracketed form: [::]:123
 *	last.1=		timestamp of 2nd newest entry client has.
 *	addr.1=		address of 2nd newest entry.
 *	[...]
 *
 * ntpq provides as many last/addr pairs as will fit in a single request
 * packet, except for the first request in a MRU fetch operation.
 *
 * The response begins with a new nonce value to be used for any
 * followup request.  Following the nonce is the next newer entry than
 * referred to by last.0 and addr.0, if the "0" entry has not been
 * bumped to the front.  If it has, the first entry returned will be the
 * next entry newer than referred to by last.1 and addr.1, and so on.
 * If none of the referenced entries remain unchanged, the request fails
 * and ntpq backs up to the next earlier set of entries to resync.
 *
 * Except for the first response, the response begins with confirmation
 * of the entry that precedes the first additional entry provided:
 *
 *	last.older=	hex l_fp timestamp matching one of the input
 *			.last timestamps, which entry now precedes the
 *			response 0. entry in the MRU list.
 *	addr.older=	text of address corresponding to older.last.
 *
 * And in any case, a successful response contains sets of values
 * comprising entries, with the oldest numbered 0 and incrementing from
 * there:
 *
 *	addr.#		text of IPv4 or IPv6 address and port
 *	last.#		hex l_fp timestamp of last receipt
 *	first.#		hex l_fp timestamp of first receipt
 *	ct.#		count of packets received
 *	mv.#		mode and version
 *	rs.#		restriction mask (RES_* bits)
 *
 * Note the code currently assumes there are no valid three letter
 * tags sent with each row, and needs to be adjusted if that changes.
 *
 * The client should accept the values in any order, and ignore .#
 * values which it does not understand, to allow a smooth path to
 * future changes without requiring a new opcode.  Clients can rely
 * on all *.0 values preceding any *.1 values, that is all values for
 * a given index number are together in the response.
 *
 * The end of the response list is noted with one or two tag=value
 * pairs.  Unconditionally:
 *
 *	now=		0x-prefixed l_fp timestamp at the server marking
 *			the end of the operation.
 *
 * If any entries were returned, now= is followed by:
 *
 *	last.newest=	hex l_fp identical to last.# of the prior
 *			entry.
 */
static void read_mru_list(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
	static const char	nulltxt[1] = 		{ '\0' };
	static const char	nonce_text[] =		"nonce";
	static const char	frags_text[] =		"frags";
	static const char	limit_text[] =		"limit";
	static const char	mincount_text[] =	"mincount";
	static const char	resall_text[] =		"resall";
	static const char	resany_text[] =		"resany";
	static const char	maxlstint_text[] =	"maxlstint";
	static const char	laddr_text[] =		"laddr";
	static const char	recent_text[] =		"recent";
	static const char	resaxx_fmt[] =		"0x%hx";

	u_int			limit;
	u_short			frags;
	u_short			resall;
	u_short			resany;
	int			mincount;
	u_int			maxlstint;
	sockaddr_u		laddr; u_int			recent;
	endpt *                 lcladr;
	u_int			count;
	static u_int		countdown;
	u_int			ui;
	u_int			uf;
	l_fp			last[16];
	sockaddr_u		addr[COUNTOF(last)];
	char			buf[128];
	struct ctl_var *	in_parms;
	const struct ctl_var *	v;
	const char *		val;
	const char *		pch;
	char *			pnonce;
	int			nonce_valid;
	size_t			i;
	int			priors;
	u_short			hash;
	mon_entry *		mon;
	mon_entry *		prior_mon;
	l_fp			now;

	if (RES_NOMRULIST & restrict_mask) {
		ctl_error(CERR_PERMISSION);
		NLOG(NLOG_SYSINFO)
			msyslog(LOG_NOTICE,
				"mrulist from %s rejected due to nomrulist restriction",
				socktoa(&rbufp->recv_srcadr));
		sys_restricted++;
		return;
	}
	/*
	 * fill in_parms var list with all possible input parameters.
	 */
	in_parms = NULL;
	set_var(&in_parms, nonce_text, sizeof(nonce_text), 0);
	set_var(&in_parms, frags_text, sizeof(frags_text), 0);
	set_var(&in_parms, limit_text, sizeof(limit_text), 0);
	set_var(&in_parms, mincount_text, sizeof(mincount_text), 0);
	set_var(&in_parms, resall_text, sizeof(resall_text), 0);
	set_var(&in_parms, resany_text, sizeof(resany_text), 0);
	set_var(&in_parms, maxlstint_text, sizeof(maxlstint_text), 0);
	set_var(&in_parms, laddr_text, sizeof(laddr_text), 0);
	set_var(&in_parms, recent_text, sizeof(recent_text), 0);
	for (i = 0; i < COUNTOF(last); i++) {
		snprintf(buf, sizeof(buf), last_fmt, (int)i);
		set_var(&in_parms, buf, strlen(buf) + 1, 0);
		snprintf(buf, sizeof(buf), addr_fmt, (int)i);
		set_var(&in_parms, buf, strlen(buf) + 1, 0);
	}

	/* decode input parms */
	pnonce = NULL;
	frags = 0;
	limit = 0;
	mincount = 0;
	resall = 0;
	resany = 0;
	maxlstint = 0;
	recent = 0;
	lcladr = NULL;
	priors = 0;
	ZERO(last);
	ZERO(addr);

	/* have to go through '(void*)' to drop 'const' property from pointer.
	 * ctl_getitem()' needs some cleanup, too.... perlinger@ntp.org
	 */
	while (NULL != (v = ctl_getitem(in_parms, (void*)&val)) &&
	       !(EOV & v->flags)) {
		int si;

		if (NULL == val)
			val = nulltxt;

		if (!strcmp(nonce_text, v->text)) {
			free(pnonce);
			pnonce = (*val) ? estrdup(val) : NULL;
		} else if (!strcmp(frags_text, v->text)) {
			if (1 != sscanf(val, "%hu", &frags))
				goto blooper;
		} else if (!strcmp(limit_text, v->text)) {
			if (1 != sscanf(val, "%u", &limit))
				goto blooper;
		} else if (!strcmp(mincount_text, v->text)) {
			if (1 != sscanf(val, "%d", &mincount))
				goto blooper;
			if (mincount < 0)
				mincount = 0;
		} else if (!strcmp(resall_text, v->text)) {
			if (1 != sscanf(val, resaxx_fmt, &resall))
				goto blooper;
		} else if (!strcmp(resany_text, v->text)) {
			if (1 != sscanf(val, resaxx_fmt, &resany))
				goto blooper;
		} else if (!strcmp(maxlstint_text, v->text)) {
			if (1 != sscanf(val, "%u", &maxlstint))
				goto blooper;
		} else if (!strcmp(laddr_text, v->text)) {
			if (!decodenetnum(val, &laddr))
				goto blooper;
			lcladr = getinterface(&laddr, 0);
		} else if (!strcmp(recent_text, v->text)) {
			if (1 != sscanf(val, "%u", &recent))
				goto blooper;
		} else if (1 == sscanf(v->text, last_fmt, &si) &&
			   (size_t)si < COUNTOF(last)) {
			if (2 != sscanf(val, "0x%08x.%08x", &ui, &uf))
				goto blooper;
			last[si] = lfpinit(ui, uf);
			if (!SOCK_UNSPEC(&addr[si]) && si == priors)
				priors++;
		} else if (1 == sscanf(v->text, addr_fmt, &si) &&
			   (size_t)si < COUNTOF(addr)) {
			if (!decodenetnum(val, &addr[si]))
				goto blooper;
			if (lfpuint(last[si]) && lfpfrac(last[si]) && si == priors)
				priors++;
		} else {
			DPRINTF(1, ("read_mru_list: invalid key item: '%s' (ignored)\n",
				    v->text));
			continue;

		blooper:
			DPRINTF(1, ("read_mru_list: invalid param for '%s': '%s' (bailing)\n",
				    v->text, val));
			free(pnonce);
			pnonce = NULL;
			break;
		}
	}
	free_varlist(in_parms);
	in_parms = NULL;

	/* return no responses until the nonce is validated */
	if (NULL == pnonce)
		return;

	nonce_valid = validate_nonce(pnonce, rbufp);
	free(pnonce);
	if (!nonce_valid)
		return;

	if ((0 == frags && !(0 < limit && limit <= MRU_ROW_LIMIT)) ||
	    frags > MRU_FRAGS_LIMIT) {
		ctl_error(CERR_BADVALUE);
		return;
	}

	/*
	 * If either frags or limit is not given, use the max.
	 */
	if (0 != frags && 0 == limit)
		limit = UINT_MAX;
	else if (0 != limit && 0 == frags)
		frags = MRU_FRAGS_LIMIT;

	/*
	 * Find the starting point if one was provided.
	 */
	mon = NULL;
	for (i = 0; i < (size_t)priors; i++) {
		hash = MON_HASH(&addr[i]);
		for (mon = mon_hash[hash];
		     mon != NULL;
		     mon = mon->hash_next)
			if (ADDR_PORT_EQ(&mon->rmtadr, &addr[i]))
				break;
		if (mon != NULL) {
			if (mon->last == last[i])
				break;
			mon = NULL;
		}
	}

	/* If a starting point was provided... */
	if (priors) {
		/* and none could be found unmodified... */
		if (NULL == mon) {
			/* tell ntpq to try again with older entries */
			ctl_error(CERR_UNKNOWNVAR);
			return;
		}
		/* confirm the prior entry used as starting point */
		ctl_putts("last.older", &mon->last);
		pch = sockporttoa(&mon->rmtadr);
		ctl_putunqstr("addr.older", pch, strlen(pch));

		/*
		 * Move on to the first entry the client doesn't have,
		 * except in the special case of a limit of one.  In
		 * that case return the starting point entry.
		 */
		if (limit > 1)
			mon = PREV_DLIST(mon_mru_list, mon, mru);
	} else {	/* start with the oldest */
		mon = TAIL_DLIST(mon_mru_list, mru);
		countdown = mru_entries;
	}

	/*
	 * send up to limit= entries in up to frags= datagrams
	 */
	get_systime(&now);
	generate_nonce(rbufp, buf, sizeof(buf));
	ctl_putunqstr("nonce", buf, strlen(buf));
	prior_mon = NULL;
	for (count = 0;
	     mon != NULL && res_frags < frags && count < limit;
	     mon = PREV_DLIST(mon_mru_list, mon, mru)) {

		if (mon->count < mincount)
			continue;
		if (resall && resall != (resall & mon->flags))
			continue;
		if (resany && !(resany & mon->flags))
			continue;
		if (maxlstint > 0 && lfpuint(now) - lfpuint(mon->last) >
		    maxlstint)
			continue;
		if (lcladr != NULL && mon->lcladr != lcladr)
			continue;
		if (recent != 0 && countdown-- > recent)
			continue;
		send_mru_entry(mon, count);
#ifdef USE_RANDOMIZE_RESPONSES
		if (!count)
			send_random_tag_value(0);
#endif /* USE_RANDOMIZE_RESPONSES */
		count++;
		prior_mon = mon;
	}

	/*
	 * If this batch completes the MRU list, say so explicitly with
	 * a now= l_fp timestamp.
	 */
	if (NULL == mon) {
#ifdef USE_RANDOMIZE_RESPONSES
		if (count > 1)
			send_random_tag_value(count - 1);
#endif /* USE_RANDOMIZE_RESPONSES */
		ctl_putts("now", &now);
		/* if any entries were returned confirm the last */
		if (prior_mon != NULL)
			ctl_putts("last.newest", &prior_mon->last);
	}
	ctl_flushpkt(0);
}

/*
 * Send a ifstats entry in response to a "ntpq -c ifstats" request.
 *
 * To keep clients honest about not depending on the order of values,
 * and thereby avoid being locked into ugly workarounds to maintain
 * backward compatibility later as new fields are added to the response,
 * the order is random.
 */
static void
send_ifstats_entry(
	endpt *	la,
	u_int	ifnum
	)
{
	const char addr_fmtu[] =	"addr.%u";
	const char bcast_fmt[] =	"bcast.%u";
	const char en_fmt[] =		"en.%u";	/* enabled */
	const char name_fmt[] =		"name.%u";
	const char flags_fmt[] =	"flags.%u";
	const char tl_fmt[] =		"tl.%u";	/* ttl */
	const char mc_fmt[] =		"mc.%u";	/* mcast count */
	const char rx_fmt[] =		"rx.%u";
	const char tx_fmt[] =		"tx.%u";
	const char txerr_fmt[] =	"txerr.%u";
	const char pc_fmt[] =		"pc.%u";	/* peer count */
	const char up_fmt[] =		"up.%u";	/* uptime */
	char	tag[32];
	uint8_t	sent[IFSTATS_FIELDS]; /* 12 tag=value pairs */
	int	noisebits;
	uint32_t noise;
	u_int	which = 0;
	u_int	remaining;
	const char *pch;

	remaining = COUNTOF(sent);
	ZERO(sent);
	noise = 0;
	noisebits = 0;
	while (remaining > 0) {
		if (noisebits < 4) {
			noise = ntp_random();
			noisebits = 31;
		}
#ifdef USE_RANDOMIZE_RESPONSES
		which = (noise & 0xf) % COUNTOF(sent);
#endif /* USE_RANDOMIZE_RESPONSES */
		noise >>= 4;
		noisebits -= 4;

		while (sent[which])
			which = (which + 1) % COUNTOF(sent);

		switch (which) {

		case 0:
			snprintf(tag, sizeof(tag), addr_fmtu, ifnum);
			pch = sockporttoa(&la->sin);
			ctl_putunqstr(tag, pch, strlen(pch));
			break;

		case 1:
			snprintf(tag, sizeof(tag), bcast_fmt, ifnum);
			if (INT_BCASTOPEN & la->flags)
				pch = sockporttoa(&la->bcast);
			else
				pch = "";
			ctl_putunqstr(tag, pch, strlen(pch));
			break;

		case 2:
			snprintf(tag, sizeof(tag), en_fmt, ifnum);
			ctl_putint(tag, !la->ignore_packets);
			break;

		case 3:
			snprintf(tag, sizeof(tag), name_fmt, ifnum);
			ctl_putstr(tag, la->name, strlen(la->name));
			break;

		case 4:
			snprintf(tag, sizeof(tag), flags_fmt, ifnum);
			ctl_puthex(tag, (u_int)la->flags);
			break;

		case 5:
			snprintf(tag, sizeof(tag), tl_fmt, ifnum);
			ctl_putint(tag, la->last_ttl);
			break;

		case 6:
			snprintf(tag, sizeof(tag), mc_fmt, ifnum);
			ctl_putint(tag, la->num_mcast);
			break;

		case 7:
			snprintf(tag, sizeof(tag), rx_fmt, ifnum);
			ctl_putint(tag, la->received);
			break;

		case 8:
			snprintf(tag, sizeof(tag), tx_fmt, ifnum);
			ctl_putint(tag, la->sent);
			break;

		case 9:
			snprintf(tag, sizeof(tag), txerr_fmt, ifnum);
			ctl_putint(tag, la->notsent);
			break;

		case 10:
			snprintf(tag, sizeof(tag), pc_fmt, ifnum);
			ctl_putuint(tag, la->peercnt);
			break;

		case 11:
			snprintf(tag, sizeof(tag), up_fmt, ifnum);
			ctl_putuint(tag, current_time - la->starttime);
			break;
		}
		sent[which] = true;
		remaining--;
	}
#ifdef USE_RANDOMIZE_RESPONSES
	send_random_tag_value((int)ifnum);
#endif /* USE_RANDOMIZE_RESPONSES */
}


/*
 * read_ifstats - send statistics for each local address, exposed by
 *		  ntpq -c ifstats
 */
static void
read_ifstats(
	struct recvbuf *	rbufp
	)
{
	u_int	ifidx;
	endpt *	la;

	UNUSED_ARG(rbufp);

	/*
	 * loop over [0..sys_ifnum] searching ep_list for each
	 * ifnum in turn.
	 */
	for (ifidx = 0; ifidx < sys_ifnum; ifidx++) {
		for (la = ep_list; la != NULL; la = la->elink)
			if (ifidx == la->ifnum)
				break;
		if (NULL == la)
			continue;
		/* return stats for one local address */
		send_ifstats_entry(la, ifidx);
	}
	ctl_flushpkt(0);
}

static void
sockaddrs_from_restrict_u(
	sockaddr_u *	psaA,
	sockaddr_u *	psaM,
	restrict_u *	pres,
	int		ipv6
	)
{
	ZERO(*psaA);
	ZERO(*psaM);
	if (!ipv6) {
		SET_AF(psaA, AF_INET);
		PSOCK_ADDR4(psaA)->s_addr = htonl(pres->u.v4.addr);
		SET_AF(psaM, AF_INET);
		PSOCK_ADDR4(psaM)->s_addr = htonl(pres->u.v4.mask);
	} else {
		SET_AF(psaA, AF_INET6);
		memcpy(&SOCK_ADDR6(psaA), &pres->u.v6.addr,
		       sizeof(SOCK_ADDR6(psaA)));
		SET_AF(psaM, AF_INET6);
		memcpy(&SOCK_ADDR6(psaM), &pres->u.v6.mask,
		       sizeof(SOCK_ADDR6(psaA)));
	}
}


/*
 * Send a restrict entry in response to a "ntpq -c reslist" request.
 *
 * To keep clients honest about not depending on the order of values,
 * and thereby avoid being locked into ugly workarounds to maintain
 * backward compatibility later as new fields are added to the response,
 * the order is random.
 */
static void
send_restrict_entry(
	restrict_u *	pres,
	int		ipv6,
	u_int		idx
	)
{
	const char addr_fmtu[] =	"addr.%u";
	const char mask_fmtu[] =	"mask.%u";
	const char hits_fmt[] =		"hits.%u";
	const char flags_fmt[] =	"flags.%u";
	char		tag[32];
	uint8_t		sent[RESLIST_FIELDS]; /* 4 tag=value pairs */
	int		noisebits;
	uint32_t		noise;
	u_int		which = 0;
	u_int		remaining;
	sockaddr_u	addr;
	sockaddr_u	mask;
	const char *	pch;
	char *		buf;
	const char *	match_str;
	const char *	access_str;

	sockaddrs_from_restrict_u(&addr, &mask, pres, ipv6);
	remaining = COUNTOF(sent);
	ZERO(sent);
	noise = 0;
	noisebits = 0;
	while (remaining > 0) {
		if (noisebits < 2) {
			noise = ntp_random();
			noisebits = 31;
		}
#ifdef USE_RANDOMIZE_RESPONSES
		which = (noise & 0x3) % COUNTOF(sent);
#endif /* USE_RANDOMIZE_RESPONSES */
		noise >>= 2;
		noisebits -= 2;

		while (sent[which])
			which = (which + 1) % COUNTOF(sent);

		switch (which) {

		case 0:
			snprintf(tag, sizeof(tag), addr_fmtu, idx);
			pch = socktoa(&addr);
			ctl_putunqstr(tag, pch, strlen(pch));
			break;

		case 1:
			snprintf(tag, sizeof(tag), mask_fmtu, idx);
			pch = socktoa(&mask);
			ctl_putunqstr(tag, pch, strlen(pch));
			break;

		case 2:
			snprintf(tag, sizeof(tag), hits_fmt, idx);
			ctl_putuint(tag, pres->count);
			break;

		case 3:
			snprintf(tag, sizeof(tag), flags_fmt, idx);
			match_str = res_match_flags(pres->mflags);
			access_str = res_access_flags(pres->flags);
			if ('\0' == match_str[0]) {
				pch = access_str;
			} else {
				LIB_GETBUF(buf);
				snprintf(buf, LIB_BUFLENGTH, "%s %s",
					 match_str, access_str);
				pch = buf;
			}
			ctl_putunqstr(tag, pch, strlen(pch));
			break;
		}
		sent[which] = true;
		remaining--;
	}
#ifdef USE_RANDOMIZE_RESPONSES
	send_random_tag_value((int)idx);
#endif /* USE_RANDOMIZE_RESPONSES */
}


static void
send_restrict_list(
	restrict_u *	pres,
	int		ipv6,
	u_int *		pidx
	)
{
	for ( ; pres != NULL; pres = pres->link) {
		send_restrict_entry(pres, ipv6, *pidx);
		(*pidx)++;
	}
}


/*
 * read_addr_restrictions - returns IPv4 and IPv6 access control lists
 */
static void
read_addr_restrictions(
	struct recvbuf *	rbufp
)
{
	u_int idx;

	UNUSED_ARG(rbufp);

	idx = 0;
	send_restrict_list(restrictlist4, false, &idx);
	send_restrict_list(restrictlist6, true, &idx);
	ctl_flushpkt(0);
}


/*
 * read_ordlist - CTL_OP_READ_ORDLIST_A for ntpq -c ifstats & reslist
 */
static void
read_ordlist(
	struct recvbuf *	rbufp,
	int			restrict_mask
	)
{
	const char ifstats_s[] = "ifstats";
	const size_t ifstatint8_ts = COUNTOF(ifstats_s) - 1;
	const char addr_rst_s[] = "addr_restrictions";
	const size_t a_r_chars = COUNTOF(addr_rst_s) - 1;
	struct ntp_control *	cpkt;
	u_short			qdata_octets;

	UNUSED_ARG(rbufp);
	UNUSED_ARG(restrict_mask);

	/*
	 * CTL_OP_READ_ORDLIST_A was first named CTL_OP_READ_IFSTATS and
	 * used only for ntpq -c ifstats.  With the addition of reslist
	 * the same opcode was generalized to retrieve ordered lists
	 * which require authentication.  The request data is empty or
	 * contains "ifstats" (not null terminated) to retrieve local
	 * addresses and associated stats.  It is "addr_restrictions"
	 * to retrieve the IPv4 then IPv6 remote address restrictions,
	 * which are access control lists.  Other request data return
	 * CERR_UNKNOWNVAR.
	 */
	cpkt = (struct ntp_control *)&rbufp->recv_pkt;
	qdata_octets = ntohs(cpkt->count);
	if (0 == qdata_octets || (ifstatint8_ts == qdata_octets &&
	    !memcmp(ifstats_s, cpkt->data, ifstatint8_ts))) {
		read_ifstats(rbufp);
		return;
	}
	if (a_r_chars == qdata_octets &&
	    !memcmp(addr_rst_s, cpkt->data, a_r_chars)) {
		read_addr_restrictions(rbufp);
		return;
	}
	ctl_error(CERR_UNKNOWNVAR);
}


/*
 * req_nonce - CTL_OP_REQ_NONCE for ntpq -c mrulist prerequisite.
 */
static void req_nonce(
	struct recvbuf *	rbufp,
	int			restrict_mask
	)
{
	char	buf[64];

	UNUSED_ARG(restrict_mask);

	generate_nonce(rbufp, buf, sizeof(buf));
	ctl_putunqstr("nonce", buf, strlen(buf));
	ctl_flushpkt(0);
}


/*
 * read_clockstatus - return clock radio status
 */
/*ARGSUSED*/
static void
read_clockstatus(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
#ifndef REFCLOCK
	UNUSED_ARG(rbufp);
	UNUSED_ARG(restrict_mask);
	/*
	 * If no refclock support, no data to return
	 */
	ctl_error(CERR_BADASSOC);
#else
	const struct ctl_var *	v;
	int			i;
	struct peer *		peer;
	char *			valuep;
	uint8_t *		wants;
	size_t			wants_alloc;
	bool			gotvar;
	const uint8_t *		cc;
	struct ctl_var *	kv;
	struct refclockstat	cs;

	UNUSED_ARG(rbufp);
	UNUSED_ARG(restrict_mask);

	if (res_associd != 0) {
		peer = findpeerbyassoc(res_associd);
	} else {
		/*
		 * Find a clock for this jerk.	If the system peer
		 * is a clock use it, else search peer_list for one.
		 */
		if (sys_peer != NULL && (FLAG_REFCLOCK &
		    sys_peer->flags))
			peer = sys_peer;
		else
			for (peer = peer_list;
			     peer != NULL;
			     peer = peer->p_link)
				if (FLAG_REFCLOCK & peer->flags)
					break;
	}
	if (NULL == peer || !(FLAG_REFCLOCK & peer->flags)) {
		ctl_error(CERR_BADASSOC);
		return;
	}
	/*
	 * If we got here we have a peer which is a clock. Get his
	 * status.
	 */
	cs.kv_list = NULL;
	refclock_control(&peer->srcadr, NULL, &cs);
	kv = cs.kv_list;
	/*
	 * Look for variables in the packet.
	 */
	rpkt.status = htons(ctlclkstatus(&cs));
	wants_alloc = CC_MAXCODE + 1 + count_var(kv);
	wants = emalloc_zero(wants_alloc);
	gotvar = false;
	while (NULL != (v = ctl_getitem(clock_var, &valuep))) {
		if (!(EOV & v->flags)) {
			wants[v->code] = true;
			gotvar = true;
		} else {
			v = ctl_getitem(kv, &valuep);
			if (NULL == v) {
				ctl_error(CERR_BADVALUE);
				free(wants);
				free_varlist(cs.kv_list);
				return;
			}

			if (EOV & v->flags) {
				ctl_error(CERR_UNKNOWNVAR);
				free(wants);
				free_varlist(cs.kv_list);
				return;
			}
			wants[CC_MAXCODE + 1 + v->code] = true;
			gotvar = true;
		}
	}

	if (gotvar) {
		for (i = 1; i <= CC_MAXCODE; i++)
			if (wants[i])
				ctl_putclock(i, &cs, true);
		if (kv != NULL)
			for (i = 0; !(EOV & kv[i].flags); i++)
				if (wants[i + CC_MAXCODE + 1])
					ctl_putdata(kv[i].text,
						    strlen(kv[i].text),
						    false);
	} else {
		for (cc = def_clock_var; *cc != 0; cc++)
			ctl_putclock((int)*cc, &cs, false);
		for ( ; kv != NULL && !(EOV & kv->flags); kv++)
			if (DEF & kv->flags)
				ctl_putdata(kv->text, strlen(kv->text),
					    false);
	}

	free(wants);
	free_varlist(cs.kv_list);

	ctl_flushpkt(0);
#endif
}


/*
 * write_clockstatus - we don't do this
 */
/*ARGSUSED*/
static void
write_clockstatus(
	struct recvbuf *rbufp,
	int restrict_mask
	)
{
	UNUSED_ARG(rbufp);
	UNUSED_ARG(restrict_mask);

	ctl_error(CERR_PERMISSION);
}

/*
 * report_event - report an event to log files
 *
 * Code lives here because in past times it reported through the
 * obsolete trap facility.
 */
void
report_event(
	int	err,		/* error code */
	struct peer *peer,	/* peer structure pointer */
	const char *str		/* protostats string */
	)
{
	char	statstr[NTP_MAXSTRLEN];
	size_t	len;

	/*
	 * Report the error to the protostats file and system log
	 */
	if (peer == NULL) {

		/*
		 * Discard a system report if the number of reports of
		 * the same type exceeds the maximum.
		 */
		if (ctl_sys_last_event != (uint8_t)err)
			ctl_sys_num_events= 0;
		if (ctl_sys_num_events >= CTL_SYS_MAXEVENTS)
			return;

		ctl_sys_last_event = (uint8_t)err;
		ctl_sys_num_events++;
		snprintf(statstr, sizeof(statstr),
		    "0.0.0.0 %04x %02x %s",
		    ctlsysstatus(), err, eventstr(err));
		if (str != NULL) {
			len = strlen(statstr);
			snprintf(statstr + len, sizeof(statstr) - len,
			    " %s", str);
		}
		NLOG(NLOG_SYSEVENT)
			msyslog(LOG_INFO, "%s", statstr);
	} else {

		/*
		 * Discard a peer report if the number of reports of
		 * the same type exceeds the maximum for that peer.
		 */
		const char *	src;
		uint8_t		errlast;

		errlast = (uint8_t)err & ~PEER_EVENT;
		if (peer->last_event != errlast)
			peer->num_events = 0;
		if (peer->num_events >= CTL_PEER_MAXEVENTS)
			return;

		peer->last_event = errlast;
		peer->num_events++;
#ifdef REFCLOCK
		if (IS_PEER_REFCLOCK(peer))
			src = refclock_name(peer);
		else
#endif /* REFCLOCK */ 
		    src = socktoa(&peer->srcadr);

		snprintf(statstr, sizeof(statstr),
		    "%s %04x %02x %s", src,
		    ctlpeerstatus(peer), err, eventstr(err));
		if (str != NULL) {
			len = strlen(statstr);
			snprintf(statstr + len, sizeof(statstr) - len,
			    " %s", str);
		}
		NLOG(NLOG_PEEREVENT)
			msyslog(LOG_INFO, "%s", statstr);
	}
	record_proto_stats(statstr);
#if defined(DEBUG) && DEBUG
	if (debug)
		printf("event at %lu %s\n", current_time, statstr);
#endif

}


/*
 * mprintf_event - printf-style varargs variant of report_event()
 */
int
mprintf_event(
	int		evcode,		/* event code */
	struct peer *	p,		/* may be NULL */
	const char *	fmt,		/* msnprintf format */
	...
	)
{
	va_list	ap;
	int	rc;
	char	msg[512];

	va_start(ap, fmt);
	rc = mvsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	report_event(evcode, p, msg);

	return rc;
}


/*
 * ctl_clr_stats - clear stat counters
 */
void
ctl_clr_stats(void)
{
	ctltimereset = current_time;
	numctlreq = 0;
	numctlbadpkts = 0;
	numctlresponses = 0;
	numctlfrags = 0;
	numctlerrors = 0;
	numctlfrags = 0;
	numctltooshort = 0;
	numctlinputresp = 0;
	numctlinputfrag = 0;
	numctlinputerr = 0;
	numctlbadoffset = 0;
	numctlbadversion = 0;
	numctldatatooshort = 0;
	numctlbadop = 0;
	numasyncmsgs = 0;
}

static u_short
count_var(
	const struct ctl_var *k
	)
{
	u_int c;

	if (NULL == k)
		return 0;

	c = 0;
	while (!(EOV & (k++)->flags))
		c++;

	NTP_ENSURE(c <= USHRT_MAX);
	return (u_short)c;
}


char *
add_var(
	struct ctl_var **kv,
	u_long size,
	u_short def
	)
{
	u_short		c;
	struct ctl_var *k;
	char *		buf;

	c = count_var(*kv);
	*kv  = erealloc(*kv, (c + 2) * sizeof(**kv));
	k = *kv;
	buf = emalloc(size);
	k[c].code  = c;
	k[c].text  = buf;
	k[c].flags = def;
	k[c + 1].code  = 0;
	k[c + 1].text  = NULL;
	k[c + 1].flags = EOV;

	return buf;
}


void
set_var(
	struct ctl_var **kv,
	const char *data,
	u_long size,
	u_short def
	)
{
	struct ctl_var *k;
	const char *s;
	const char *t;
	char *td;

	if (NULL == data || !size)
		return;

	k = *kv;
	if (k != NULL) {
		while (!(EOV & k->flags)) {
			if (NULL == k->text)	{
				td = emalloc(size);
				memcpy(td, data, size);
				k->text = td;
				k->flags = def;
				return;
			} else {
				s = data;
				t = k->text;
				while (*t != '=' && *s == *t) {
					s++;
					t++;
				}
				if (*s == *t && ((*t == '=') || !*t)) {
					td = erealloc((void *)(intptr_t)k->text, size);
					memcpy(td, data, size);
					k->text = td;
					k->flags = def;
					return;
				}
			}
			k++;
		}
	}
	td = add_var(kv, size, def);
	memcpy(td, data, size);
}


void
set_sys_var(
	const char *data,
	u_long size,
	u_short def
	)
{
	set_var(&ext_sys_var, data, size, def);
}


/*
 * get_ext_sys_var() retrieves the value of a user-defined variable or
 * NULL if the variable has not been setvar'd.
 */
const char *
get_ext_sys_var(const char *tag)
{
	struct ctl_var *	v;
	size_t			c;
	const char *		val;

	val = NULL;
	c = strlen(tag);
	for (v = ext_sys_var; !(EOV & v->flags); v++) {
		if (NULL != v->text && !memcmp(tag, v->text, c)) {
			if ('=' == v->text[c]) {
				val = v->text + c + 1;
				break;
			} else if ('\0' == v->text[c]) {
				val = "";
				break;
			}
		}
	}

	return val;
}


void
free_varlist(
	struct ctl_var *kv
	)
{
	struct ctl_var *k;
	if (kv) {
		for (k = kv; !(k->flags & EOV); k++)
			free((void *)(intptr_t)k->text);
		free((void *)kv);
	}
}


/* from ntp_request.c when ntpdc was nuked */

/*
 *  * A hack.  To keep the authentication module clear of ntp-ism's, we
 *   * include a time reset variable for its stats here.
 *    */
u_long auth_timereset;

/*
 *  * reset_auth_stats - reset the authentication stat counters.  Done here
 *   *                    to keep ntp-isms out of the authentication module
 *    */
void
reset_auth_stats(void)
{
        authkeylookups = 0;
        authkeynotfound = 0;
        authencryptions = 0;
        authdecryptions = 0;
        authkeyuncached = 0;
        auth_timereset = current_time;
}

