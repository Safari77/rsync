/*
 * Reimplementations of standard functions for platforms that don't have them.
 *
 * Copyright (C) 1998 Andrew Tridgell
 * Copyright (C) 2002 Martin Pool
 * Copyright (C) 2004-2020 Wayne Davison
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, visit the http://fsf.org website.
 */

#include <math.h>
#include <float.h>

#include "rsync.h"
#include "itypes.h"

#ifndef isfinite
# ifdef HAVE_ISFINITE
#  define isfinite(x) isfinite(x)
# else
#  define isfinite(x) ((x) <= DBL_MAX && (x) >= -DBL_MAX)
# endif
#endif

static char number_separator;

char get_number_separator(void)
{
    if (!number_separator) {
        char buf[32];
        snprintf(buf, sizeof buf, "%f", 3.14);
        if (strchr(buf, '.') != NULL)
            number_separator = ',';
        else
            number_separator = '.';
    }

    return number_separator;
}

char get_decimal_point(void)
{
    return get_number_separator() == ',' ? '.' : ',';
}

#ifndef HAVE_GETCWD
#error getcwd needed
#endif

#ifndef HAVE_WAITPID
 pid_t waitpid(pid_t pid, int *statptr, int options)
{
#ifdef HAVE_WAIT4
    return wait4(pid, statptr, options, NULL);
#else
    /* If wait4 is also not available, try wait3 for SVR3 variants */
    /* Less ideal because can't actually request a specific pid */
    /* At least the WNOHANG option is supported */
    /* Code borrowed from apache fragment written by dwd@bell-labs.com */
    int tmp_pid, dummystat;
    if (pid > 0 && kill(pid, 0) == -1 && errno == ESRCH) {
        errno = ECHILD;
        return -1;
    }
    if (statptr == NULL)
        statptr = &dummystat;
    while (((tmp_pid = wait3(statptr, options, 0)) != pid) &&
            (tmp_pid != -1) && (tmp_pid != 0) && (pid != -1))
        ;
    return tmp_pid;
#endif
}
#endif


#ifndef HAVE_MEMMOVE
 void *memmove(void *dest, const void *src, size_t n)
{
    bcopy((char *) src, (char *) dest, n);
    return dest;
}
#endif

#ifndef HAVE_STRPBRK
/**
 * Find the first occurrence in @p s of any character in @p accept.
 *
 * Derived from glibc
 **/
 char *strpbrk(const char *s, const char *accept)
{
    while (*s != '\0')  {
        const char *a = accept;
        while (*a != '\0') {
            if (*a++ == *s)    return (char *)s;
        }
        ++s;
    }

    return NULL;
}
#endif


#ifndef HAVE_STRLCPY
/**
 * Like strncpy but does not 0 fill the buffer and always null
 * terminates.
 *
 * @param bufsize is the size of the destination buffer.
 *
 * @return length of the source string (strlen(s)).
 **/
 size_t strlcpy(char *d, const char *s, size_t bufsize)
{
    size_t len = strlen(s);
    size_t ret = len;
    if (bufsize > 0) {
        if (len >= bufsize)
            len = bufsize-1;
        memcpy(d, s, len);
        d[len] = 0;
    }
    return ret;
}
#endif

#ifndef HAVE_STRLCAT
/*
 * Appends src to string dst of size siz (unlike strncat, siz is the
 * full size of dst, not space left).  At most siz-1 characters
 * will be copied.  Always NUL terminates (unless siz <= strlen(dst)).
 * Returns strlen(src) + MIN(siz, strlen(initial dst)).
 * If retval >= siz, truncation occurred.
 */
 size_t strlcat(char *dst, const char *src, size_t siz)
{
    char *d = dst;
    const char *s = src;
    size_t n = siz;
    size_t dlen;

    /* Find the end of dst and adjust bytes left but don't go past end */
    while (n-- != 0 && *d != '\0')
        d++;
    dlen = d - dst;
    n = siz - dlen;

    if (n == 0)
        return dlen + strlen(s);
    while (*s != '\0') {
        if (n != 1) {
            *d++ = *s;
            n--;
        }
        s++;
    }
    *d = '\0';

    return dlen + (s - src);    /* count does not include NUL */
}
#endif

/* some systems don't take the 2nd argument */
int sys_gettimeofday(struct timeval *tv)
{
#ifdef HAVE_GETTIMEOFDAY_TZ
    return gettimeofday(tv, NULL);
#else
    return gettimeofday(tv);
#endif
}

