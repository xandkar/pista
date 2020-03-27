#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "khlib_log.h"
#include "khlib_time.h"

struct timespec
khlib_timespec_of_float(double n)
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
khlib_sleep(struct timespec *t)
{
	struct timespec remainder;

	if (nanosleep(t, &remainder) < 0) {
		if (errno == EINTR) {
			khlib_warn(
			    "nanosleep interrupted. Remainder: "
			    "{ tv_sec = %ld, tv_nsec = %ld }",
			    remainder.tv_sec, remainder.tv_nsec);
			/* No big deal if we occasionally sleep less,
			 * so not attempting to correct after an interruption.
			 */
		} else {
			khlib_fatal("nanosleep: %s\n", strerror(errno));
		}
	}
}
