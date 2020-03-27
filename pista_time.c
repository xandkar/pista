#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pista_log.h"
#include "pista_time.h"

struct timespec
pista_timespec_of_float(double n)
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
pista_sleep(struct timespec *t)
{
	struct timespec remainder;

	if (nanosleep(t, &remainder) < 0) {
		if (errno == EINTR) {
			pista_warn(
			    "nanosleep interrupted. Remainder: "
			    "{ tv_sec = %ld, tv_nsec = %ld }",
			    remainder.tv_sec, remainder.tv_nsec);
			/* No big deal if we occasionally sleep less,
			 * so not attempting to correct after an interruption.
			 */
		} else {
			pista_fatal("nanosleep: %s\n", strerror(errno));
		}
	}
}
