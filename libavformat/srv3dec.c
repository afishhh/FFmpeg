/*
 * Copyright (c) 2024 Hubert Głuchowski
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

/**
 * @file
 * SRV3/YTT subtitle demuxer
 * This is a youtube specific subtitle format that utilizes XML.
 * Because there is currently no official documentation some information about the format,
 * some information was acquired by reading YTSubConverter code.
 * @see https://github.com/arcusmaximus/YTSubConverter
 */

#include <libxml/parser.h>
#include <libxml/tree.h>
#include "srv3.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "subtitles.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"

typedef struct SRV3GlobalSegments {
    SRV3Segment *list;
    struct SRV3GlobalSegments *next;
} SRV3GlobalSegments;

typedef struct SRV3Context {
    const AVClass *class;
    FFDemuxSubtitlesQueue q;
    SRV3Pen *pens;
    SRV3WindowPos *wps;
    SRV3GlobalSegments *segments;
} SRV3Context;

static SRV3Pen srv3_default_pen = {
    .id = -1,

    .font_size = 100,
    .font_style = 0,
    .attrs = 0,

    .edge_type = 0,
    .edge_color = 0x020202,

    .ruby_part = SRV3_RUBY_NONE,

    .foreground_color = 0xFFFFFF,
    .foreground_alpha = 254,
    .background_color = 0x080808,
    .background_alpha = 192,

    .next = NULL
};

static void srv3_free_context_data(SRV3Context *ctx) {
    void *next;

#define FREE_LIST(type, list, until)                     \
do {                                                                \
    for (void *current = list; current && current != until; current = next) {  \
        next = ((type*)current)->next;                              \
        av_free(current);                                           \
    }                                                               \
} while(0)

    FREE_LIST(SRV3Pen, ctx->pens, &srv3_default_pen);
    FREE_LIST(SRV3WindowPos, ctx->wps, NULL);

    for (SRV3GlobalSegments *segments = ctx->segments; segments; segments = next) {
        FREE_LIST(SRV3Segment, segments->list, NULL);
        next = segments->next;
        av_free(segments);
    }
}

static SRV3Pen *srv3_get_pen(SRV3Context *ctx, int id) {
    for (SRV3Pen *pen = ctx->pens; pen; pen = pen->next)
        if (pen->id == id)
            return pen;
    return NULL;
}

static int srv3_probe(const AVProbeData *p)
{
    if (strstr(p->buf, "<timedtext format=\"3\">"))
        return AVPROBE_SCORE_MAX;

    return 0;
}

static int srv3_parse_numeric_value(SRV3Context *ctx, const char *parent, const char *name, const char *value, int base, int *out, int min, int max)
{
    char *endptr;
    long parsed;

    parsed = strtol(value, &endptr, base);

    if (*endptr != 0) {
        av_log(ctx, AV_LOG_WARNING, "Failed to parse value \"%s\" of %s attribute %s as an integer\n", value, parent, name);
        return AVERROR_INVALIDDATA;
    } else if (parsed < min || parsed > max) {
        av_log(ctx, AV_LOG_WARNING, "Value %li out of range for %s attribute %s ([%i, %i])\n", parsed, parent, name, min, max);
        return AVERROR(ERANGE);
    } else if(out) {
        *out = parsed;
        return 0;
    } else return parsed;
}

static int srv3_parse_numeric_attr(SRV3Context *ctx, const char *parent, xmlAttrPtr attr, int *out, int min, int max)
{
    return srv3_parse_numeric_value(ctx, parent, attr->name, attr->children->content, 10, out, min, max) == 0;
}

static void srv3_parse_color_attr(SRV3Context *ctx, const char *parent, xmlAttrPtr attr, int *out)
{
    srv3_parse_numeric_value(ctx, parent, attr->name, attr->children->content + (*attr->children->content == '#'), 16, out, 0, 0xFFFFFF);
}

