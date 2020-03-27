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

#include "bsdtimespec.h"


#define debug(...) \
	if (log_level >= Debug) { \
		fprintf(stderr, "[debug] " __VA_ARGS__); \
		fflush(stderr); \
	}
#define info(...) \
	if (log_level >= Info ) { \
		fprintf(stderr, "[info] "  __VA_ARGS__); \
		fflush(stderr); \
	}
#define warn(...) \
	if (log_level >= Warn ) { \
		fprintf(stderr, "[warn] "  __VA_ARGS__); \
		fflush(stderr); \
	}
#define error(...) \
	if (log_level >= Error) { \
		fprintf(stderr, "[error] " __VA_ARGS__); \
		fflush(stderr); \
	}
#define fatal(...) \
	{ \
		fprintf(stderr, "[fatal] " __VA_ARGS__); \
		exit(EXIT_FAILURE); \
	}
#define usage(...) \
	{ \
		print_usage(); \
		fprintf(stderr, "Error:\n    " __VA_ARGS__); \
		exit(EXIT_FAILURE); \
	}

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
	char * separator;
	char   expiry_character;
	Slot * slots;
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

enum LogLevel {
	Nothing,
	Error,
	Warn,
	Info,
	Debug
};


char *argv0 = NULL;
int running = 1;
enum LogLevel log_level = Error;
static const char errmsg[] = ERRMSG;
static const int  errlen   = sizeof(ERRMSG) - 1;


struct timespec
timespec_of_float(double n)
{
	double integral;
	double fractional;
	struct timespec t;

	fractional = modf(n, &integral);
	t.tv_sec = (int) integral;
	t.tv_nsec = (int) (1E9 * fractional);

	return t;
}

void
snooze(struct timespec *t)
{
	struct timespec remainder;

	if (nanosleep(t, &remainder) < 0) {
		if (errno == EINTR) {
			warn(
			    "nanosleep interrupted. Remainder: "
			    "{ tv_sec = %ld, tv_nsec = %ld }",
			    remainder.tv_sec, remainder.tv_nsec);
			/* No big deal if we occasionally sleep less,
			 * so not attempting to correct after an interruption.
			 */
		} else {
			fatal("nanosleep: %s\n", strerror(errno));
		}
	}
}

char *
buf_create(Config *cfg)
{
	int seplen;
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
	seplen = strlen(cfg->separator);
	/* Set the separators */
	for (s = cfg->slots; s; s = s->next) {
		/* Skip the first, left-most */
		if (s->out_pos_lo) {
			/* Copying only seplen ensures we omit the '\0' byte. */
			strncpy(
			    buf + (s->out_pos_lo - seplen),
			    cfg->separator,
			    seplen
			);
		}
	}
	return buf;
}

Slot *
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

void
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

void
slots_log(Slot *head)
{
	Slot *s = head;

	for (; s; s = s->next) {
		slot_log(s);
	}
}

void
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

void
slot_close(Slot *s)
{
	close(s->in_fd);
	s->in_fd        = -1;
	s->out_pos_cur  = s->out_pos_lo;
}

void
slots_close(Slot *s)
{
	for (; s; s = s->next)
		if (s->in_fd > -1)
			slot_close(s);
}


void
slot_expire(Slot *s, struct timespec t, char expiry_character, char *buf)
{
	struct timespec td;

	timespecsub(&t, &(s->in_last_read), &td);
	if (timespeccmp(&td, &(s->out_ttl), >=)) {
		memset(
		    buf + s->out_pos_lo,
		    expiry_character,
		    s->out_width
		);
		warn("Slot expired: \"%s\"\n", s->in_fifo);
	}
}

void
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

enum read_status
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

