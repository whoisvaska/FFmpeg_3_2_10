/*
 * AU muxer and demuxer
 * Copyright (c) 2001 Fabrice Bellard
 *
 * first version by Francois Revol <revol@free.fr>
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

/*
 * Reference documents:
 * http://www.opengroup.org/public/pubs/external/auformat.html
 * http://www.goice.co.jp/member/mo/formats/au.html
 */

#include "avformat.h"
#include "internal.h"
#include "avio_internal.h"
#include "pcm.h"
#include "libavutil/avassert.h"

/* if we don't know the size in advance */
#define AU_UNKNOWN_SIZE ((uint32_t)(~0))
/* the specification requires an annotation field of at least eight bytes */
#define AU_DEFAULT_HEADER_SIZE (24+8)

static const AVCodecTag codec_au_tags[] = {
    { AV_CODEC_ID_PCM_MULAW,  1 },
    { AV_CODEC_ID_PCM_S8,     2 },
    { AV_CODEC_ID_PCM_S16BE,  3 },
    { AV_CODEC_ID_PCM_S24BE,  4 },
    { AV_CODEC_ID_PCM_S32BE,  5 },
    { AV_CODEC_ID_PCM_F32BE,  6 },
    { AV_CODEC_ID_PCM_F64BE,  7 },
    { AV_CODEC_ID_ADPCM_G726LE, 23 },
    { AV_CODEC_ID_ADPCM_G722,24 },
    { AV_CODEC_ID_ADPCM_G726LE, 25 },
    { AV_CODEC_ID_ADPCM_G726LE, 26 },
    { AV_CODEC_ID_PCM_ALAW,  27 },
    { AV_CODEC_ID_ADPCM_G726LE, MKBETAG('7','2','6','2') },
    { AV_CODEC_ID_NONE,       0 },
};

#if CONFIG_AU_DEMUXER

static int au_probe(AVProbeData *p)
{
    if (p->buf[0] == '.' && p->buf[1] == 's' &&
        p->buf[2] == 'n' && p->buf[3] == 'd')
        return AVPROBE_SCORE_MAX;
    else
        return 0;
}

static int au_read_annotation(AVFormatContext *s, int size)
{
    static const char * keys[] = {
        "title",
        "artist",
        "album",
        "track",
        "genre",
        NULL };
    AVIOContext *pb = s->pb;
    enum { PARSE_KEY, PARSE_VALUE, PARSE_FINISHED } state = PARSE_KEY;
    char c;
    AVBPrint bprint;
    char * key = NULL;
    char * value = NULL;
    int i;

    av_bprint_init(&bprint, 64, AV_BPRINT_SIZE_UNLIMITED);

    while (size-- > 0) {
        if (avio_feof(pb)) {
            av_bprint_finalize(&bprint, NULL);
            av_freep(&key);
            return AVERROR_EOF;
        }
        c = avio_r8(pb);
        switch(state) {
        case PARSE_KEY:
            if (c == '\0') {
                state = PARSE_FINISHED;
            } else if (c == '=') {
                av_bprint_finalize(&bprint, &key);
                av_bprint_init(&bprint, 64, AV_BPRINT_SIZE_UNLIMITED);
                state = PARSE_VALUE;
            } else {
                av_bprint_chars(&bprint, c, 1);
            }
            break;
        case PARSE_VALUE:
            if (c == '\0' || c == '\n') {
                if (av_bprint_finalize(&bprint, &value) != 0) {
                    av_log(s, AV_LOG_ERROR, "Memory error while parsing AU metadata.\n");
                } else {
                    av_bprint_init(&bprint, 64, AV_BPRINT_SIZE_UNLIMITED);
                    for (i = 0; keys[i] != NULL && key != NULL; i++) {
                        if (av_strcasecmp(keys[i], key) == 0) {
                            av_dict_set(&(s->metadata), keys[i], value, AV_DICT_DONT_STRDUP_VAL);
                            av_freep(&key);
                            value = NULL;
                        }
                    }
                }
                av_freep(&key);
                av_freep(&value);
                state = (c == '\0') ? PARSE_FINISHED : PARSE_KEY;
            } else {
                av_bprint_chars(&bprint, c, 1);
            }
            break;
        case PARSE_FINISHED:
            break;
        default:
            /* should never happen */
            av_assert0(0);
        }
    }
    av_bprint_finalize(&bprint, NULL);
    av_freep(&key);
    return 0;
}

