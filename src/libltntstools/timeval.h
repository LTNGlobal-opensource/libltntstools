/* Copyright LTN Global, Inc. 2024. All Rights Reserved. */

#ifndef LTN_TIMEVAL_H
#define LTN_TIMEVAL_H

#include <stdint.h>
#include <sys/time.h>

static inline int64_t ltn_timeval_to_ms(struct timeval *tv)
{
	int64_t sec = tv->tv_sec, usec = tv->tv_usec;
	return (sec * 1000) + (usec / 1000);
}

static inline int64_t ltn_timeval_to_us(struct timeval *tv)
{
	int64_t sec = tv->tv_sec, usec = tv->tv_usec;
	return (sec * 1000000) + usec;
}

static inline int64_t ltn_timeval_subtract_ms(struct timeval *x, struct timeval *y)
{
	return ltn_timeval_to_ms(x) - ltn_timeval_to_ms(y);
}

static inline int64_t ltn_timeval_subtract_us(struct timeval *x, struct timeval *y)
{
	return ltn_timeval_to_us(x) - ltn_timeval_to_us(y);
}


#endif /* LTN_TIMEVAL_H */