void
slots_read(Config *cfg, struct timespec *ti, char *buf)
{
	fd_set fds;
	int maxfd = -1;
	int ready = 0;
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
	ready = pselect(maxfd + 1, &fds, NULL, NULL, ti, NULL);
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
			return;
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
				debug("reading: %s\n", s->in_fifo);
				switch (slot_read(s, buf)) {
				/*
				 * ### MESSAGE LOSS ###
				 * is introduced by closing at EOM in addition
				 * to EOF, since there may be unread messages
				 * remaining in the pipe. However,
				 *
				 * ### INTER-MESSAGE PUSHBACK ###
				 * is also gained, since pipes block at the
				 * "open" call.
				 *
				 * This is an acceptable trade-off because we
				 * are a stateless reporter of a _most-recent_
				 * status, not a stateful accumulator.
				 *
				 * ### LOSSLESS ALTERNATIVES ###
				 * - Read each pipe until EOF before reading
				 *   another.
				 *   PROBLEM: a fast writer can trap us in the
				 *   read loop.
				 *
				 * - Read each pipe until EOM, but close only
				 *   at EOF.
				 *   PROBLEM: a fast writer can fill the pipe
				 *   faster than we can read it and we end-up
				 *   displaying stale data.
				 *
				 */
				case END_OF_MESSAGE:
				case END_OF_FILE:
				case FAILURE:
					slot_close(s);
					s->in_last_read = t;
					ready--;
					break;
				case RETRY:
					break;
				default:
					assert(0);
				}
			} else {
				slot_expire(s, t, cfg->expiry_character, buf);
			}
		}
	} while (ready);
	assert(ready == 0);
}

void
config_log(Config *cfg)
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

void
config_stretch_for_separators(Config *cfg)
{
	int seplen = strlen(cfg->separator);
	int prefix = 0;
	int nslots = 0;
	Slot *s    = cfg->slots;

	while (s) {
		s->out_pos_lo  += prefix;
		s->out_pos_hi  += prefix;
		s->out_pos_cur  = s->out_pos_lo;
		prefix         += seplen;
		nslots++;
		s = s->next;
	}
	cfg->buf_width += (seplen * (nslots - 1));
}

int
is_pos_num(char *str)
{
	while (*str != '\0')
		if (!isdigit(*(str++)))
			return 0;
	return 1;
}

int
is_decimal(char *str)
{
	char c;
	int seen = 0;

	while ((c = *(str++)) != '\0')
		if (!isdigit(c)) {
			if (c == '.' && !seen++)
				continue;
			else
				return 0;
		}
	return 1;
}

void
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
	    "  OPTION     = -i INTERVAL\n"
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
	    Nothing,
	    Debug
	);
	fprintf(
	    stderr,
	    "Example: %s -i 1 /dev/shm/pista/pista_sensor_x 4 10\n"
	    "\n",
	    argv0
	);
}

/* For mutually-recursive calls. */
void opts_parse_any(Config *, int, char *[], int);

void
parse_opts_opt_i(Config *cfg, int argc, char *argv[], int i)
{
	char *param;

	if (i >= argc)
		usage("Option -i parameter is missing.\n");
	param = argv[i++];
	if (!is_decimal(param))
		usage("Option -i parameter is invalid: \"%s\"\n", param);
	cfg->interval = atof(param);
	opts_parse_any(cfg, argc, argv, i);
}

void
parse_opts_opt_s(Config *cfg, int argc, char *argv[], int i)
{
	if (i >= argc)
		usage("Option -s parameter is missing.\n");
	cfg->separator = calloc((strlen(argv[i]) + 1), sizeof(char));
	strcpy(cfg->separator, argv[i]);
	opts_parse_any(cfg, argc, argv, ++i);
}

void
parse_opts_opt_l(Config *cfg, int argc, char *argv[], int i)
{
	char *param;

	if (i >= argc)
		usage("Option -l parameter is missing.\n");
	param = argv[i++];
	if (!is_pos_num(param))
		usage("Option -l parameter is invalid: \"%s\"\n", param);
	log_level = atoi(param);
	if (log_level > Debug)
		usage(
		    "Option -l value (%d) exceeds maximum (%d)\n",
		    log_level,
		    Debug
		);
	opts_parse_any(cfg, argc, argv, i);
}

void
parse_opts_opt_e(Config *cfg, int argc, char *argv[], int i)
{
	if (i >= argc)
		usage("Option -e parameter is missing.\n");
	cfg->expiry_character = argv[i++][0];
	opts_parse_any(cfg, argc, argv, i);
}

void
parse_opts_opt(Config *cfg, int argc, char *argv[], int i)
{
	switch (argv[i][1]) {
	case 'i':
		/* TODO: Generic set_int */
		parse_opts_opt_i(cfg, argc, argv, ++i);
		break;
	case 's':
		/* TODO: Generic set_str */
		parse_opts_opt_s(cfg, argc, argv, ++i);
		break;
	case 'x':
		cfg->to_x_root = 1;
		opts_parse_any(cfg, argc, argv, ++i);
		break;
	case 'l':
		/* TODO: Generic set_int */
		parse_opts_opt_l(cfg, argc, argv, ++i);
		break;
	case 'e':
		/* TODO: Generic set_str */
		parse_opts_opt_e(cfg, argc, argv, ++i);
		break;
	default :
		usage("Option \"%s\" is invalid\n", argv[i]);
	}
}