#define BLOCK_SIZE 1024

static int au_read_header(AVFormatContext *s)
{
    int size, data_size = 0;
    unsigned int tag;
    AVIOContext *pb = s->pb;
    unsigned int id, channels, rate;
    int bps;
    enum AVCodecID codec;
    AVStream *st;

    tag = avio_rl32(pb);
    if (tag != MKTAG('.', 's', 'n', 'd'))
        return AVERROR_INVALIDDATA;
    size = avio_rb32(pb); /* header size */
    data_size = avio_rb32(pb); /* data size in bytes */

    if (data_size < 0 && data_size != AU_UNKNOWN_SIZE) {
        av_log(s, AV_LOG_ERROR, "Invalid negative data size '%d' found\n", data_size);
        return AVERROR_INVALIDDATA;
    }

    id       = avio_rb32(pb);
    rate     = avio_rb32(pb);
    channels = avio_rb32(pb);

    if (size > 24) {
        /* parse annotation field to get metadata */
        au_read_annotation(s, size - 24);
    }

    codec = ff_codec_get_id(codec_au_tags, id);

    if (codec == AV_CODEC_ID_NONE) {
        avpriv_request_sample(s, "unknown or unsupported codec tag: %u", id);
        return AVERROR_PATCHWELCOME;
    }

    bps = av_get_bits_per_sample(codec);
    if (codec == AV_CODEC_ID_ADPCM_G726LE) {
        if (id == MKBETAG('7','2','6','2')) {
            bps = 2;
        } else {
            const uint8_t bpcss[] = {4, 0, 3, 5};
            av_assert0(id >= 23 && id < 23 + 4);
            bps = bpcss[id - 23];
        }
    } else if (!bps) {
        avpriv_request_sample(s, "Unknown bits per sample");
        return AVERROR_PATCHWELCOME;
    }

    if (channels == 0 || channels >= INT_MAX / (BLOCK_SIZE * bps >> 3)) {
        av_log(s, AV_LOG_ERROR, "Invalid number of channels %u\n", channels);
        return AVERROR_INVALIDDATA;
    }

    if (rate == 0 || rate > INT_MAX) {
        av_log(s, AV_LOG_ERROR, "Invalid sample rate: %u\n", rate);
        return AVERROR_INVALIDDATA;
    }

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_tag   = id;
    st->codecpar->codec_id    = codec;
    st->codecpar->channels    = channels;
    st->codecpar->sample_rate = rate;
    st->codecpar->bits_per_coded_sample = bps;
    st->codecpar->bit_rate    = channels * rate * bps;
    st->codecpar->block_align = FFMAX(bps * st->codecpar->channels / 8, 1);
    if (data_size != AU_UNKNOWN_SIZE)
        st->duration = (((int64_t)data_size)<<3) / (st->codecpar->channels * (int64_t)bps);

    st->start_time = 0;
    avpriv_set_pts_info(st, 64, 1, rate);

    return 0;
}

AVInputFormat ff_au_demuxer = {
    .name        = "au",
    .long_name   = NULL_IF_CONFIG_SMALL("Sun AU"),
    .read_probe  = au_probe,
    .read_header = au_read_header,
    .read_packet = ff_pcm_read_packet,
    .read_seek   = ff_pcm_read_seek,
    .codec_tag   = (const AVCodecTag* const []) { codec_au_tags, 0 },
};

#endif /* CONFIG_AU_DEMUXER */

#if CONFIG_AU_MUXER

typedef struct AUContext {
    uint32_t header_size;
} AUContext;

#include "rawenc.h"

