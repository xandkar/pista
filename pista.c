#include <sys/select.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>

#include "arg.h"
#include "bsdtimespec.h"


#define debug(...) \
	do {if (log_level >= DEBUG) { \
		fprintf(stderr, "[debug] " __VA_ARGS__); \
		fflush(stderr); \
	}} while (0)
#define info(...) \
	do {if (log_level >= INFO ) { \
		fprintf(stderr, "[info] "  __VA_ARGS__); \
		fflush(stderr); \
	}} while (0)
#define warn(...) \
	do {if (log_level >= WARN ) { \
		fprintf(stderr, "[warn] "  __VA_ARGS__); \
		fflush(stderr); \
	}} while (0)
#define error(...) \
	do {if (log_level >= ERROR) { \
		fprintf(stderr, "[error] " __VA_ARGS__); \
		fflush(stderr); \
	}} while (0)
#define fatal(...) \
	do { \
		fprintf(stderr, "[fatal] " __VA_ARGS__); \
		exit(EXIT_FAILURE); \
	} while (0)
#define usage(...) \
	do { \
		print_usage(); \
		fprintf(stderr, "Error:\n    " __VA_ARGS__); \
		exit(EXIT_FAILURE); \
	} while (0)

#define ERRMSG "ERROR"


/* TODO: Convert slot list to slot array. */
typedef struct Slot Slot;
struct Slot {
	char            *in_fifo;
	int              in_fd;
	struct timespec  in_last_read;
	struct timespec  out_ttl;
	int              out_width;
	int              out_pos_lo;   /* Lowest  position on the output buffer. */
	int              out_pos_cur;  /* Current position on the output buffer. */
	int              out_pos_hi;   /* Highest position on the output buffer. */
	Slot            *next;
};

typedef struct Config Config;
struct Config {
	double interval;
	char  *left_pad;
	char  *separator;
	char  *right_pad;
	char   expiry_character;
	Slot  *slots;
	int    slot_count;
	int    buf_width;
	int    to_x_root;
};

enum read_status {
	END_OF_FILE,
	END_OF_MESSAGE,
	RETRY,
	FAILURE
};

enum log_level {
	NOTHING,
	ERROR,
	WARN,
	INFO,
	DEBUG
};


char *argv0 = NULL;  /* set by arg.h */
static int running = 1;
static int exit_code = EXIT_SUCCESS;
static enum log_level log_level = ERROR;
static const char errmsg[] = ERRMSG;
static const int  errlen   = sizeof(ERRMSG) - 1;


static struct timespec
timespec_of_float(const double n)
{
	double integral;
	double fractional;
	struct timespec t;

	fractional = modf(n, &integral);
	t.tv_sec = (int) integral;
	t.tv_nsec = (int) (1E9 * fractional);

	return t;
}

static char *
buf_create(Config *cfg)
{
	int lpadlen = strlen(cfg->left_pad);
	int seplen  = strlen(cfg->separator);
	int rpadlen = strlen(cfg->right_pad);
	int nslots  = 0;
	char *buf;
	Slot *s;

	buf = calloc(1, cfg->buf_width + 1);
	if (buf == NULL)
		fatal(
		    "[memory] Failed to allocate buffer of %d bytes",
		    cfg->buf_width
		);

	memset(buf, ' ', cfg->buf_width);
	buf[cfg->buf_width] = '\0';

	/* Set left pad */
	strncpy(buf, cfg->left_pad, lpadlen);

	/* Set the separators */
	for (s = cfg->slots; s; s = s->next) {
		/* Skip the first, left-most */
		if (nslots++) {
			/* Copying only seplen ensures we omit the '\0' byte. */
			strncpy(
			    buf + (s->out_pos_lo - seplen),
			    cfg->separator,
			    seplen
			);
		}
	}

	/* Set right pad */
	strncpy(buf + (cfg->buf_width - rpadlen), cfg->right_pad, rpadlen);

	return buf;
}

