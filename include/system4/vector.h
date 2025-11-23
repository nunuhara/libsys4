/* The MIT License

   Copyright (c) 2008, by Attractive Chaos <attractor@live.co.uk>

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

#ifndef SYSTEM4_VECTOR_H
#define SYSTEM4_VECTOR_H

#include "system4.h"

/* An example:

#include "system4/vector.h"
int main() {
	vector_t(int) array;
	vector_init(array);
	vector_push(int, array, 10); // append
	vector_a(int, array, 20) = 5; // dynamic
	vector_A(array, 20) = 4; // static
	vector_destroy(array);
	return 0;
}

*/

#define vector_initializer {0}

#define vector_t(type) struct { size_t n, m; type *a; }
#define vector_init(v) ((v).n = (v).m = 0, (v).a = 0)
#define vector_destroy(v) free((v).a)
#define vector_A(v, i) ((v).a[(i)])
#define vector_pop(v) ((v).a[--(v).n])
#define vector_peek(v) ((v).a[(v).n-1])
#define vector_peekn(v, i) ((v).a[(v).n-(i+1)])
#define vector_length(v) ((v).n)
#define vector_empty(v) (vector_length(v) == 0)
#define vector_capacity(v) ((v).m)
#define vector_set_capacity(type, v, s)  ((v).m = (s), (v).a = (type*)xrealloc((v).a, sizeof(type) * (v).m))
#define vector_data(v) ((v).a)

#define vector_resize(t, v, n) \
	do { \
		vector_set_capacity(t, v, n); \
		vector_length(v) = n; \
	} while (0)

#define vector_push(type, v, x) do { \
		if ((v).n == (v).m) { \
			(v).m = (v).m? (v).m<<1 : 2; \
			(v).a = (type*)xrealloc((v).a, sizeof(type) * (v).m); \
		} \
		(v).a[(v).n++] = (x); \
	} while (0)

#define vector_pushp(type, v) \
	((((v).n == (v).m)? \
		((v).m = ((v).m? (v).m<<1 : 2), \
			(v).a = (type*)xrealloc((v).a, sizeof(type) * (v).m), 0) \
		: 0), ((v).a + ((v).n++)))

#define vector_a(type, v, i) (((v).m <= (size_t)(i)? \
	((v).m = (v).n = (i) + 1, kv_roundup32((v).m), \
	 (v).a = (type*)xrealloc((v).a, sizeof(type) * (v).m), 0) \
	: (v).n <= (size_t)(i)? (v).n = (i) + 1 \
	: 0), (v).a[(i)])

#define vector_set(type, v, i, val) (((v).m <= (size_t)(i)? \
	((v).m = (v).n = (i) + 1, kv_roundup32((v).m), \
	 (v).a = (type*)xrealloc((v).a, sizeof(type) * (v).m), 0) \
	: (v).n <= (size_t)(i)? (v).n = (i) + 1 \
	: 0), (v).a[(i)] = val)

#define vector_foreach(var, vec) \
	for (__typeof__(var) *__i = (vec).a; (__i - (vec).a < (vec).n) && (var = *__i, true); __i++)

#define vector_foreach_reverse(var, vec) \
	for (__typeof__(var) *__i = (vec).a + ((vec.n) - 1); (__i >= (vec).a) && (var = *__i, true); __i--)

#define vector_foreach_p(var, vec) \
	for (var = (vec).a; var - (vec).a < (vec).n; var++)

#define vector_foreach_reverse_p(var, vec) \
	for (var = (vec).a + ((vec.n) - 1); var >= (vec).a; var--)

#endif // SYSTEM4_VECTOR_H
