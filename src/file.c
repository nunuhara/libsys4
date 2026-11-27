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

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "system4.h"
#include "system4/file.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#ifdef _WIN32
#include <Windows.h>
#include <direct.h>
#elif !defined(__SWITCH__)
#include <libgen.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __APPLE__

// Normalizes a UTF-8 encoded file name to Unicode NFC (Normalization Form
// Composed). This is necessary because Apple file systems store file names
// in NFD (Normalization Form Decomposed), which can cause issues when
// comparing file names, especially for characters like 'が' (U+304C) vs.
// 'が' (U+304B U+3099).
static char *normalize_to_nfc(const char *str)
{
	CFStringRef cfstr = CFStringCreateWithCString(kCFAllocatorDefault, str, kCFStringEncodingUTF8);
	CFMutableStringRef normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, cfstr);
	CFStringNormalize(normalized, kCFStringNormalizationFormC);
	CFIndex utf16_len = CFStringGetLength(normalized);
	CFIndex len = CFStringGetMaximumSizeForEncoding(utf16_len, kCFStringEncodingUTF8);
	char *buf = xmalloc(len + 1);
	CFStringGetCString(normalized, buf, len + 1, kCFStringEncodingUTF8);
	CFRelease(normalized);
	CFRelease(cfstr);
	return buf;
}

#elif defined(__EMSCRIPTEN__)

// Normalization is needed, for macOS and iOS Safari.
EM_JS(char*, normalize_to_nfc, (const char *str), {
	return stringToNewUTF8(UTF8ToString(str).normalize('NFC'));
});

#else

// No normalization needed on other platforms.
static char *normalize_to_nfc(const char *str)
{
	return xstrdup(str);
}

#endif

static _Atomic uint64_t g_file_mutation_gen = 1;

uint64_t file_mutation_gen(void)
{
	return atomic_load_explicit(&g_file_mutation_gen, memory_order_acquire);
}

void file_note_fs_mutation(void)
{
	atomic_fetch_add_explicit(&g_file_mutation_gen, 1, memory_order_release);
}

static inline bool mode_mutates_fs(const char *mode)
{
	if (!mode)
		return false;
	for (const char *p = mode; *p; p++) {
		if (*p == 'w' || *p == 'a' || *p == '+')
			return true;
	}
	return false;
}

#ifdef _WIN32
static int make_dir(const char *path, possibly_unused int mode)
{
	wchar_t *wpath = utf8_to_wchar(path);
	int r = _wmkdir(wpath);
	free(wpath);
	return r;
}

UDIR *opendir_utf8(const char *path)
{
	wchar_t *wpath = utf8_to_wchar(path);
	UDIR *r = _wopendir(wpath);
	free(wpath);
	return r;
}

int closedir_utf8(UDIR *dir)
{
	return _wclosedir(dir);
}

char *readdir_utf8(UDIR *dir)
{
	struct _wdirent *e = _wreaddir(dir);
	if (!e)
		return NULL;
	return wchar_to_utf8(e->d_name);
}

int stat_utf8(const char *path, ustat *st)
{
	wchar_t *wpath = utf8_to_wchar(path);
	int r = _wstat64(wpath, st);
	free(wpath);
	return r;
}

char *realpath_utf8(const char *upath)
{
	wchar_t realpath[PATH_MAX];
	wchar_t *relpath = utf8_to_wchar(upath);
	if (!_wfullpath(realpath, relpath, PATH_MAX)) {
		return NULL;
	}
	char *utf8 = wchar_to_utf8(realpath);
	for (int i = 0; utf8[i]; i++) {
		if (utf8[i] == '\\')
			utf8[i] = '/';
	}
	return utf8;
}

int remove_utf8(const char *path)
{
	wchar_t *wpath = utf8_to_wchar(path);
	int r = _wremove(wpath);
	free(wpath);
	if (r == 0)
		file_note_fs_mutation();
	return r;
}

int rmdir_utf8(const char *path)
{
	wchar_t *wpath = utf8_to_wchar(path);
	int r = _wrmdir(wpath);
	free(wpath);
	if (r == 0)
		file_note_fs_mutation();
	return r;
}

FILE *file_open_utf8(const char *path, const char *mode)
{
	wchar_t *wpath = utf8_to_wchar(path);
	wchar_t wmode[64];
	mbstowcs(wmode, mode, 64);

	FILE *f = _wfopen(wpath, wmode);
	free(wpath);
	if (f && mode_mutates_fs(mode))
		file_note_fs_mutation();
	return f;
}

static inline bool is_path_separator(char c)
{
	return c == '/' || c == '\\';
}