static void
slot_create(Config *c, const char *fifo0, const unsigned int width, const float ttl)
{
	Slot *s;
	struct timespec in_last_read;
	char *fifo1;
	const int fifo_len = strlen(fifo0) + 1;

	in_last_read.tv_sec  = 0;
	in_last_read.tv_nsec = 0;
	s = calloc(1, sizeof(struct Slot));
	fifo1 = calloc(fifo_len, sizeof(char));

	if (s && fifo1) {
		strncpy(fifo1, fifo0, fifo_len);
		s->in_fifo      = fifo1;
		s->in_fd        = -1;
		s->out_width    = width;
		s->out_ttl      = timespec_of_float(ttl);
		s->in_last_read = in_last_read;
		s->out_pos_lo   = c->buf_width;
		s->out_pos_cur  = s->out_pos_lo;
		s->out_pos_hi   = s->out_pos_lo + s->out_width - 1;
		s->next		= c->slots;

		c->slots        = s;
		c->buf_width += s->out_width;
		c->slot_count++;
	} else {
		fatal("[memory] Failed to allocate slot \"%s\"\n", fifo0);
	}
}

static Slot *
slots_rev(Slot *old)
{
	Slot *tmp = NULL;
	Slot *new = NULL;

	while (old) {
		tmp       = old->next;
		old->next = new;
		new       = old;
		old       = tmp;
	}
	return new;
}

static void
slot_log(Slot *s)
{
	info("Slot "
	    "{"
	    " in_fifo = %s,"
	    " in_fd = %d,"
	    " out_width = %d,"
	    " in_last_read = {tv_sec = %ld, tv_nsec = %ld}"
	    " out_ttl = {tv_sec = %ld, tv_nsec = %ld},"
	    " out_pos_lo = %d,"
	    " out_pos_cur = %d,"
	    " out_pos_hi = %d,"
	    " next = %p,"
	    " }\n",
	    s->in_fifo,
	    s->in_fd,
	    s->out_width,
	    s->in_last_read.tv_sec,
	    s->in_last_read.tv_nsec,
	    s->out_ttl.tv_sec,
	    s->out_ttl.tv_nsec,
	    s->out_pos_lo,
	    s->out_pos_cur,
	    s->out_pos_hi,
	    s->next
	);
}

static void
slots_log(Slot *head)
{
	Slot *s = head;

	for (; s; s = s->next) {
		slot_log(s);
	}
}

static void
slots_assert_fifos_exist(Slot *s)
{
	struct stat st;
	int errors = 0;

	for (; s; s = s->next) {
		if (lstat(s->in_fifo, &st) < 0) {
			error(
			    "Cannot stat \"%s\". Error: %s\n",
			    s->in_fifo,
			    strerror(errno)
			);
			errors++;
			continue;
		}
		if (!(st.st_mode & S_IFIFO)) {
			error("\"%s\" is not a FIFO\n", s->in_fifo);
			errors++;
			continue;
		}
	}
	if (errors)
		fatal(
		    "Encountered errors with given file paths. See log.\n"
		);
}

static void
slot_close(Slot *s)
{
	close(s->in_fd);
	s->in_fd        = -1;
	s->out_pos_cur  = s->out_pos_lo;
}

static void
slots_close(Slot *s)
{
	for (; s; s = s->next)
		if (s->in_fd > -1)
			slot_close(s);
}


static int
slot_expire(
	const Slot *s,
	const struct timespec t,
	const char expiry_character,
	char *buf
)
{
	struct timespec td;

	timespecsub(&t, &(s->in_last_read), &td);
	if (
		s->out_ttl.tv_sec >= 0  /* Negative == infinity */
		&&
		timespeccmp(&td, &(s->out_ttl), >=)
	) {
		memset(
		    buf + s->out_pos_lo,
		    expiry_character,
		    s->out_width
		);
		debug("Slot expired: \"%s\"\n", s->in_fifo);
		return 1;
	}
	return 0;
}

static void
slot_set_error(Slot *s, char *buf)
{
	char *b;
	int i;

	s->in_fd = -1;
	b = buf + s->out_pos_lo;
	/* Copy as much of the error message as possible.
	 * EXCLUDING the terminating \0. */
	for (i = 0; i < errlen && i < s->out_width; i++)
		b[i] = errmsg[i];
	/* Any remaining positions: */
	memset(b + i, '_', s->out_width - i);
}