/* Return the int64 number as a string.  If the human_flag arg is non-zero,
 * we may output the number in K, M, G, or T units.  If we don't add a unit
 * suffix, we will append the fract string, if it is non-NULL.  We can
 * return up to 4 buffers at a time. */
char *do_big_num(int64 num, int human_flag, const char *fract)
{
    static char bufs[4][128]; /* more than enough room */
    static unsigned int n;
    char *s;
    int len, negated;
    uint64_t abs_num;

    if (human_flag && !number_separator)
        (void)get_number_separator();

    n = (n + 1) % (sizeof bufs / sizeof bufs[0]);
    abs_num = num < 0 ? 0 - (uint64_t)num : (uint64_t)num;

    if (human_flag > 1) {
        unsigned int mult = human_flag == 2 ? 1000 : 1024;

        if (abs_num >= mult) {
            const char* units = " KMGTPE";
            uint64_t powi = 1;

            for (;;) {
                if (abs_num / mult < powi)
                    break;

                if (units[1] == '\0')
                    break;

                powi *= mult;
                ++units;
            }

            snprintf(bufs[n], sizeof bufs[0], "%.2f%c",
                (double) num / powi, *units);
            return bufs[n];
        }
    }

    s = bufs[n] + sizeof bufs[0] - 1;
    if (fract) {
        len = strlen(fract);
        if ((size_t)len > sizeof bufs[0] - 32)
            len = sizeof bufs[0] - 32;
        s -= len;
        strlcpy(s, fract, len + 1);
    } else
        *s = '\0';

    len = 0;

    if (!num)
        *--s = '0';
    if (num < 0) {
        /* A maximum-size negated number can't fit as a positive,
         * so do one digit in negated form to start us off. */
        *--s = (char)(-(num % 10)) + '0';
        num = -(num / 10);
        len++;
        negated = 1;
    } else
        negated = 0;

    while (num) {
        if (human_flag) {
            if (len == 3) {
                *--s = number_separator;
                len = 1;
            } else
                len++;
        }
        *--s = (char)(num % 10) + '0';
        num /= 10;
    }

    if (negated)
        *--s = '-';

    return s;
}

/* Return the double number as a string.  If the human_flag option is > 1,
 * we may output the number in K, M, G, or T units.  The buffer we use for
 * our result is either from rotating buffers defined here, or a buffer
 * we get from do_big_num().  We can return up to 4 buffers at a time. */
char *do_big_dnum(double dnum, int human_flag, int decimal_digits)
{
    static char tmp_bufs[4][128];
    static unsigned int tmp_n;
    char *tmp_buf;
#if SIZEOF_INT64 >= 8
    char *fract;
#endif

    tmp_n = (tmp_n + 1) % (sizeof tmp_bufs / sizeof tmp_bufs[0]);
    tmp_buf = tmp_bufs[tmp_n];

    /* Non-finite values (NaN, +Inf, -Inf) cannot be formatted with
     * thousands separators or SI/binary prefixes. */
    if (!isfinite(dnum)) {
        snprintf(tmp_buf, sizeof tmp_bufs[0], "%f", dnum);
        return tmp_buf;
    }

    if (decimal_digits < 0)
        decimal_digits = 0;

    if (human_flag > 1) {
        double d = dnum < 0 ? -dnum : dnum;
        unsigned int mult = human_flag == 2 ? 1000 : 1024;

        if (d >= mult) {
            const char *units = " KMGTPE";
            double powi = 1;

            for (;;) {
                if (d / mult < powi)
                    break;
                if (units[1] == '\0')
                    break;
                powi *= mult;
                ++units;
            }

            snprintf(tmp_buf, sizeof tmp_bufs[0], "%.*f%c",
                     decimal_digits, dnum / powi, *units);
            return tmp_buf;
        }
    }

    snprintf(tmp_buf, sizeof tmp_bufs[0], "%.*f", decimal_digits, dnum);

    if (!human_flag || (dnum < 1000.0 && dnum > -1000.0))
        return tmp_buf;

#if SIZEOF_INT64 >= 8
    /* Check that dnum fits within int64 bounds before casting to avoid UB. */
    if (dnum < 9223372036854775807.0 && dnum > -9223372036854775808.0) {
        for (fract = tmp_buf + 1; isDigit(fract); fract++) {}
        return do_big_num((int64)dnum, human_flag, fract);
    }
#endif

    /* A big number might lose digits converting to a too-short int64,
     * so let's just return the raw double conversion. */
    return tmp_buf;
}