char *path_dirname(const char *path)
{
	static char buf[PATH_MAX];
	strncpy(buf, path, PATH_MAX-1);

	char *s = (isalpha(buf[0]) && buf[1] == ':') ? buf + 2 : buf;
	if (!*s)
		return ".";

	int i = strlen(s) - 1;
	if (is_path_separator(s[i])) {
		if (!i)
			return buf;
		i--;
	}
	while (!is_path_separator(s[i])) {
		if (!i) {
			strcpy(s, ".");
			return buf;
		}
		i--;
	}
	if (i && is_path_separator(s[i]))
		i--;
	s[i+1] = '\0';
	return buf;
}

char *path_basename(const char *path)
{
	if (isalpha(path[0]) && path[1] == ':')
		path += 2;
	if (!*path)
		return ".";

	static char buf[PATH_MAX];
	strncpy(buf, path, PATH_MAX-1);
	int i = strlen(buf) - 1;
	if (i && is_path_separator(buf[i]))
		buf[i--] = '\0';
	while (i && !is_path_separator(buf[i-1]))
		i--;
	return buf + i;
}

#else

#define make_dir(path, mode) mkdir(path, mode)

UDIR *opendir_utf8(const char *path)
{
	return opendir(path);
}

int closedir_utf8(UDIR *dir)
{
	return closedir(dir);
}

char *readdir_utf8(UDIR *dir)
{
	struct dirent *e = readdir(dir);
	if (!e)
		return NULL;
	return normalize_to_nfc(e->d_name);
}

int stat_utf8(const char *path, ustat *st)
{
	return stat(path, st);
}

char *realpath_utf8(const char *upath)
{
	return realpath(upath, NULL);
}

int remove_utf8(const char *path)
{
	int r = remove(path);
	if (r == 0)
		file_note_fs_mutation();
	return r;
}

int rmdir_utf8(const char *path)
{
	int r = rmdir(path);
	if (r == 0)
		file_note_fs_mutation();
	return r;
}

FILE *file_open_utf8(const char *path, const char *mode)
{
	FILE *f = fopen(path, mode);
	if (f && mode_mutates_fs(mode))
		file_note_fs_mutation();
	return f;
}

#ifdef __SWITCH__
static char *switch_path_component(const char *path, char *buf)
{
	if (!path || !*path)
		path = ".";
	snprintf(buf, PATH_MAX, "%s", path);
	char *start = buf;
	char *mount = strchr(buf, ':');
	if (mount && mount[1] == '/' && strcspn(buf, "/") == (size_t)(mount + 1 - buf))
		start = mount + 1;
	size_t len = strlen(start);
	while (len > 1 && start[len - 1] == '/')
		start[--len] = '\0';
	return start;
}
#endif

char *path_dirname(const char *path)
{
	static char buf[PATH_MAX];
#ifdef __SWITCH__
	char *start = switch_path_component(path, buf);
	char *slash = strrchr(start, '/');
	if (!slash)
		return ".";
	while (slash > start && slash[-1] == '/')
		slash--;
	*(slash == start ? slash + 1 : slash) = '\0';
	return buf;
#else
	if (!path)
		return ".";
	size_t n = strlen(path);
	if (n >= PATH_MAX)
		n = PATH_MAX - 1;
	memmove(buf, path, n);
	buf[n] = '\0';
	return dirname(buf);
#endif
}

char *path_basename(const char *path)
{
	static char buf[PATH_MAX];
#ifdef __SWITCH__
	char *start = switch_path_component(path, buf);
	char *slash = strrchr(start, '/');
	return slash && slash[1] ? slash + 1 : start;
#else
	if (!path)
		return ".";
	size_t n = strlen(path);
	if (n >= PATH_MAX)
		n = PATH_MAX - 1;
	memmove(buf, path, n);
	buf[n] = '\0';
	return basename(buf);
#endif
}
#endif

void *file_read(const char *path, size_t *len_out)
{
	FILE *fp;
	long len;
	uint8_t *buf;

	ustat s;
	if (!path || stat_utf8(path, &s) < 0) {
		return NULL;
	}
	if (!S_ISREG(s.st_mode)) {
		WARNING("'%s' is not a regular file", path);
		return NULL;
	}

	if (!(fp = file_open_utf8(path, "rb")))
		return NULL;

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	len = ftell(fp);
	if (len < 0 || fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return NULL;
	}

	buf = xmalloc((size_t)len + 1);
	if (len > 0 && fread(buf, (size_t)len, 1, fp) != 1) {
		fclose(fp);
		free(buf);
		return NULL;
	}
	if (fclose(fp)) {
		free(buf);
		return NULL;
	}

	if (len_out)
		*len_out = (size_t)len;

	buf[len] = 0;
	return buf;
}

bool file_write(const char *path, uint8_t *data, size_t data_size)
{
	FILE *fp = file_open_utf8(path, "wb");
	if (!fp)
		return false;
	int r = data_size == 0 ? 1 : (int)fwrite(data, data_size, 1, fp);
	int tmp = errno;
	fclose(fp);
	errno = tmp;
	return r == 1;
}