static int srv3_read_pen(SRV3Context *ctx, xmlNodePtr element)
{
    SRV3Pen *pen = av_malloc(sizeof(SRV3Pen));
    if (!pen)
        return AVERROR(ENOMEM);
    memcpy(pen, &srv3_default_pen, sizeof(SRV3Pen));
    pen->next = ctx->pens;
    ctx->pens = pen;

    for (xmlAttrPtr attr = element->properties; attr; attr = attr->next) {
        if (!strcmp(attr->name, "id"))
            srv3_parse_numeric_attr(ctx, "pen", attr, &pen->id, 0, INT_MAX);
        else if (!strcmp(attr->name, "sz"))
            srv3_parse_numeric_attr(ctx, "pen", attr, &pen->font_size, 0, INT_MAX);
        else if (!strcmp(attr->name, "fs"))
            srv3_parse_numeric_attr(ctx, "pen", attr, &pen->font_style, 1, 7);
        else if (!strcmp(attr->name, "et"))
            srv3_parse_numeric_attr(ctx, "pen", attr, &pen->edge_type, 1, 4);
        else if (!strcmp(attr->name, "ec"))
            srv3_parse_color_attr(ctx, "pen", attr, &pen->edge_color);
        else if (!strcmp(attr->name, "fc"))
            srv3_parse_color_attr(ctx, "pen", attr, &pen->foreground_color);
        else if (!strcmp(attr->name, "fo"))
            srv3_parse_numeric_attr(ctx, "pen", attr, &pen->foreground_alpha, 0, 0xFF);
        else if (!strcmp(attr->name, "bc"))
            srv3_parse_color_attr(ctx, "pen", attr, &pen->background_color);
        else if (!strcmp(attr->name, "bo"))
            srv3_parse_numeric_attr(ctx, "pen", attr, &pen->background_alpha, 0, 0xFF);
        else if (!strcmp(attr->name, "rb")) {
            srv3_parse_numeric_attr(ctx, "pen", attr, &pen->ruby_part, 0, 5);
            /*
            * For whatever reason three seems to be an unused value for this enum.
            */
            if (pen->ruby_part == 3) {
                pen->ruby_part = 0;
                av_log(ctx, AV_LOG_WARNING, "Encountered unknown ruby part 3\n");
            }
        } else if (!strcmp(attr->name, "i"))
            pen->attrs |= (!strcmp(attr->children->content, "1")) * SRV3_PEN_ATTR_ITALIC;
        else if (!strcmp(attr->name, "b"))
            pen->attrs |= (!strcmp(attr->children->content, "1")) * SRV3_PEN_ATTR_BOLD;
        else {
            av_log(ctx, AV_LOG_WARNING, "Unhandled pen property %s\n", attr->name);
            continue;
        }
    }

    return 0;
}

static int srv3_read_window_pos(SRV3Context *ctx, xmlNodePtr element)
{
    SRV3WindowPos *wp = av_mallocz(sizeof(SRV3Pen));
    if (!wp)
        return AVERROR(ENOMEM);
    wp->next = ctx->wps;
    ctx->wps = wp;

    for (xmlAttrPtr attr = element->properties; attr; attr = attr->next) {
        if (!strcmp(attr->name, "id"))
            srv3_parse_numeric_attr(ctx, "window pos", attr, &wp->id, 0, INT_MAX);
        else if (!strcmp(attr->name, "ap"))
            srv3_parse_numeric_attr(ctx, "window pos", attr, &wp->point, 0, 8);
        else if (!strcmp(attr->name, "ah"))
            srv3_parse_numeric_attr(ctx, "window pos", attr, &wp->x, 0, 100);
        else if (!strcmp(attr->name, "av"))
            srv3_parse_numeric_attr(ctx, "window pos", attr, &wp->y, 0, 100);
        else {
            av_log(ctx, AV_LOG_WARNING, "Unhandled window pos property %s\n", attr->name);
            continue;
        }
    }

    return 0;
}

static int srv3_read_pens(SRV3Context *ctx, xmlNodePtr head)
{
    int ret;

    for (xmlNodePtr element = head->children; element; element = element->next) {
        if (!strcmp(element->name, "pen")) {
            if ((ret = srv3_read_pen(ctx, element)) < 0)
                return ret;
        } else if (!strcmp(element->name, "wp")) {
            if ((ret = srv3_read_window_pos(ctx, element)) < 0)
                return ret;
        }
    }

    return 0;
}

#define ZERO_WIDTH_SPACE "\u200B"
#define YTSUBCONV_PADDING_SPACE ZERO_WIDTH_SPACE " " ZERO_WIDTH_SPACE

static int srv3_clean_segment_text(char *text) {
    char *out = text, *start = text;

    while (1) {
        char *zw = strstr(start, ZERO_WIDTH_SPACE);
        char *pad = strstr(start, YTSUBCONV_PADDING_SPACE);
        char *end = pad ? pad : zw;
        unsigned cnt = end ? (unsigned)(end - start) : (unsigned)strlen(start);

        memmove(out, start, cnt);
        out += cnt;
        if (end) {
            if (pad)
                start = pad + strlen(YTSUBCONV_PADDING_SPACE);
            else
                start = zw + strlen(ZERO_WIDTH_SPACE);
        } else break;
    }

    *out = '\0';
    return out - text;
}