static enum read_status
slot_read(Slot *s, char *buf)
{
	char c;  /* Character read. */
	int  r;  /* Remaining unused positions in buffer range. */

	for (;;) {
		switch (read(s->in_fd, &c, 1)) {
		case -1:
			error(
			    "Failed to read: \"%s\". errno: %d, msg: %s\n",
			    s->in_fifo,
			    errno,
			    strerror(errno)
			);
			switch (errno) {
			case EINTR:
			case EAGAIN:
				return RETRY;
			default:
				return FAILURE;
			}
		case  0:
			debug("%s: End of FILE\n", s->in_fifo);
			s->out_pos_cur = s->out_pos_lo;
			return END_OF_FILE;
		case  1:
			/* TODO: Consider making msg term char a CLI option */
			if (c == '\n' || c == '\0') {
				r = (s->out_pos_hi - s->out_pos_cur) + 1;
				if (r > 0)
					memset(buf + s->out_pos_cur, ' ', r);
				return END_OF_MESSAGE;
			} else {
				if (s->out_pos_cur <= s->out_pos_hi)
					buf[s->out_pos_cur++] = c;
				else
					/*
					 * Force EOM beyond available range.
					 * To ensure that a rogue large message
					 * doesn't trap us here needlessly
					 * long.
					 */
					return END_OF_MESSAGE;
			}
			break;
		default:
			assert(0);
		}
	}
}

static int
slots_read(const Config *cfg, const struct timespec *timeout, char *buf)
{
	fd_set fds;
	int maxfd = -1;
	int ready = 0;
	int updated = 0;
	struct stat st;
	struct timespec t;
	Slot *s;

	FD_ZERO(&fds);
	for (s = cfg->slots; s; s = s->next) {
		/* TODO: Create the FIFO if it doesn't already exist. */
		if (lstat(s->in_fifo, &st) < 0) {
			error(
			    "Cannot stat \"%s\". Error: %s\n",
			    s->in_fifo,
			    strerror(errno)
			);
			slot_set_error(s, buf);
			continue;
		}
		if (!(st.st_mode & S_IFIFO)) {
			error("\"%s\" is not a FIFO\n", s->in_fifo);
			slot_set_error(s, buf);
			continue;
		}
		if (s->in_fd < 0) {
			debug(
			    "%s: closed. opening. in_fd: %d\n",
			    s->in_fifo,
			    s->in_fd
			);
			s->in_fd = open(s->in_fifo, O_RDONLY | O_NONBLOCK);
		} else {
			debug(
			    "%s: already openned. in_fd: %d\n",
			    s->in_fifo,
			    s->in_fd
			);
		}
		if (s->in_fd == -1) {
			/* TODO Consider backing off retries for failed slots */
			error("Failed to open \"%s\"\n", s->in_fifo);
			slot_set_error(s, buf);
			continue;
		}
		debug("%s: open. in_fd: %d\n", s->in_fifo, s->in_fd);
		if (s->in_fd > maxfd)
			maxfd = s->in_fd;
		FD_SET(s->in_fd, &fds);
	}
	debug("selecting...\n");
	ready = pselect(maxfd + 1, &fds, NULL, NULL, timeout, NULL);
	debug("ready: %d\n", ready);
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (ready == -1) {
		switch (errno) {
		case EINTR:
			error(
			    "pselect interrupted: %d, errno: %d, msg: %s\n",
			    ready,
			    errno,
			    strerror(errno)
			);
			/* TODO: Reconsider what to do here. */
			return updated;
		default:
			fatal(
			    "pselect failed: %d, errno: %d, msg: %s\n",
			    ready,
			    errno,
			    strerror(errno)
			);
		}
	}
	/* At-least-once ensures that expiries are still checked on timeouts. */
	do {
		for (s = cfg->slots; s; s = s->next) {
			if (s->in_fd < 0)
				continue;
			if (FD_ISSET(s->in_fd, &fds)) {
				updated++;
				debug("reading: %s\n", s->in_fifo);
				switch (slot_read(s, buf)) {
				/*
				 * When to yield a pipe read?
				 * When to close the pipe?
				 * ============================================
				 * M : EOM : End Of Message = LF or max msg length
				 * F : EOF : End Of File
				 *
				 * Breadth first:
				 * - yield @ M
				 * - close @ M
				 * --------------------------------------------
				 * PRO: Inter-message pushback.
				 *      (pipes block at "open" call)
				 * CON: Message loss. Clients have to retry.
				 *      Unread messages remain in the pipe and
				 *      are dropped, which maybe an acceptable
				 *      trade-off given that we only care about
				 *      the latest state.
				 *
				 * >>> CURRENT <<<
				 * Breadth first:
				 * - yield @ M
				 * - close @ F
				 * --------------------------------------------
				 * PRO: Lossless. Yield after each msg.
				 * CON: A fast writer can fill the pipe faster
				 *      than we can read it and we end-up
				 *      displaying stale data.
				 *
				 * Depth first:
				 * - yield @ F
				 * - close @ F
				 * --------------------------------------------
				 * PRO: Lossless. Fastest.
				 * CON: Blocks the loop. Fast writer can trap us.
				 *
				 * Limited-depth first:
				 * - yield @ F after a limit (N msgs or bytes)
				 * - close @ F
				 * --------------------------------------------
				 * PRO: Lossless. Best of all of the above worlds?
				 * CON: ?
				 *
				 */
				case END_OF_MESSAGE:
					s->out_pos_cur = s->out_pos_lo;
					s->in_last_read = t;
					ready--;
					break;
				case END_OF_FILE:
				case FAILURE:
					slot_close(s);
					ready--;
					break;
				case RETRY:
					break;
				default:
					assert(0);
				}
			} else {
				updated += slot_expire(
				    s,t, cfg->expiry_character, buf
				);
			}
		}
	} while (ready);
	assert(ready == 0);
	return updated;
}

