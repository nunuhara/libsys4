/*
 * qnt.c  extract QNT cg
 *
 * Copyright (C) 1997-1998 Masaki Chikama (Wren) <chikama@kasumi.ipl.mech.nagoya-u.ac.jp>
 *               1998-                           <masaki-c@is.aist-nara.ac.jp>
 *               2019-2020 Nunuhara Cabbage      <nunuhara@haniwa.technology>
 *               2020      <KichikuouChrome@gmail.com>
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>
#include "little_endian.h"
#include "system4.h"
#include "system4/cg.h"
#include "system4/qnt.h"

/*
  zlib の展開バッファで、幅×高さ×３に、さらにどれくらい余裕をとるか
   (リクルスで 1164バイトというのがあった)
*/
#define ZLIBBUF_MARGIN 5*1024

/*
 * Get information from header
 *
 *   b: raw data
 *
 *   return: acquired qnt information object
 */
void qnt_extract_header(const uint8_t *b, struct qnt_header *qnt)
{
	int rsv0;

	rsv0 = LittleEndian_getDW(b, 4);
	if (rsv0 == 0) {
		qnt->hdr_size = 48;
		qnt->x0  = LittleEndian_getDW(b, 8);
		qnt->y0  = LittleEndian_getDW(b, 12);
		qnt->width  = LittleEndian_getDW(b, 16);
		qnt->height = LittleEndian_getDW(b, 20);
		qnt->bpp    = LittleEndian_getDW(b, 24);
		qnt->rsv    = LittleEndian_getDW(b, 28);
		qnt->pixel_size = LittleEndian_getDW(b, 32);
		qnt->alpha_size = LittleEndian_getDW(b, 36);
	} else {
		qnt->hdr_size = LittleEndian_getDW(b, 8);
		qnt->x0     = LittleEndian_getDW(b, 12);
		qnt->y0     = LittleEndian_getDW(b, 16);
		qnt->width  = LittleEndian_getDW(b, 20);
		qnt->height = LittleEndian_getDW(b, 24);
		qnt->bpp    = LittleEndian_getDW(b, 28);
		qnt->rsv    = LittleEndian_getDW(b, 32);
		qnt->pixel_size = LittleEndian_getDW(b, 36);
		qnt->alpha_size = LittleEndian_getDW(b, 40);
	}
	if (qnt->bpp != 24)
		WARNING("Unsupported bits-per-pixel: %d", qnt->bpp);
}

/*
 * Do extract qnt pixel image
 *
 *   qnt: qnt header information
 *   pic: pixel to be stored
 *   b  : raw data (pointer to pixel)
 */
static void extract_pixel(struct qnt_header *qnt, uint8_t *pic, const uint8_t *b)
{
	int i, j, x, y, w, h;
	unsigned long ucbuf = (qnt->width+1) * (qnt->height+1) * 3 + ZLIBBUF_MARGIN;
	uint8_t *raw = malloc(sizeof(uint8_t) * ucbuf);

	if (Z_OK != uncompress(raw, &ucbuf, b, qnt->pixel_size)) {
		WARNING("uncompress failed\n");
		free(raw);
		return;
	}

	w = qnt->width;
	h = qnt->height;

	j = 0;
	for (i = 2; i >= 0; i--) {
		for (y = 0; y < (h -1); y+=2) {
			for (x = 0; x < (w -1); x+=2) {
				pic[( y   *w+x)  *3 +i] = raw[j];
				pic[((y+1)*w+x)  *3 +i] = raw[j+1];
				pic[( y   *w+x+1)*3 +i] = raw[j+2];
				pic[((y+1)*w+x+1)*3 +i] = raw[j+3];
				j+=4;
			}
			if (x != w) {
				pic[( y   *w+x)*3 +i] = raw[j];
				pic[((y+1)*w+x)*3 +i] = raw[j+1];
				j+=4;
			}
		}
		if (y != h) {
			for (x = 0; x < (w -1); x+=2) {
				pic[(y*w+x  )*3+i] = raw[j];
				pic[(y*w+x+1)*3+i] = raw[j+2];
				j+=4;
			}
			if (x != w) {
				pic[( y   *w+x)*3 +i] = raw[j];
				j+=4;
			}
		}
	}

	if (w > 1) {
		for (x = 1; x < w; x++) {
			pic[x*3  ] = pic[(x-1)*3  ] - pic[x*3  ];
			pic[x*3+1] = pic[(x-1)*3+1] - pic[x*3+1];
			pic[x*3+2] = pic[(x-1)*3+2] - pic[x*3+2];
		}
	}

	if (h > 1) {
		for (y = 1; y < h; y++) {
			pic[(y*w)*3  ] = pic[((y-1)*w)*3  ] - pic[(y*w)*3  ];
			pic[(y*w)*3+1] = pic[((y-1)*w)*3+1] - pic[(y*w)*3+1];
			pic[(y*w)*3+2] = pic[((y-1)*w)*3+2] - pic[(y*w)*3+2];

			for (x = 1; x < w; x++) {
				int px, py;
				py = pic[((y-1)*w+x  )*3];
				px = pic[( y   *w+x-1)*3];
				pic[(y*w+x)*3] = ((py+px)>>1) - pic[(y*w+x)*3];
				py = pic[((y-1)*w+x  )*3+1];
				px = pic[( y   *w+x-1)*3+1];
				pic[(y*w+x)*3+1] = ((py+px)>>1) - pic[(y*w+x)*3+1];
				py = pic[((y-1)*w+x  )*3+2];
				px = pic[( y   *w+x-1)*3+2];
				pic[(y*w+x)*3+2] = ((py+px)>>1) - pic[(y*w+x)*3+2];
			}
		}
	}

	free(raw);
}