static int srv3_read_body(SRV3Context *ctx, xmlNodePtr body)
{
    int ret = 0;
    AVBPrint textbuf;
    char *text;
    AVPacket *sub;
    SRV3WindowPos *wp;
    SRV3EventMeta *event;
    int start, duration;

    av_bprint_init(&textbuf, 0, AV_BPRINT_SIZE_UNLIMITED);

    for (xmlNodePtr element = body->children; element; element = element->next) {
        if (!strcmp(element->name, "p")) {
            SRV3Segment **segments_tail_next, *segments_tail = NULL;
            SRV3GlobalSegments *global_segments;
            int textlen, lastlen = 0;
            SRV3Pen *event_pen = &srv3_default_pen;

            if ((event = av_mallocz(sizeof(SRV3EventMeta))) == NULL) {
                ret = AVERROR(ENOMEM);
                goto end;
            }

            segments_tail_next = &event->segments;

            for (xmlAttrPtr attr = element->properties; attr; attr = attr->next) {
                if (!strcmp(attr->name, "t"))
                    srv3_parse_numeric_attr(ctx, "event", attr, &start, 0, INT_MAX);
                else if (!strcmp(attr->name, "d"))
                    srv3_parse_numeric_attr(ctx, "event", attr, &duration, 0, INT_MAX);
                else if (!strcmp(attr->name, "wp")) {
                    int id;
                    srv3_parse_numeric_attr(ctx, "event", attr, &id, 0, INT_MAX);
                    for (wp = ctx->wps; wp; wp = wp->next)
                        if (wp->id == id) {
                            event->wp = wp;
                            break;
                        }
                    if (!event->wp)
                        av_log(ctx, AV_LOG_WARNING, "Non-existent window pos %i assigned to event\n", id);
                } else if (!strcmp(attr->name, "p")) {
                    int id;
                    if(srv3_parse_numeric_attr(ctx, "event", attr, &id, 0, INT_MAX)) {
                        SRV3Pen *pen = srv3_get_pen(ctx, id);
                        if(pen)
                            event_pen = pen;
                        else
                            av_log(ctx, AV_LOG_WARNING, "Non-existent pen %i assigned to event\n", id);
                    }
                } else if (!strcmp(attr->name, "ws")) {
                    // TODO: Handle window styles
                } else {
                    av_log(ctx, AV_LOG_WARNING, "Unhandled event property %s\n", attr->name);
                    continue;
                }
            }

            for (xmlNodePtr node = element->children; node; node = node->next) {
                SRV3Segment *segment;

                if (node->type != XML_ELEMENT_NODE && node->type != XML_TEXT_NODE) {
                    av_log(ctx, AV_LOG_WARNING, "Unexpected event child node type %i\n", node->type);
                    continue;
                } else if(node->type == XML_ELEMENT_NODE && strcmp(node->name, "s")) {
                    av_log(ctx, AV_LOG_WARNING, "Unknown event child node name %s\n", node->name);
                    continue;
                } else if (node->type == XML_ELEMENT_NODE && !node->children)
                    continue;

                segment = av_mallocz(sizeof(SRV3Segment));
                if (!segment) {
                    ret = AVERROR(ENOMEM);
                    goto end;
                }

                segment->pen = event_pen;

                if (node->type == XML_ELEMENT_NODE)
                    for (xmlAttrPtr attr = node->properties; attr; attr = attr->next) {
                        if (!strcmp(attr->name, "p")) {
                            int id;
                            if(srv3_parse_numeric_attr(ctx, "segment", attr, &id, 0, INT_MAX)) {
                                SRV3Pen *pen = srv3_get_pen(ctx, id);
                                if(pen)
                                    segment->pen = pen;
                                else
                                    av_log(ctx, AV_LOG_WARNING, "Non-existent pen %i assigned to segment\n", id);
                            }
                        } else {
                            av_log(ctx, AV_LOG_WARNING, "Unhandled segment property %s\n", attr->name);
                            continue;
                        }
                    }

                text = node->type == XML_ELEMENT_NODE ? node->children->content : node->content;
                textlen = srv3_clean_segment_text(text);

                if (textlen > 0) {
                    for (int i = 0; i < textlen; ++i)
                        if (text[i] != '\n' && text[i] != '\r')
                            goto add_segment;

                    av_bprint_append_data(&textbuf, text, textlen);

                    // If possible append this segment's text to the previous segment
                    // Otherwise leave it here for it to be prepended to the next segment
                    if (segments_tail && (segments_tail->pen->font_size == segment->pen->font_size || segment->next == NULL)) {
                        segments_tail->size += textlen;
                        lastlen = textbuf.len;
                    }
                }

                av_free(segment);
                continue;

add_segment:
                av_bprint_append_data(&textbuf, text, textlen);

                segment->size = textbuf.len - lastlen;
                lastlen = textbuf.len;
                *segments_tail_next = segment;
                segments_tail_next = &segment->next;
                segments_tail = segment;
            }

            if (!av_bprint_is_complete(&textbuf)) {
                ret = AVERROR(ENOMEM);
                goto end;
            }

            global_segments = av_mallocz(sizeof(SRV3GlobalSegments));
            if (!global_segments) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            global_segments->list = event->segments;
            global_segments->next = ctx->segments;
            ctx->segments = global_segments;

            sub = ff_subtitles_queue_insert(&ctx->q, textbuf.str, textbuf.len, 0);
            if (!sub) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            sub->pts = start;
            sub->duration = duration;

            if ((ret = av_packet_add_side_data(sub, AV_PKT_DATA_SRV3_EVENT, (uint8_t*)event, sizeof(SRV3EventMeta))) < 0)
               goto end;

            av_bprint_clear(&textbuf);
        }
    }

end:
    av_bprint_finalize(&textbuf, NULL);
    return ret;
}

