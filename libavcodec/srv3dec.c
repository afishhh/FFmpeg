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
 * SRV3/YTT subtitle decoder
 * @see https://github.com/arcusmaximus/YTSubConverter
 */

#include "avcodec.h"
#include "ass.h"
#include "codec_internal.h"
#include "srv3.h"
#include "libavutil/bprint.h"
#include "version.h"

const int PLAY_RES_X = 1280;
const int PLAY_RES_Y = 720;
const int BASE_FONT_SIZE = 38;

// From https://github.com/arcusmaximus/YTSubConverter/blob/38fb2ab469f37e8f3a5a6a27adf91d9d0e81ea4f/YTSubConverter.Shared/Formats/YttDocument.cs#L1123
static const char *srv3_font_style_to_font_name(int font_style) {
    switch(font_style) {
    case 1:
        return "Courier New";
    case 2:
        return "Times New Roman";
    case 3:
        return "Lucida Console";
    case 4:
        return "Comic Sans Ms";
    case 6:
        return "Monotype Corsiva";
    case 7:
        return "Carrois Gothic Sc";
    default:
        return "Roboto";
    };
}

static int srv3_point_to_ass_alignment(int point) {
    if (point >= 6)
        return point - 5;
    else if (point < 3)
        return point + 7;
    return point + 1;
}

static int srv3_coord_to_ass(int coord, int max) {
    return (2.0 + coord * 0.96) / 100.0 * max;
}

static float srv3_font_size_to_ass(int size) {
    return BASE_FONT_SIZE * (1.0 + ((size / 100.0) - 1.0) / 4.0);
}

#define RGB2BGR(color) (((color) & 0x0000FF) << 16 | ((color) & 0x00FF00) | ((color) & 0xFF0000) >> 16)
#define RGB2ASS(color, alpha) RGB2BGR(color) | ((0xFF - (alpha)) << 24)
#define ASSBOOL(value) ((value) > 0) * -1

static void srv3_style_segment(AVCodecContext *ctx, AVBPrint *buf, SRV3Segment *segment) {
    av_bprintf(buf, "{\\rP%i}", segment->pen->id + 1);

    if (segment->pen->background_alpha == 0) {
        switch(segment->pen->edge_type) {
        case SRV3_EDGE_HARD_SHADOW:
            av_bprintf(buf, "{\\shad2}");
            break;
        /*
         * I think falling back to a glow effect on soft shadow is better than just using a normal shadow.
         * YTSubConverter doesn't agree with me on this and I'm not completely sure whether it's the right choice.
         */
        case SRV3_EDGE_SOFT_SHADOW:
            av_bprintf(buf, "{\\bord2\\blur3}");
            break;
        case SRV3_EDGE_GLOW:
            av_bprintf(buf, "{\\bord1\\blur1}");
            break;
        case SRV3_EDGE_BEVEL:
            av_bprintf(buf, "{\\shad2}");
            break;
        case SRV3_EDGE_NONE:
            break;
        default:
            av_log(ctx, AV_LOG_WARNING, "bug: Unhandled edge type %i in decoder\n", segment->pen->edge_type);
            break;
        }
    } else if (segment->pen->edge_type) {
        /*
         * ASS doesn't support text shadows or outlines with BorderStyle 3.
         * TODO: Add an option to enable BorderStyle 4 usage
         */
    }
}

static void srv3_process_text(AVBPrint *buf, const char *text, int count) {
    for (int i = 0; i < count; ++i) {
        if (text[i] == '\r')
            continue;
        else if (text[i] == '\n')
            av_bprintf(buf, "\\N");
        else
            av_bprintf(buf, "%c", text[i]);
    }
}

static void srv3_position_event(SRV3EventMeta *event, int *x, int *y, int *align) {
    if (event->wp) {
        *x = srv3_coord_to_ass(event->wp->x , PLAY_RES_X);
        *y = srv3_coord_to_ass(event->wp->y, PLAY_RES_Y);
        *align = srv3_point_to_ass_alignment(event->wp->point);
    } else {
        *x = srv3_coord_to_ass(50, PLAY_RES_X);
        *y = srv3_coord_to_ass(100, PLAY_RES_Y);
        *align = 2;
    }
}

static void srv3_event_text_ass(AVCodecContext *ctx, AVBPrint *buf, const char *text, SRV3EventMeta *event)
{
    SRV3Segment *segment;
    int x, y, alignment;

    srv3_position_event(event, &x, &y, &alignment);
    av_bprintf(buf, "{\\an%i\\pos(%i,%i)}", alignment, x, y);

    for (segment = event->segments; segment; segment = segment->next) {
        srv3_style_segment(ctx, buf, segment);
        srv3_process_text(buf, text, segment->size);
        text += segment->size;
    }
}