/*
 * Do extract qnt alpha image
 *
 *   qnt: qnt header information
 *   pic: pixel to be stored
 *   b  : raw data (pointer to alpha pixel)
 */
static void extract_alpha(struct qnt_header *qnt, uint8_t *pic, const uint8_t *b)
{
	int i, x, y, w, h;
	unsigned long ucbuf = (qnt->width+1) * (qnt->height+1) + ZLIBBUF_MARGIN;
	uint8_t *raw = malloc(sizeof(uint8_t) * ucbuf);

	if (Z_OK != uncompress(raw, &ucbuf, b, qnt->alpha_size)) {
		WARNING("uncompress failed\n");
		free(raw);
		return;
	}

	w = qnt->width;
	h = qnt->height;

	i = 1;
	if (w > 1) {
		pic[0] = raw[0];
		for (x = 1; x < w; x++) {
			pic[x] = pic[x-1] - raw[i];
			i++;
		}
		if (w%2) i++;
	}

	if (h > 1) {
		for (y = 1; y < h; y++) {
			pic[y*w] = pic[(y-1) *w] - raw[i]; i++;
			for (x = 1; x < w; x++) {
				int pax, pay;
				pax = pic[ y   * w + x -1];
				pay = pic[(y-1)* w + x   ];
				pic[y*w+x] = ((pax+pay) >> 1) - raw[i];
				i++;
			}
			if (w%2) i++;
		}
	}

	free(raw);
}

/*
 * Check data is qnt format cg or not
 *
 *   data: raw data (pointer to data top)
 *
 *   return: true if data is qnt
 */
bool qnt_checkfmt(const uint8_t *data)
{
	if (data[0] != 'Q' || data[1] != 'N' || data[2] != 'T') return false;
	return true;
}

static void qnt_init_metrics(struct qnt_header *qnt, struct cg_metrics *dst)
{
	dst->w = qnt->width;
	dst->h = qnt->height;
	dst->bpp = qnt->bpp;
	dst->has_pixel = qnt->pixel_size > 0;
	dst->has_alpha = qnt->alpha_size > 0;
	dst->pixel_pitch = qnt->width * (qnt->bpp / 8);
	dst->alpha_pitch = 1;
}

bool qnt_get_metrics(const uint8_t *data, struct cg_metrics *dst)
{
	struct qnt_header qnt;
	qnt_extract_header(data, &qnt);
	qnt_init_metrics(&qnt, dst);
	return true;
}

/*
 * Extract qnt header and pixel
 *
 *   data: raw data (pointer to data top)
 *
 *   return: extracted image data and information
*/
void qnt_extract(const uint8_t *data, struct cg *cg)
{
	struct qnt_header qnt;
	qnt_extract_header(data, &qnt);
	qnt_init_metrics(&qnt, &cg->metrics);

	uint8_t *pixels = xcalloc(3, (qnt.width+10) * (qnt.height+10));
	if (qnt.pixel_size) {
		extract_pixel(&qnt, pixels, data + qnt.hdr_size);
	}

	cg->type = ALCG_QNT;

	// combine color/alpha data
	uint8_t *alpha = xmalloc((qnt.width+10) * (qnt.height+10));
	uint8_t *tmp = xmalloc((qnt.width+10) * (qnt.height+10) * 4);
	if (qnt.alpha_size) {
		extract_alpha(&qnt, alpha, data + qnt.hdr_size + qnt.pixel_size);
	} else {
		// FIXME: Some CGs don't display correctly unless we add an alpha channel here.
		//        Not sure why. It seems to affect some but not all alpha-less CGs.
		//        E.g. CG#90 (and similar) from the Rance 2 digest version.
		memset(alpha, 0xFF, (qnt.width+10)*(qnt.height+10));
	}
	for (int src_i = 0, dst_i = 0, p = 0; p < qnt.width * qnt.height; p++) {
		tmp[dst_i++] = pixels[src_i++];
		tmp[dst_i++] = pixels[src_i++];
		tmp[dst_i++] = pixels[src_i++];
		tmp[dst_i++] = alpha[p];
	}
	free(alpha);
	free(pixels);
	cg->pixels = tmp;
}

/*
 * QNT encoder adapted from xsys35c
 * (github.com/kichikuou/xsys35c)
 */

static void fputdw(uint32_t n, FILE *fp) {
	fputc(n, fp);
	fputc(n >> 8, fp);
	fputc(n >> 16, fp);
	fputc(n >> 24, fp);
}