static int srv3_read_header(AVFormatContext *s)
{
    int ret = 0;
    SRV3Context *ctx = s->priv_data;
    AVPacketSideData *head_sd;
    SRV3Head *head;
    AVBPrint content;
    xmlDocPtr document = NULL;
    xmlNodePtr root_element;
    AVStream *st;

    av_bprint_init(&content, 0, INT_MAX);

    st = avformat_new_stream(s, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    avpriv_set_pts_info(st, 64, 1, 1000);
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st->codecpar->codec_id   = AV_CODEC_ID_SRV3;
    st->disposition = AV_DISPOSITION_CAPTIONS;

    if (!(head_sd = av_packet_side_data_new(&st->codecpar->coded_side_data, &st->codecpar->nb_coded_side_data, AV_PKT_DATA_SRV3_HEAD, sizeof(SRV3Head), 0))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    head = (SRV3Head*)head_sd->data;

    if ((ret = avio_read_to_bprint(s->pb, &content, SIZE_MAX)) < 0)
        goto end;
    if (!avio_feof(s->pb) || !av_bprint_is_complete(&content)) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    LIBXML_TEST_VERSION;

    document = xmlReadMemory(content.str, content.len, s->url, NULL, 0);

    if (!document) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    root_element = xmlDocGetRootElement(document);

    for (xmlAttrPtr attr = root_element->properties; attr; attr = attr->next) {
        if (!strcmp(attr->name, "format")) {
            if (!attr->children || strcmp(attr->children->content, "3"))
                av_log(s, AV_LOG_WARNING, "Unrecognized timedtext format version: %s\nParsing will still be attempted but may produce unexpected results\n", attr->children->content);
        }
    }

    ctx->pens = &srv3_default_pen;

    for (xmlNodePtr element = root_element->children; element; element = element->next) {
        if (!strcmp(element->name, "head"))
            if ((ret = srv3_read_pens(ctx, element)) < 0)
                goto end;
    }

    for (xmlNodePtr element = root_element->children; element; element = element->next) {
        if (!strcmp(element->name, "body"))
            if ((ret = srv3_read_body(ctx, element)) < 0)
                goto end;
    }

    head->pens = ctx->pens;
    ff_subtitles_queue_finalize(s, &ctx->q);

end:
    xmlFreeDoc(document);
    av_bprint_finalize(&content, NULL);
    return ret;
}

static int srv3_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    SRV3Context *ctx = s->priv_data;
    return ff_subtitles_queue_read_packet(&ctx->q, pkt);
}

static int srv3_read_seek(AVFormatContext *s, int stream_index,
                            int64_t min_ts, int64_t ts, int64_t max_ts, int flags)
{
    SRV3Context *ctx = s->priv_data;
    return ff_subtitles_queue_seek(&ctx->q, s, stream_index,
                                   min_ts, ts, max_ts, flags);
}

static int srv3_read_close(AVFormatContext *s)
{
    SRV3Context *ctx = s->priv_data;
    ff_subtitles_queue_clean(&ctx->q);
    srv3_free_context_data(ctx);
    return 0;
}

#define OFFSET(x) offsetof(SRV3Context, x)
#define KIND_FLAGS AV_OPT_FLAG_SUBTITLE_PARAM|AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    { NULL }
};

static const AVClass srv3_demuxer_class = {
    .class_name  = "SRV3 demuxer",
    .option      = options,
    .version     = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_srv3_demuxer = {
    .p.name         = "srv3",
    .p.long_name    = NULL_IF_CONFIG_SMALL("SRV3 subtitle"),
    .p.extensions   = "srv3",
    .p.priv_class   = &srv3_demuxer_class,
    .priv_data_size = sizeof(SRV3Context),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = srv3_probe,
    .read_header    = srv3_read_header,
    .read_packet    = srv3_read_packet,
    .read_seek2     = srv3_read_seek,
    .read_close     = srv3_read_close,
};