static void
config_log(const Config *cfg)
{
	info(
	    "Config "
	    "{"
	    " interval = %f,"
	    " separator = %s,"
	    " slot_count = %d,"
	    " buf_width = %d,"
	    " slots = ..."
	    " }\n",
	    cfg->interval,
	    cfg->separator,
	    cfg->slot_count,
	    cfg->buf_width
	);
	slots_log(cfg->slots);
}

static void
config_stretch_for_separators(Config *cfg)
{
	int lpadlen = strlen(cfg->left_pad);
	int seplen  = strlen(cfg->separator);
	int rpadlen = strlen(cfg->right_pad);
	int prefix  = lpadlen;
	int nslots  = 0;
	Slot *s     = cfg->slots;

	while (s) {
		s->out_pos_lo  += prefix;
		s->out_pos_hi  += prefix;
		s->out_pos_cur  = s->out_pos_lo;
		prefix         += seplen;
		nslots++;
		s = s->next;
	}
	cfg->buf_width += lpadlen + (seplen * (nslots - 1)) + rpadlen;
}

static int
is_pos_num(const char *str)
{
	while (*str != '\0')
		if (!isdigit(*(str++)))
			return 0;
	return 1;
}

static int
is_decimal(const char *str)
{
	char c;
	int seen = 0;

	if (*str == '-')
		str++;

	while ((c = *(str++)) != '\0')
		if (!isdigit(c)) {
			if (c == '.' && !seen++)
				continue;
			else
				return 0;
		}
	return 1;
}

static void
print_usage()
{
	assert(argv0);
	fprintf(
	    stderr,
	    "\n"
	    "Usage: %s [OPTION ...] SPEC [SPEC ...]\n"
	    "\n"
	    "  SPEC       = FILE_PATH DATA_WIDTH DATA_TTL\n"
	    "  FILE_PATH  = string\n"
	    "  DATA_WIDTH = int  (* (positive) number of characters *)\n"
	    "  DATA_TTL   = float  (* (positive) number of seconds *)\n"
	    "  OPTION     = -i INTERVAL (* Max IO wait before yielding"
			    "to check slot expirations. "
			    "Must be greater than 0 *)\n"
	    "             | -s SEPARATOR\n"
	    "             | -x (* Output to X root window *)\n"
	    "             | -l LOG_LEVEL\n"
	    "             | -e EXPIRY_CHARACTER\n"
	    "  SEPARATOR  = string\n"
	    "  INTERVAL   = float  (* (positive) number of seconds *)\n"
	    "  LOG_LEVEL  = int  (* %d through %d *)\n"
	    "  EXPIRY_CHARACTER = string  "
	    "(* Character with which to fill the slot upon expiration. *)\n"
	    "\n",
	    argv0,
	    NOTHING,
	    DEBUG
	);
	fprintf(
	    stderr,
	    "Example: %s -i 1 /dev/shm/pista/pista_sensor_x 4 10\n"
	    "\n",
	    argv0
	);
}