static void qnt_write_header(struct qnt_header *qnt, FILE *fp) {
	fputc('Q', fp);
	fputc('N', fp);
	fputc('T', fp);
	fputc('\0', fp);
	fputdw(1, fp);
	fputdw(qnt->hdr_size, fp);
	fputdw(qnt->x0, fp);
	fputdw(qnt->y0, fp);
	fputdw(qnt->width, fp);
	fputdw(qnt->height, fp);
	fputdw(qnt->bpp, fp);
	fputdw(qnt->rsv, fp);
	fputdw(qnt->pixel_size, fp);
	fputdw(qnt->alpha_size, fp);
	for (int i = 44; i < qnt->hdr_size; i++)
		fputc(0, fp);
}

static uint8_t *encode_pixels(struct qnt_header *qnt, uint8_t **rows) {
	int width = (qnt->width + 1) & ~1;
	int height = (qnt->height + 1) & ~1;

	const int bufsize = width * height * 3;
	uint8_t *buf = malloc(bufsize);
	uint8_t *p = buf;
	for (int c = 2; c >= 0; c--) {
		for (int y = 0; y < height; y += 2) {
			for (int x = 0; x < width; x += 2) {
				*p++ = rows[y  ][ x   *4 + c];
				*p++ = rows[y+1][ x   *4 + c];
				*p++ = rows[y  ][(x+1)*4 + c];
				*p++ = rows[y+1][(x+1)*4 + c];
			}
		}
	}
	assert(p == buf + bufsize);

	unsigned long destsize = compressBound(bufsize);
	uint8_t *compressed = malloc(destsize);
	int r = compress2(compressed, &destsize, buf, bufsize, Z_BEST_COMPRESSION);
	if (r != Z_OK) {
		WARNING("qnt: compress() failed with error code %d", r);
		free(buf);
		free(compressed);
		return NULL;
	}
	qnt->pixel_size = destsize;

	free(buf);
	return compressed;
}

static uint8_t *encode_alpha(struct qnt_header *qnt, uint8_t **rows) {
	int width = (qnt->width + 1) & ~1;
	int height = (qnt->height + 1) & ~1;

	const int bufsize = width * height;
	uint8_t *buf = malloc(bufsize);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++)
			buf[y * width + x] = rows[y][x * 4 + 3];
	}

	unsigned long destsize = compressBound(bufsize);
	uint8_t *compressed = malloc(destsize);
	int r = compress2(compressed, &destsize, buf, bufsize, Z_BEST_COMPRESSION);
	if (r != Z_OK) {
		WARNING("qnt: compress() failed with error code %d", r);
		free(buf);
		free(compressed);
		return NULL;
	}
	qnt->alpha_size = destsize;

	free(buf);
	return compressed;
}

static void filter(uint8_t **rows, int width, int height) {
	for (int y = height - 1; y > 0; y--) {
		for (int x = width - 1; x > 0; x--) {
			for (int c = 0; c < 4; c++) {
				int up = rows[y-1][x*4+c];
				int left = rows[y][(x-1)*4+c];
				rows[y][x*4+c] = ((up + left) >> 1) - rows[y][x*4+c];
			}
		}

		for (int c = 0; c < 4; c++)
			rows[y][c] = rows[y-1][c] - rows[y][c];
	}
	for (int x = width - 1; x > 0; x--) {
		for (int c = 0; c < 4; c++)
			rows[0][x*4+c] = rows[0][(x-1)*4+c] - rows[0][x*4+c];
	}
}

static uint8_t **allocate_bitmap_buffer(int width, int height)
{
	uint8_t **rows = xmalloc(sizeof(uint8_t*)*height);
	uint8_t *buffer = calloc(1, height * width * 4);
	for (int y = 0; y < height; y++) {
		rows[y] = buffer + y * width * 4;
	}
	return rows;
}

static void free_bitmap_buffer(uint8_t **rows)
{
	free(rows[0]);
	free(rows);
}

int qnt_write(struct cg *cg, FILE *f)
{
	struct qnt_header qnt = {
		.hdr_size = 52,
		.width = cg->metrics.w,
		.height = cg->metrics.h,
		.bpp = 24,
		.rsv = 1,
	};

	uint8_t **rows = allocate_bitmap_buffer((qnt.width + 1) & ~1, (qnt.height + 1) & ~1);
	uint8_t *buf = cg->pixels;
	for (int i = 0; i < qnt.height; i++) {
		memcpy(rows[i], buf + 4*qnt.width*i, 4*qnt.width);
	}

	filter(rows, cg->metrics.w, cg->metrics.h);
	uint8_t *pixel_data = encode_pixels(&qnt, rows);
	uint8_t *alpha_data = encode_alpha(&qnt, rows);
	if (!pixel_data || !alpha_data) {
		free(pixel_data);
		free(alpha_data);
		return 0;
	}

	qnt_write_header(&qnt, f);
	fwrite(pixel_data, qnt.pixel_size, 1, f);
	free(pixel_data);
	fwrite(alpha_data, qnt.alpha_size, 1, f);
	free(alpha_data);
	free_bitmap_buffer(rows);
	return 1;
}
