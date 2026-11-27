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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "system4.h"
#include "system4/utfsjis.h"

bool sys_silent;

void (*sys_error_handler)(const char *msg) = NULL;
void (*sys_warning_handler)(const char *msg) = NULL;
void (*sys_exit_handler)(int code) = NULL;

mem_alloc void *_xmalloc(size_t size, const char *func)
{
	void *ptr = malloc(size ? size : 1);
	if (!ptr) {
		sys_error("*ERROR*(%s): Out of memory\n", func);
	}
	return ptr;
}

mem_alloc void *_xcalloc(size_t nmemb, size_t size, const char *func)
{
	void *ptr = calloc(nmemb ? nmemb : 1, size ? size : 1);
	if (!ptr) {
		sys_error("*ERROR*(%s): Out of memory\n", func);
	}
	return ptr;
}

mem_alloc void *_xrealloc(void *ptr, size_t size, const char *func)
{
	ptr = realloc(ptr, size ? size : 1);
	if (!ptr) {
		sys_error("*ERROR*(%s): Out of memory\n", func);
	}
	return ptr;
}

mem_alloc void *_xcalloc_aligned(size_t alignment, size_t size, const char *func)
{
	void *ptr;
	if (alignment == 0 || (alignment & (alignment - 1)) != 0)
		sys_error("*ERROR*(%s): Invalid alignment %zu\n", func, alignment);
	if (size > SIZE_MAX - (alignment - 1))
		sys_error("*ERROR*(%s): Out of memory\n", func);
	size = size ? ((size + alignment - 1) & ~(alignment - 1)) : alignment;
#ifdef _WIN32
	// aligned_alloc is C11, but not supported by MSVC's C runtime library.
	ptr = _aligned_malloc(size, alignment);
#elif defined(__ANDROID__)
	// aligned_alloc requires Android 9.0 or later.
	size_t posix_align = alignment < sizeof(void *) ? sizeof(void *) : alignment;
	if (posix_memalign(&ptr, posix_align, size) != 0)
		ptr = NULL;
#else
	ptr = aligned_alloc(alignment, size);
#endif
	if (!ptr) {
		sys_error("*ERROR*(%s): Out of memory\n", func);
	}
	memset(ptr, 0, size);
	return ptr;
}

void xfree_aligned(void *ptr)
{
#ifdef _WIN32
	_aligned_free(ptr);
#else
	free(ptr);
#endif
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
	size_t new_bytes, old_bytes;
	if (__builtin_mul_overflow(new_nmemb, size, &new_bytes) ||
	    __builtin_mul_overflow(old_nmemb, size, &old_bytes)) {
		sys_error("*ERROR*(%s): Out of memory\n", __func__);
	}
	dst = xrealloc(dst, new_bytes);
	if (new_bytes > old_bytes)
		memset((char*)dst + old_bytes, 0, new_bytes - old_bytes);
	return dst;
}

_Noreturn void sys_verror(const char *fmt, va_list ap)
{
	if (sys_error_handler) {
		char msg[4096];
		va_list copy;
		va_copy(copy, ap);
		vsnprintf(msg, 4096, fmt, copy);
		va_end(copy);
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
	if (sys_warning_handler) {
		char msg[1024];
		va_list copy;
		va_copy(copy, ap);
		vsnprintf(msg, sizeof(msg), fmt, copy);
		va_end(copy);
		sys_warning_handler(msg);
	}
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
	if (sys_exit_handler)
		sys_exit_handler(code);
	// TODO: cleanup
	exit(code);
}
