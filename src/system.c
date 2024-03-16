/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "system4.h"
#include "system4/utfsjis.h"

bool sys_silent;

void (*sys_error_handler)(const char *msg) = NULL;

mem_alloc void *_xmalloc(size_t size, const char *func)
{
	void *ptr = malloc(size);
	if (!ptr) {
		sys_error("*ERROR*(%s): Out of memory\n", func);
	}
	return ptr;
}

mem_alloc void *_xcalloc(size_t nmemb, size_t size, const char *func)
{
	void *ptr = calloc(nmemb, size);
	if (!ptr) {
		sys_error("*ERROR*(%s): Out of memory\n", func);
	}
	return ptr;
}

mem_alloc void *_xrealloc(void *ptr, size_t size, const char *func)
{
	ptr = realloc(ptr, size);
	if (!ptr) {
		sys_error("*ERROR*(%s): Out of memory\n", func);
	}
	return ptr;
}

mem_alloc char *_xstrdup(const char *in, const char *func)
{
	char *out = strdup(in);
	if (!out) {
		sys_error("*ERROR*(%s): Out of memory\n", func);
	}
	return out;
}

mem_alloc void *xrealloc_array(void *dst, size_t old_nmemb, size_t new_nmemb, size_t size)
{
	dst = xrealloc(dst, new_nmemb * size);
	if (new_nmemb > old_nmemb)
		memset((char*)dst + old_nmemb*size, 0, (new_nmemb - old_nmemb) * size);
	return dst;
}

_Noreturn void sys_verror(const char *fmt, va_list ap)
{
	if (sys_error_handler) {
		char msg[4096];
		vsnprintf(msg, 4096, fmt, ap);
		sys_error_handler(msg);
	}
#ifdef __ANDROID__
	__android_log_vprint(ANDROID_LOG_FATAL, "libsys4", fmt, ap);
#else
	vfprintf(stderr, fmt, ap);
#endif
	sys_exit(1);
}

_Noreturn void sys_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sys_verror(fmt, ap);
}

void sys_vwarning(const char *fmt, va_list ap)
{
#ifdef __ANDROID__
	__android_log_vprint(ANDROID_LOG_WARN, "libsys4", fmt, ap);
#else
	vfprintf(stderr, fmt, ap);
#endif
}

void sys_warning(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	sys_vwarning(fmt, ap);
	va_end(ap);
}

void sys_vmessage(const char *fmt, va_list ap)
{
	if (sys_silent)
		return;
#ifdef __ANDROID__
	__android_log_vprint(ANDROID_LOG_INFO, "libsys4", fmt, ap);
#else
	vprintf(fmt, ap);
#endif
	fflush(stdout);
}

void sys_message(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sys_vmessage(fmt, ap);
	va_end(ap);
}

_Noreturn void sys_exit(int code)
{
	// TODO: cleanup
	exit(code);
}
