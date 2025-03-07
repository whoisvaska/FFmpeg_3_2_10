/*
 * MagicYUV decoder
 * Copyright (c) 2016 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdlib.h>
#include <string.h>

#include "libavutil/pixdesc.h"
#include "libavutil/qsort.h"

#include "avcodec.h"
#include "bytestream.h"
#include "get_bits.h"
#include "huffyuvdsp.h"
#include "internal.h"
#include "thread.h"

typedef struct Slice {
    uint32_t start;
    uint32_t size;
} Slice;

typedef enum Prediction {
    LEFT = 1,
    GRADIENT,
    MEDIAN,
} Prediction;

typedef struct HuffEntry {
    uint8_t  sym;
    uint8_t  len;
    uint32_t code;
} HuffEntry;

typedef struct MagicYUVContext {
    AVFrame          *p;
    int               slice_height;
    int               nb_slices;
    int               planes;         // number of encoded planes in bitstream
    int               decorrelate;    // postprocessing work
    int               interlaced;     // video is interlaced
    uint8_t          *buf;            // pointer to AVPacket->data
    int               hshift[4];
    int               vshift[4];
    Slice            *slices[4];      // slice bitstream positions for each plane
    unsigned int      slices_size[4]; // slice sizes for each plane
    uint8_t           len[4][256];    // table of code lengths for each plane
    VLC               vlc[4];         // VLC for each plane
    HuffYUVDSPContext hdsp;
} MagicYUVContext;

static int huff_cmp_len(const void *a, const void *b)
{
    const HuffEntry *aa = a, *bb = b;
    return (aa->len - bb->len) * 256 + aa->sym - bb->sym;
}

static int huff_build(VLC *vlc, uint8_t *len)
{
    HuffEntry he[256];
    uint32_t codes[256];
    uint8_t bits[256];
    uint8_t syms[256];
    uint32_t code;
    int i;

    for (i = 0; i < 256; i++) {
        he[i].sym = 255 - i;
        he[i].len = len[i];
        if (len[i] == 0 || len[i] > 32)
            return AVERROR_INVALIDDATA;
    }
    AV_QSORT(he, 256, HuffEntry, huff_cmp_len);

    code = 1;
    for (i = 255; i >= 0; i--) {
        codes[i] = code >> (32 - he[i].len);
        bits[i]  = he[i].len;
        syms[i]  = he[i].sym;
        code += 0x80000000u >> (he[i].len - 1);
    }

    ff_free_vlc(vlc);
    return ff_init_vlc_sparse(vlc, FFMIN(he[255].len, 12), 256,
                              bits,  sizeof(*bits),  sizeof(*bits),
                              codes, sizeof(*codes), sizeof(*codes),
                              syms,  sizeof(*syms),  sizeof(*syms), 0);
}

static int magy_decode_slice(AVCodecContext *avctx, void *tdata,
                             int j, int threadnr)
{
    MagicYUVContext *s = avctx->priv_data;
    int interlaced = s->interlaced;
    AVFrame *p = s->p;
    int i, k, x;
    GetBitContext gb;
    uint8_t *dst;

    for (i = 0; i < s->planes; i++) {
        int left, lefttop, top;
        int height = AV_CEIL_RSHIFT(FFMIN(s->slice_height, avctx->coded_height - j * s->slice_height), s->vshift[i]);
        int width = AV_CEIL_RSHIFT(avctx->coded_width, s->hshift[i]);
        int sheight = AV_CEIL_RSHIFT(s->slice_height, s->vshift[i]);
        ptrdiff_t fake_stride = p->linesize[i] * (1 + interlaced);
        ptrdiff_t stride = p->linesize[i];
        int flags, pred;
        int ret = init_get_bits8(&gb, s->buf + s->slices[i][j].start,
                                 s->slices[i][j].size);

        if (ret < 0)
            return ret;

        flags = get_bits(&gb, 8);
        pred  = get_bits(&gb, 8);

        dst = p->data[i] + j * sheight * stride;
        if (flags & 1) {
            for (k = 0; k < height; k++) {
                for (x = 0; x < width; x++)
                    dst[x] = get_bits(&gb, 8);

                dst += stride;
            }
        } else {
            for (k = 0; k < height; k++) {
                for (x = 0; x < width; x++) {
                    int pix;
                    if (get_bits_left(&gb) <= 0)
                        return AVERROR_INVALIDDATA;

                    pix = get_vlc2(&gb, s->vlc[i].table, s->vlc[i].bits, 3);
                    if (pix < 0)
                        return AVERROR_INVALIDDATA;

                    dst[x] = 255 - pix;
                }
                dst += stride;
            }
        }

        switch (pred) {
        case LEFT:
            dst = p->data[i] + j * sheight * stride;
            s->hdsp.add_hfyu_left_pred(dst, dst, width, 0);
            dst += stride;
            if (interlaced) {
                s->hdsp.add_hfyu_left_pred(dst, dst, width, 0);
                dst += stride;
            }
            for (k = 1 + interlaced; k < height; k++) {
                s->hdsp.add_hfyu_left_pred(dst, dst, width, dst[-fake_stride]);
                dst += stride;
            }
            break;
        case GRADIENT:
            dst = p->data[i] + j * sheight * stride;
            s->hdsp.add_hfyu_left_pred(dst, dst, width, 0);
            left = lefttop = 0;
            dst += stride;
            if (interlaced) {
                s->hdsp.add_hfyu_left_pred(dst, dst, width, 0);
                left = lefttop = 0;
                dst += stride;
            }
            for (k = 1 + interlaced; k < height; k++) {
                top = dst[-fake_stride];
                left = top + dst[0];
                dst[0] = left;
                for (x = 1; x < width; x++) {
                    top = dst[x - fake_stride];
                    lefttop = dst[x - (fake_stride + 1)];
                    left += top - lefttop + dst[x];
                    dst[x] = left;
                }
                dst += stride;
            }
            break;
        case MEDIAN:
            dst = p->data[i] + j * sheight * stride;
            lefttop = left = dst[0];
            s->hdsp.add_hfyu_left_pred(dst, dst, width, 0);
            dst += stride;
            if (interlaced) {
                lefttop = left = dst[0];
                s->hdsp.add_hfyu_left_pred(dst, dst, width, 0);
                dst += stride;
            }
            for (k = 1 + interlaced; k < height; k++) {
                s->hdsp.add_hfyu_median_pred(dst, dst - fake_stride,
                                             dst, width, &left, &lefttop);
                lefttop = left = dst[0];
                dst += stride;
            }
            break;
        default:
            avpriv_request_sample(avctx, "Unknown prediction: %d", pred);
        }
    }

    if (s->decorrelate) {
        int height = FFMIN(s->slice_height, avctx->coded_height - j * s->slice_height);
        int width = avctx->coded_width;
        uint8_t *b = p->data[0] + j * s->slice_height * p->linesize[0];
        uint8_t *g = p->data[1] + j * s->slice_height * p->linesize[1];
        uint8_t *r = p->data[2] + j * s->slice_height * p->linesize[2];

        for (i = 0; i < height; i++) {
            s->hdsp.add_bytes(b, g, width);
            s->hdsp.add_bytes(r, g, width);
            b += p->linesize[0];
            g += p->linesize[1];
            r += p->linesize[2];
        }
    }

    return 0;
}

static int magy_decode_frame(AVCodecContext *avctx, void *data,
                             int *got_frame, AVPacket *avpkt)
{
    MagicYUVContext *s = avctx->priv_data;
    ThreadFrame frame = { .f = data };
    AVFrame *p = data;
    GetByteContext gbyte;
    GetBitContext gbit;
    uint32_t first_offset, offset, next_offset, header_size, slice_width;
    int width, height, format, version, table_size;
    int ret, i, j, k;

    bytestream2_init(&gbyte, avpkt->data, avpkt->size);
    if (bytestream2_get_le32(&gbyte) != MKTAG('M', 'A', 'G', 'Y'))
        return AVERROR_INVALIDDATA;

    header_size = bytestream2_get_le32(&gbyte);
    if (header_size < 32 || header_size >= avpkt->size) {
        av_log(avctx, AV_LOG_ERROR,
               "header or packet too small %"PRIu32"\n", header_size);
        return AVERROR_INVALIDDATA;
    }

    version = bytestream2_get_byte(&gbyte);
    if (version != 7) {
        avpriv_request_sample(avctx, "Version %d", version);
        return AVERROR_PATCHWELCOME;
    }

    s->hshift[1] =
    s->vshift[1] =
    s->hshift[2] =
    s->vshift[2] = 0;
    s->decorrelate = 0;

    format = bytestream2_get_byte(&gbyte);
    switch (format) {
    case 0x65:
        avctx->pix_fmt = AV_PIX_FMT_GBRP;
        s->decorrelate = 1;
        break;
    case 0x66:
        avctx->pix_fmt = AV_PIX_FMT_GBRAP;
        s->decorrelate = 1;
        break;
    case 0x67:
        avctx->pix_fmt = AV_PIX_FMT_YUV444P;
        break;
    case 0x68:
        avctx->pix_fmt = AV_PIX_FMT_YUV422P;
        s->hshift[1] =
        s->hshift[2] = 1;
        break;
    case 0x69:
        avctx->pix_fmt = AV_PIX_FMT_YUV420P;
        s->hshift[1] =
        s->vshift[1] =
        s->hshift[2] =
        s->vshift[2] = 1;
        break;
    case 0x6a:
        avctx->pix_fmt = AV_PIX_FMT_YUVA444P;
        break;
    case 0x6b:
        avctx->pix_fmt = AV_PIX_FMT_GRAY8;
        break;
    default:
        avpriv_request_sample(avctx, "Format 0x%X", format);
        return AVERROR_PATCHWELCOME;
    }
    s->planes = av_pix_fmt_count_planes(avctx->pix_fmt);

    bytestream2_skip(&gbyte, 2);
    s->interlaced = !!(bytestream2_get_byte(&gbyte) & 2);
    bytestream2_skip(&gbyte, 3);

    width  = bytestream2_get_le32(&gbyte);
    height = bytestream2_get_le32(&gbyte);
    ret = ff_set_dimensions(avctx, width, height);
    if (ret < 0)
        return ret;

    slice_width = bytestream2_get_le32(&gbyte);
    if (slice_width != avctx->coded_width) {
        avpriv_request_sample(avctx, "Slice width %"PRIu32, slice_width);
        return AVERROR_PATCHWELCOME;
    }
    s->slice_height = bytestream2_get_le32(&gbyte);
    if (s->slice_height <= 0 || s->slice_height > INT_MAX - avctx->coded_height) {
        av_log(avctx, AV_LOG_ERROR,
               "invalid slice height: %d\n", s->slice_height);
        return AVERROR_INVALIDDATA;
    }

    bytestream2_skip(&gbyte, 4);

    s->nb_slices = (avctx->coded_height + s->slice_height - 1) / s->slice_height;
    if (s->nb_slices > INT_MAX / sizeof(Slice)) {
        av_log(avctx, AV_LOG_ERROR,
               "invalid number of slices: %d\n", s->nb_slices);
        return AVERROR_INVALIDDATA;
    }

    if (s->interlaced) {
        if ((s->slice_height >> s->vshift[1]) < 2) {
            av_log(avctx, AV_LOG_ERROR, "impossible slice height\n");
            return AVERROR_INVALIDDATA;
        }
        if ((avctx->coded_height % s->slice_height) && ((avctx->coded_height % s->slice_height) >> s->vshift[1]) < 2) {
            av_log(avctx, AV_LOG_ERROR, "impossible height\n");
            return AVERROR_INVALIDDATA;
        }
    }

    for (i = 0; i < s->planes; i++) {
        av_fast_malloc(&s->slices[i], &s->slices_size[i], s->nb_slices * sizeof(Slice));
        if (!s->slices[i])
            return AVERROR(ENOMEM);

        offset = bytestream2_get_le32(&gbyte);
        if (offset >= avpkt->size - header_size)
            return AVERROR_INVALIDDATA;

        if (i == 0)
            first_offset = offset;

        for (j = 0; j < s->nb_slices - 1; j++) {
            s->slices[i][j].start = offset + header_size;

            next_offset = bytestream2_get_le32(&gbyte);
            if (next_offset <= offset || next_offset >= avpkt->size - header_size)
                return AVERROR_INVALIDDATA;

            s->slices[i][j].size = next_offset - offset;
            offset = next_offset;
        }

        s->slices[i][j].start = offset + header_size;
        s->slices[i][j].size  = avpkt->size - s->slices[i][j].start;

        if (s->slices[i][j].size < 2)
            return AVERROR_INVALIDDATA;
    }

    if (bytestream2_get_byte(&gbyte) != s->planes)
        return AVERROR_INVALIDDATA;

    bytestream2_skip(&gbyte, s->nb_slices * s->planes);

    table_size = header_size + first_offset - bytestream2_tell(&gbyte);
    if (table_size < 2)
        return AVERROR_INVALIDDATA;

    ret = init_get_bits8(&gbit, avpkt->data + bytestream2_tell(&gbyte), table_size);
    if (ret < 0)
        return ret;

    memset(s->len, 0, sizeof(s->len));
    j = i = 0;
    while (get_bits_left(&gbit) >= 8) {
        int b = get_bits(&gbit, 4);
        int x = get_bits(&gbit, 4);
        int l = get_bitsz(&gbit, b) + 1;

        for (k = 0; k < l; k++)
            if (j + k < 256)
                s->len[i][j + k] = x;

        j += l;
        if (j == 256) {
            j = 0;
            if (huff_build(&s->vlc[i], s->len[i])) {
                av_log(avctx, AV_LOG_ERROR, "Cannot build Huffman codes\n");
                return AVERROR_INVALIDDATA;
            }
            i++;
            if (i == s->planes) {
                break;
            }
        } else if (j > 256) {
            return AVERROR_INVALIDDATA;
        }
    }

    if (i != s->planes) {
        av_log(avctx, AV_LOG_ERROR, "Huffman tables too short\n");
        return AVERROR_INVALIDDATA;
    }

    p->pict_type = AV_PICTURE_TYPE_I;
    p->key_frame = 1;

    if ((ret = ff_thread_get_buffer(avctx, &frame, 0)) < 0)
        return ret;

    s->buf = avpkt->data;
    s->p = p;
    avctx->execute2(avctx, magy_decode_slice, NULL, NULL, s->nb_slices);

    if (avctx->pix_fmt == AV_PIX_FMT_GBRP ||
        avctx->pix_fmt == AV_PIX_FMT_GBRAP) {
        FFSWAP(uint8_t*, p->data[0], p->data[1]);
        FFSWAP(int, p->linesize[0], p->linesize[1]);
    }

    *got_frame = 1;

    return avpkt->size;
}

#if HAVE_THREADS
static int magy_init_thread_copy(AVCodecContext *avctx)
{
    MagicYUVContext *s = avctx->priv_data;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(s->slices); i++) {
        s->slices[i] = NULL;
        s->slices_size[i] = 0;
    }

    return 0;
}
#endif

static av_cold int magy_decode_init(AVCodecContext *avctx)
{
    MagicYUVContext *s = avctx->priv_data;
    ff_huffyuvdsp_init(&s->hdsp);
    return 0;
}

static av_cold int magy_decode_end(AVCodecContext *avctx)
{
    MagicYUVContext * const s = avctx->priv_data;
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(s->slices); i++) {
        av_freep(&s->slices[i]);
        s->slices_size[i] = 0;
        ff_free_vlc(&s->vlc[i]);
    }

    return 0;
}

AVCodec ff_magicyuv_decoder = {
    .name             = "magicyuv",
    .long_name        = NULL_IF_CONFIG_SMALL("MagicYUV video"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_MAGICYUV,
    .priv_data_size   = sizeof(MagicYUVContext),
    .init             = magy_decode_init,
    .init_thread_copy = ONLY_IF_THREADS_ENABLED(magy_init_thread_copy),
    .close            = magy_decode_end,
    .decode           = magy_decode_frame,
    .capabilities     = AV_CODEC_CAP_DR1 |
                        AV_CODEC_CAP_FRAME_THREADS |
                        AV_CODEC_CAP_SLICE_THREADS,
    .caps_internal    = FF_CODEC_CAP_INIT_THREADSAFE,
};