bool file_copy(const char *src, const char *dst)
{
	size_t data_size;
	uint8_t *data = file_read(src, &data_size);
	if (!data)
		return false;
	bool r = file_write(dst, data, data_size);
	free(data);
	return r;
}

bool file_exists(const char *path)
{
	if (!path)
		return false;
#ifdef _WIN32
	wchar_t *wpath = utf8_to_wchar(path);
	bool r = _waccess(wpath, F_OK) != -1;
	free(wpath);
	return r;
#else
	return access(path, F_OK) != -1;
#endif
}

char *path_join(const char *dir, const char *base)
{
	size_t dir_len = strlen(dir);
	size_t base_len = strlen(base);
	if (!dir_len)
		return xstrdup(base);

	int need_sep = dir[dir_len-1] != '/' ? 1 : 0;
	char *path = xmalloc(dir_len + need_sep + base_len + 1);
	memcpy(path, dir, dir_len);
	if (need_sep)
		path[dir_len] = '/';
	memcpy(path + dir_len + need_sep, base, base_len+1);
	return path;
}

struct string *path_join_string(struct string *dir, struct string *base)
{
	if (!dir || !dir->size)
		return string_dup(base);

	struct string *out = string_dup(dir);
	if (dir->text[dir->size - 1] != '/')
		string_push_back(&out, '/');
	string_append(&out, base);
	return out;
}

static char *path_find_icase(const char *dir_name, const char *base_name)
{
	DIR *d = opendir(dir_name);
	if (!d)
		return NULL;

	struct dirent *dir;
	while ((dir = readdir(d)) != NULL) {
		if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, "..")) {
			continue;
		}

		char *d_name = normalize_to_nfc(dir->d_name);
		if (!strcasecmp(d_name, base_name)) {
			char *path = path_join(dir_name, d_name);
			free(d_name);
			closedir(d);
			return path;
		}
		free(d_name);
	}

	closedir(d);
	return NULL;
}

char *path_get_icase(const char *path)
{
#ifdef __SWITCH__
	if (!path || !*path)
		return NULL;
	char *parts = xstrdup(path);
	char *slash = strchr(parts, '/');
	char *rest = parts;
	char *resolved;
	if (slash && (slash == parts || slash[-1] == ':')) {
		const size_t len = slash + 1 - parts;
		resolved = xmalloc(len + 1);
		memcpy(resolved, parts, len);
		resolved[len] = '\0';
		rest = slash + 1;
	} else {
		resolved = xstrdup(".");
	}
	char *state;
	for (char *part = strtok_r(rest, "/", &state); part; part = strtok_r(NULL, "/", &state)) {
		if (!strcmp(part, "."))
			continue;
		char *next = path_join(resolved, part);
		if (!file_exists(next)) {
			free(next);
			next = path_find_icase(resolved, part);
		}
		free(resolved);
		resolved = next;
		if (!resolved)
			break;
	}
	free(parts);
	if (resolved && !file_exists(resolved)) {
		free(resolved);
		return NULL;
	}
	return resolved;
#else
	return path_find_icase(path_dirname(path), path_basename(path));
#endif
}

const char *file_extension(const char *path)
{
	const char *ext = strrchr(path, '.');
	return ext ? ext+1 : NULL;
}

bool is_directory(const char *path)
{
	ustat s;
	if (!path || stat_utf8(path, &s) < 0)
		return false;
	return S_ISDIR(s.st_mode);
}

off_t file_size(const char *path)
{
	ustat s;
	if (!path || stat_utf8(path, &s) < 0)
		return -1;
	if (!S_ISREG(s.st_mode))
		return -1;
	return s.st_size;
}

// Adapted from http://stackoverflow.com/a/2336245/119527
int mkdir_p(const char *path)
{
	const size_t len = strlen(path);
	char _path[PATH_MAX];
	char *p;

	errno = 0;

	// Copy string so its mutable
	if (len > sizeof(_path)-1) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(_path, path);

	// In xsystem4-android this function is used to create a save directory.
	// Save directories must be group-readable so that they can be transferred
	// via MTP.
#ifdef __ANDROID__
	int mode = S_IRWXU | S_IRWXG;
#else
	int mode = S_IRWXU;
#endif

	p = _path + 1;
#ifdef __SWITCH__
	char *slash = strchr(_path, '/');
	if (slash && slash > _path && slash[-1] == ':')
		p = slash + 1;
#endif
	bool created = false;
	for (; *p; p++) {
		if (*p == '/') {
			// Temporarily truncate
			*p = '\0';

			if (make_dir(_path, mode) != 0) {
				if (errno != EEXIST) {
					if (created)
						file_note_fs_mutation();
					return -1;
				}
			} else {
				created = true;
			}

			*p = '/';
		}
	}

	if (make_dir(_path, mode) != 0) {
		if (errno != EEXIST) {
			if (created)
				file_note_fs_mutation();
			return -1;
		}
	} else {
		created = true;
	}

	if (created)
		file_note_fs_mutation();
	return 0;
}
