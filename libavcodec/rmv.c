/*
 * Retro Motion Video (RMV) common values.
 * Copyright (c) 2012 Hans-Kristian Arntzen
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

#include <stdio.h>
#include <stdlib.h>

#include "libavutil/common.h"
#include "avcodec.h"
#include "rmv.h"

typedef struct RmvContext
{
   AVCodecContext *avctx;
   AVFrame pic;
} RmvContext;

static int rmv_decode_intra(RmvContext *c)
{
   return 0;
}

static int rmv_decode_inter(RmvContext *c)
{
   return 0;
}

static int decode_frame(AVCodecContext *avctx, void *data, int *data_size, AVPacket *avpkt)
{
   return buf_size;
}

static av_cold int decode_init(AVCodecContext *avctx)
{
   return 0;
}

static av_cold int decode_end(AVCodecContext *avctx)
{
   return 0;
}

AVCodec ff_rmv_decoder = {
   .name           = "rmv",
   .type           = AVMEDIA_TYPE_VIDEO,
   .id             = AV_CODEC_ID_RMV,
   .priv_data_size = sizeof(RmvContext),
   .init           = decode_init,
   .close          = decode_end,
   .decode         = decode_frame,
   .capabilities   = CODEC_CAP_DR1,
   .long_name      = NULL_IF_CONFIG_SMALL("Retro Motion Video"),
};