static int au_get_annotations(AVFormatContext *s, char **buffer)
{
    static const char * keys[] = {
        "Title",
        "Artist",
        "Album",
        "Track",
        "Genre",
        NULL };
    int i;
    int cnt = 0;
    AVDictionary *m = s->metadata;
    AVDictionaryEntry *t = NULL;
    AVBPrint bprint;

    av_bprint_init(&bprint, 64, AV_BPRINT_SIZE_UNLIMITED);

    for (i = 0; keys[i] != NULL; i++) {
        t = av_dict_get(m, keys[i], NULL, 0);
        if (t != NULL) {
            if (cnt++)
                av_bprint_chars(&bprint, '\n', 1);
            av_bprint_append_data(&bprint, keys[i], strlen(keys[i]));
            av_bprint_chars(&bprint, '=', 1);
            av_bprint_append_data(&bprint, t->value, strlen(t->value));
        }
    }
    /* pad with 0's */
    av_bprint_append_data(&bprint, "\0\0\0\0\0\0\0\0", 8);
    return av_bprint_finalize(&bprint, buffer);
}

static int au_write_header(AVFormatContext *s)
{
    int ret;
    AUContext *au = s->priv_data;
    AVIOContext *pb = s->pb;
    AVCodecParameters *par = s->streams[0]->codecpar;
    char *annotations = NULL;

    au->header_size = AU_DEFAULT_HEADER_SIZE;

    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR, "only one stream is supported\n");
        return AVERROR(EINVAL);
    }

    par->codec_tag = ff_codec_get_tag(codec_au_tags, par->codec_id);
    if (!par->codec_tag) {
        av_log(s, AV_LOG_ERROR, "unsupported codec\n");
        return AVERROR(EINVAL);
    }

    if (av_dict_count(s->metadata) > 0) {
        ret = au_get_annotations(s, &annotations);
        if (ret < 0)
            return ret;
        if (annotations != NULL) {
            au->header_size = (24 + strlen(annotations) + 8) & ~7;
            if (au->header_size < AU_DEFAULT_HEADER_SIZE)
                au->header_size = AU_DEFAULT_HEADER_SIZE;
        }
    }
    ffio_wfourcc(pb, ".snd");                   /* magic number */
    avio_wb32(pb, au->header_size);             /* header size */
    avio_wb32(pb, AU_UNKNOWN_SIZE);             /* data size */
    avio_wb32(pb, par->codec_tag);              /* codec ID */
    avio_wb32(pb, par->sample_rate);
    avio_wb32(pb, par->channels);
    if (annotations != NULL) {
        avio_write(pb, annotations, au->header_size - 24);
        av_freep(&annotations);
    } else {
        avio_wb64(pb, 0); /* annotation field */
    }
    avio_flush(pb);

    return 0;
}

static int au_write_trailer(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    AUContext *au = s->priv_data;
    int64_t file_size = avio_tell(pb);

    if (s->pb->seekable && file_size < INT32_MAX) {
        /* update file size */
        avio_seek(pb, 8, SEEK_SET);
        avio_wb32(pb, (uint32_t)(file_size - au->header_size));
        avio_seek(pb, file_size, SEEK_SET);
        avio_flush(pb);
    }

    return 0;
}

AVOutputFormat ff_au_muxer = {
    .name          = "au",
    .long_name     = NULL_IF_CONFIG_SMALL("Sun AU"),
    .mime_type     = "audio/basic",
    .extensions    = "au",
    .priv_data_size = sizeof(AUContext),
    .audio_codec   = AV_CODEC_ID_PCM_S16BE,
    .video_codec   = AV_CODEC_ID_NONE,
    .write_header  = au_write_header,
    .write_packet  = ff_raw_write_packet,
    .write_trailer = au_write_trailer,
    .codec_tag     = (const AVCodecTag* const []) { codec_au_tags, 0 },
    .flags         = AVFMT_NOTIMESTAMPS,
};

#endif /* CONFIG_AU_MUXER */