void
parse_opts_spec(Config *cfg, int argc, char *argv[], int i)
{
	char *n;
	char *w;
	char *t;
	struct timespec in_last_read;
	Slot *s;

	if ((i + 3) > argc)
		usage(
		    "[spec] Parameter(s) missing for fifo \"%s\".\n",
		    argv[i]
		);

	n = argv[i++];
	w = argv[i++];
	t = argv[i++];

	if (!is_pos_num(w))
		usage("[spec] Invalid width: \"%s\", for fifo \"%s\"\n", w, n);
	if (!is_decimal(t))
		usage("[spec] Invalid TTL: \"%s\", for fifo \"%s\"\n", t, n);

	in_last_read.tv_sec  = 0;
	in_last_read.tv_nsec = 0;
	s = calloc(1, sizeof(struct Slot));

	if (s) {
		s->in_fifo      = n;
		s->in_fd        = -1;
		s->out_width    = atoi(w);
		s->out_ttl      = timespec_of_float(atof(t));
		s->in_last_read = in_last_read;
		s->out_pos_lo   = cfg->buf_width;
		s->out_pos_cur  = s->out_pos_lo;
		s->out_pos_hi   = s->out_pos_lo + s->out_width - 1;
		s->next		= cfg->slots;

		cfg->slots        = s;
		cfg->buf_width += s->out_width;
		cfg->slot_count++;
	} else {
		fatal("[memory] Allocation failure.");
	}
	opts_parse_any(cfg, argc, argv, i);
}

void
opts_parse_any(Config *cfg, int argc, char *argv[], int i)
{
	if (i < argc) {
		switch (argv[i][0]) {
		case '-':
			parse_opts_opt(cfg, argc, argv, i);
			break;
		default :
			parse_opts_spec(cfg, argc, argv, i);
		}
	}
}

void
opts_parse(Config *cfg, int argc, char *argv[])
{
	opts_parse_any(cfg, argc, argv, 1);
	cfg->slots = slots_rev(cfg->slots);
	config_log(cfg);
	if (cfg->slots == NULL)
		usage("No slot specs were given!\n");
}

void
loop(Config *cfg, char *buf, Display *d)
{
	struct timespec
		t0,  /* time stamp. before reading slots */
		t1,  /* time stamp. after  reading slots */
		ti,  /* time interval desired    (t1 - t0) */
		td,  /* time interval measured   (t1 - t0) */
		tc;  /* time interval correction (ti - td) when td < ti */

	ti = timespec_of_float(cfg->interval);
	while (running) {
		clock_gettime(CLOCK_MONOTONIC, &t0); // FIXME: check errors
		slots_read(cfg, &ti, buf);
		if (cfg->to_x_root) {
			if (XStoreName(d, DefaultRootWindow(d), buf) < 0)
				fatal("XStoreName failed.\n");
			XFlush(d);
		} else {
			puts(buf);
			fflush(stdout);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1); // FIXME: check errors
		timespecsub(&t1, &t0, &td);
		debug(
		    "td {tv_sec = %ld, tv_nsec = %ld}\n",
		    td.tv_sec,
		    td.tv_nsec
		);
		if (timespeccmp(&td, &ti, <)) {
			/*
			 * Pushback on data producers by refusing to read the
			 * pipe more frequently than the interval.
			 */
			timespecsub(&ti, &td, &tc);
			snooze(&tc);
		}
	}
}

void
terminate(int s)
{
	debug("terminating due to signal %d\n", s);
	running = 0;
}

int
main(int argc, char *argv[])
{
	argv0 = argv[0];

	Config cfg = {
		.interval    = 1.0,
		.separator   = "|",
		.expiry_character = '_',
		.slots       = NULL,
		.slot_count  = 0,
		.buf_width   = 0,
		.to_x_root   = 0,
	};
	char *buf;
	Display *d = NULL;
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = terminate;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT , &sa, NULL);

	opts_parse(&cfg, argc, argv);
	slots_assert_fifos_exist(cfg.slots);
	config_stretch_for_separators(&cfg);
	buf = buf_create(&cfg);
	if (cfg.to_x_root && !(d = XOpenDisplay(NULL)))
		fatal("XOpenDisplay failed with: %p\n", d);
	loop(&cfg, buf, d);
	slots_close(cfg.slots);
	return EXIT_SUCCESS;
}
