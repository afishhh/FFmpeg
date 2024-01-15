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

#ifndef AVFORMAT_SRV3_H
#define AVFORMAT_SRV3_H

#include "avformat.h"
#include "internal.h"

enum SRV3PenAttrs {
    SRV3_PEN_ATTR_ITALIC = 1,
    SRV3_PEN_ATTR_BOLD = 2,
};

// https://github.com/arcusmaximus/YTSubConverter/blob/38fb2ab469f37e8f3a5a6a27adf91d9d0e81ea4f/YTSubConverter.Shared/Formats/YttDocument.cs#L1019C14-L1019C14
enum SRV3EdgeType {
    SRV3_EDGE_NONE = 0,
    SRV3_EDGE_HARD_SHADOW = 1,
    SRV3_EDGE_BEVEL = 2,
    SRV3_EDGE_GLOW = 3,
    SRV3_EDGE_SOFT_SHADOW = 4,
};

enum SRV3RubyPart {
    SRV3_RUBY_NONE = 0,
    SRV3_RUBY_BASE = 1,
    SRV3_RUBY_PARENTHESIS = 2,
    SRV3_RUBY_BEFORE = 4,
    SRV3_RUBY_AFTER = 5,
};

typedef struct SRV3Pen {
    int id;

    int font_size, font_style;
    int attrs;

    int edge_type, edge_color;

    int ruby_part;

    int foreground_color, foreground_alpha;
    int background_color, background_alpha;

    struct SRV3Pen *next;
} SRV3Pen;

typedef struct SRV3WindowPos {
    int id;

    int point, x, y;

    struct SRV3WindowPos *next;
} SRV3WindowPos;

typedef struct SRV3Head {
    SRV3Pen *pens;
} SRV3Head;

typedef struct SRV3Segment {
    int size;
    SRV3Pen *pen;

    /*
     * The next segment in the same event.
     */
    struct SRV3Segment *next;
} SRV3Segment;

typedef struct SRV3EventMeta {
    /*
    * An ordered list of segments.
    */
    SRV3Segment *segments;
    SRV3WindowPos *wp;
} SRV3EventMeta;

#endif // AVFORMAT_SRV3_H