static int srv3_decode_frame(AVCodecContext *avctx, AVSubtitle *sub,
                             int *got_sub_ptr, const AVPacket *avpkt)
{
    int ret = 0;
    FFASSDecoderContext *ctx = avctx->priv_data;
    const char *text = avpkt->data;
    SRV3EventMeta *event = (SRV3EventMeta*)av_packet_get_side_data(avpkt, AV_PKT_DATA_SRV3_EVENT, NULL);
    AVBPrint buf;

    if (!text || avpkt->size == 0)
        return 0;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    srv3_event_text_ass(avctx, &buf, text, event);
    if (av_bprint_is_complete(&buf))
        ret = ff_ass_add_rect(sub, buf.str, ctx->readorder++, 0, NULL, NULL);
    else
        ret = AVERROR(ENOMEM);

    av_bprint_finalize(&buf, NULL);

    if (ret < 0)
        return ret;
    *got_sub_ptr = sub->num_rects > 0;
    return avpkt->size;
}

static av_cold int srv3_decoder_init(AVCodecContext *avctx) {
    int ret = 0;
    AVBPrint header;
    const AVPacketSideData *head_sd;
    SRV3Pen *pen;

    av_bprint_init(&header, 0, AV_BPRINT_SIZE_UNLIMITED);

    av_bprintf(&header,
               "[Script Info]\r\n"
               "; Script generated by FFmpeg/Lavc%s\r\n"
               "ScriptType: v4.00+\r\n"
               "PlayResX: %i\r\n"
               "PlayResY: %i\r\n"
               "WrapStyle: 0\r\n"
               "ScaledBorderAndShadow: yes\r\n"
               "YCbCr Matrix: None\r\n"
               "\r\n"
               "[V4+ Styles]\r\n"
               "Format: Name, "
               "Fontname, Fontsize, "
               "PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
               "Bold, Italic, Underline, StrikeOut, "
               "ScaleX, ScaleY, "
               "Spacing, Angle, "
               "BorderStyle, Outline, Shadow, "
               "Alignment, MarginL, MarginR, MarginV, "
               "Encoding\r\n",
               !(avctx->flags & AV_CODEC_FLAG_BITEXACT) ? AV_STRINGIFY(LIBAVCODEC_VERSION) : "",
               PLAY_RES_X, PLAY_RES_Y);

    head_sd = av_packet_side_data_get(avctx->coded_side_data, avctx->nb_coded_side_data, AV_PKT_DATA_SRV3_HEAD);
    if (head_sd) {
        for (pen = ((SRV3Head*)head_sd->data)->pens; pen; pen = pen->next)
            av_bprintf(&header,
                       "Style: "
                       "P%i,"                 /* Name */
                       "%s,%f,"               /* Font{name,size} */
                       "&H%x,&H0,&H%x,&H%x,"  /* {Primary,Secondary,Outline,Back}Colour */
                       "%i,%i,0,0,"           /* Bold, Italic, Underline, StrikeOut */
                       "100,100,"             /* Scale{X,Y} */
                       "0,0,"                 /* Spacing, Angle */
                       "%i,%i,0,"             /* BorderStyle, Outline, Shadow */
                       "2,0,0,0,"             /* Alignment, Margin[LRV] */
                       "1\r\n",               /* Encoding */
                       pen->id + 1,
                       srv3_font_style_to_font_name(pen->font_style), srv3_font_size_to_ass(pen->font_size),
                       RGB2ASS(pen->foreground_color, pen->foreground_alpha),
                       pen->background_alpha > 0
                           ? RGB2ASS(pen->background_color, pen->background_alpha)
                           : RGB2ASS(pen->edge_color, pen->foreground_alpha),
                       pen->background_alpha > 0
                           ? RGB2ASS(pen->background_color, pen->background_alpha)
                           : RGB2ASS(pen->edge_color, pen->foreground_alpha),
                       ASSBOOL(pen->attrs & SRV3_PEN_ATTR_BOLD), ASSBOOL(pen->attrs & SRV3_PEN_ATTR_ITALIC),
                       pen->background_alpha > 0 ? 3 : (pen->edge_type > 0), pen->background_alpha > 0);
    }

    av_bprintf(&header,
               "[Events]\r\n"
               "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n");

    av_bprint_finalize(&header, (char**)&avctx->subtitle_header);
    if (!avctx->subtitle_header) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    avctx->subtitle_header_size = header.len;

end:
    av_bprint_finalize(&header, NULL);
    return ret;
}

const FFCodec ff_srv3_decoder = {
    .p.name         = "srv3",
    CODEC_LONG_NAME("SRV3 subtitle"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_SRV3,
    FF_CODEC_DECODE_SUB_CB(srv3_decode_frame),
    .init           = srv3_decoder_init,
    .flush          = ff_ass_decoder_flush,
    .priv_data_size = sizeof(FFASSDecoderContext),
};