static void
loop(const Config *cfg, char *buf, Display *d)
{
	struct timespec timeout;

	timeout = timespec_of_float(cfg->interval);
	while (running) {
		// Only bother outputting if something was updated.
		if (slots_read(cfg, &timeout, buf) > 0) {
			if (cfg->to_x_root) {
				if (XStoreName(d, DefaultRootWindow(d), buf) < 0)
					fatal("XStoreName failed.\n");
				XFlush(d);
			} else {
				puts(buf);
				fflush(stdout);
			}
		}
	}
}

static void
terminate(const int s)
{
	warn("Terminating due to signal: %d\n", s);
	running = 0;
	exit_code = EXIT_FAILURE;
}

int
main(int argc, char *argv[])
{
	Config cfg = {
		.interval    = 1.0,
		.left_pad    = " ",
		.separator   = " ",
		.right_pad   = " ",
		.expiry_character = '_',
		.slots       = NULL,
		.slot_count  = 0,
		.buf_width   = 0,
		.to_x_root   = 0,
	};
	int i;
	char *tmpstr;
	int   tmpint;
	char *fifo;
	char *width;
	char *ttl;
	char *buf;
	Display *d = NULL;
	struct sigaction sa;

	ARGBEGIN {
	case 'i':
		tmpstr = EARGF(print_usage());
		if (!is_decimal(tmpstr))
			usage("Option -i parameter invalid: \"%s\"\n", tmpstr);
		tmpint = atof(tmpstr);
		if (tmpint <= 0)
			usage("Interval must be greater than 0!\n");
		cfg.interval = atof(tmpstr);
		break;
	case 'f':  /* left padding. 'l' is taken by log_level */
		tmpstr = EARGF(print_usage());
		tmpint = strlen(tmpstr) + 1;
		cfg.left_pad = calloc(tmpint, sizeof(char));
		strncpy(cfg.left_pad, tmpstr, tmpint);
		break;
	case 's':  /* middle separator */
		tmpstr = EARGF(print_usage());
		tmpint = strlen(tmpstr) + 1;
		cfg.separator = calloc(tmpint, sizeof(char));
		strncpy(cfg.separator, tmpstr, tmpint);
		break;
	case 'r':  /* right padding */
		tmpstr = EARGF(print_usage());
		tmpint = strlen(tmpstr) + 1;
		cfg.right_pad = calloc(tmpint, sizeof(char));
		strncpy(cfg.right_pad, tmpstr, tmpint);
		break;
	case 'x':
		cfg.to_x_root = 1;
		break;
	case 'l':
		tmpstr = EARGF(print_usage());
		if (!is_pos_num(tmpstr))
			usage("Option -l parameter invalid: \"%s\"\n", tmpstr);
		tmpint = atoi(tmpstr);
		if (tmpint > DEBUG)
			usage(
			    "Option -l value (%d) exceeds maximum (%d)\n",
			    tmpint,
			    DEBUG
			);
		log_level = tmpint;
		break;
	case 'e':
		cfg.expiry_character = EARGF(print_usage())[0];
		break;
	default:
		print_usage();
		exit(EXIT_FAILURE);
	} ARGEND

	for (i = 0; i < argc; ) {
		if ((i + 3) > argc)
			usage(
			    "[spec] Parameter(s) missing "
			    "for fifo \"%s\".\n",
			    argv[i]
			);
		fifo  = argv[i++];
		width = argv[i++];
		ttl   = argv[i++];
		if (!is_pos_num(width))
			usage("[spec] Invalid width: \"%s\", "
				"for fifo \"%s\"\n", width, fifo);
		if (!is_decimal(ttl))
			usage("[spec] Invalid TTL: \"%s\", "
				"for fifo \"%s\"\n", ttl, fifo);
		slot_create(&cfg, fifo, atoi(width), atof(ttl));
	}
	if (cfg.slots == NULL)
		usage("No slot specs were given!\n");

	cfg.slots = slots_rev(cfg.slots);
	config_log(&cfg);

	slots_assert_fifos_exist(cfg.slots);
	config_stretch_for_separators(&cfg);
	buf = buf_create(&cfg);
	if (cfg.to_x_root && !(d = XOpenDisplay(NULL)))
		fatal("XOpenDisplay failed with: %p\n", d);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = terminate;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT , &sa, NULL);

	loop(&cfg, buf, d);
	slots_close(cfg.slots);
	return exit_code;
}
