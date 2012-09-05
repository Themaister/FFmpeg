/*
 * Retro Motion Video (RMV) decoder.
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
#include <stdint.h>

#include "libavutil/common.h"
#include "avcodec.h"
#include "rmv.h"

typedef struct RmvContext
{
   AVCodecContext *avctx;
   AVFrame pic;

   int width, height, full_width, full_height;

   uint8_t *planes[4];
   int planes_used;
   int plane_stride;
} RmvContext;

static int decode_intra_plane(RmvContext *c, uint8_t *out_buf, int out_stride, const uint8_t *buffer)
{
   const uint8_t *orig_buffer = buffer;

   memcpy(out_buf, buffer, c->width);
   buffer  += c->width;
   out_buf += out_stride;

   int x = 0;
   int y = 1;

   while (y < c->height)
   {
      int key = *buffer++;

      if (key & 0x80) // Run.
      {
         int run = key & 0x7f;
         int run_to_edge    = FFMIN(c->width - x, run);
         int run_after_edge = run - run_to_edge;

         for (int i = 0; i < run_to_edge; i++)
            out_buf[i] = out_buf[i - out_stride] + buffer[i];

         x      += run_to_edge;
         buffer += run_to_edge;

         if (run_after_edge)
         {
            y++;
            x = 0;
            
            out_buf += out_stride;

            for (int i = 0; i < run_after_edge; i++)
               out_buf[i] = out_buf[i - out_stride] + buffer[i];

            buffer += run_after_edge;
            x      += run_after_edge;
         }
      }
      else // Run with zeroes.
      {
         int run = key & 0x7f;
         int run_to_edge    = FFMIN(c->width - x, run);
         int run_after_edge = run - run_to_edge;

         for (int i = 0; i < run_to_edge; i++)
            out_buf[i] = out_buf[i - out_stride];

         x += run_to_edge;

         if (run_after_edge)
         {
            y++;
            x = 0;
            
            out_buf += out_stride;

            for (int i = 0; i < run_after_edge; i++)
               out_buf[i] = out_buf[i - out_stride];

            x += run_after_edge;
         }
      }
   }

   return buffer - orig_buffer;
}

static int rmv_decode_intra(RmvContext *c, const uint8_t *buffer)
{
   int buffer_used = 0;
   for (int i = 0; i < c->planes_used; i++)
   {
      uint8_t magic = *buffer++;
      uint8_t pred  = *buffer++;
      buffer_used += 2;

      if (magic != 'P') // Magic check.
         return -1;

      if (pred != RMV_INTRA_PRED_UP) // Only supported intra prediction type yet.
         return -1;

      uint8_t *out_buf = c->pic.data[i];
      int out_stride   = c->pic.linesize[i];

      int used = decode_intra_plane(c, out_buf, out_stride, buffer);
      if (used < 0)
         return -1;

      buffer_used += used;
      buffer      += used;

      magic = *buffer++;
      if (magic != 'E') // Another magic check.
         return -1;

      buffer_used++;
   }

   return buffer_used;
}

static int rmv_decode_inter(RmvContext *c, const uint8_t *buffer, int block_size)
{
   int buffer_used = 0;

   for (int i = 0; i < c->planes_used; i++)
   {
      uint8_t magic = *buffer++;
      if (magic != 'P')
         return -1;

      uint8_t *out_buf = c->pic.data[i];
      int out_stride   = c->pic.linesize[i];

      int used = decode_inter_plane(c, out_buf, out_stride, buffer, c->planes[i]);

      magic = *buffer++;
      if (magic != 'E')
         return -1;

      buffer_used++;
   }

   return buffer_used;
}

static void copy_frame_internal(uint8_t **planes, uint8_t **in_planes,
      int width, int height, int out_stride, const int *in_stride, int planes)
{
   for (int i = 0; i < planes; i++)
   {
      uint8_t *out      = planes[i];
      const uint8_t *in = in_planes[i];

      for (int h = 0; h < height; h++; out += out_stride, in += in_stride[i])
         memcpy(out, in, width);
   }
}

static int decode_frame(AVCodecContext *avctx, void *data, int *data_size, AVPacket *avpkt)
{
   const uint8_t *buf = avpkt->data;
   const uint8_t buf_size = avpkt->size;

   RmvContext *c = avctx->priv_data;

   if (c->pic.data[0])
      avctx->release_buffer(avctx, &c->pic);

   c->pic.reference = 3;
   c->pic.buffer_hints = FF_BUFFER_HINTS_VALID;
   int ret = avctx->get_buffer(avctx, &c->pic);
   if (ret < 0)
   {
      av_log(avctx, AV_LOG_ERROR, "get_buffer() failed.\n");
      return ret;
   }

   if (buf_size < 6)
   {
      av_log(avctx, AV_LOG_ERROR, "invalid packet.\n");
      return AVERROR(EINVAL);
   }

   if (memcmp(buf, "RMV", 3))
   {
      av_log(avctx, AV_LOG_ERROR, "packet is not RMV.\n");
      return AVERROR(EINVAL);
   }
   buf += 3;

   uint8_t frame_type = *buf++;
   uint8_t pix_type   = *buf++;
   uint8_t block_size = *buf++;

   if (pix_type != RMV_PIX_FMT_GBRP)
   {
      av_log(avctx, AV_LOG_ERROR, "unsupported pixel format.\n");
      return AVERROR(EINVAL);
   }

   switch (frame_type)
   {
      case RMV_FRAME_INTRA:
         ret = decode_intra(c, buf);
         if (ret < 0)
         {
            av_log(avctx, AV_LOG_ERROR, "failed to decode intra.\n");
            return AVERROR(EINVAL);
         }
         buf += ret;
         break;

      case RMV_FRAME_INTER:
         ret = decode_inter(c, buf, block_size);
         if (ret < 0)
         {
            av_log(avctx, AV_LOG_ERROR, "failed to decode inter.\n");
            return AVERROR(EINVAL);
         }
         buf += ret;
         break;

      default:
         av_log(avctx, AV_LOG_ERROR, "invalid frame type.\n");
         return AVERROR(EINVAL);
   }

   copy_frame_internal(c->planes, c->pic.data,
         c->width, c->height,
         c->plane_stride, c->pic.linesize,
         c->planes_used);

   *data_size      = sizeof(AVFrame);
   *(AVFrame*)data = c->pic;

   return buf_size;
}

static av_cold int decode_init(AVCodecContext *avctx)
{
   RmvContext *c = avctx->priv_data;

   c->avctx  = avctx;
   c->width  = avctx->width;
   c->height = avctx->height;

   c->full_width   = FFALIGN(c->width, 32);
   c->full_height  = FFALIGN(c->height, 32);
   c->plane_stride = c->width;
   avcodec_get_frame_defaults(&c->pic);

   avctx->pix_fmt = PIX_FMT_GBRP;
   c->planes_used = 3;

   for (int i = 0; i < c->planes_used; i++)
   {
      if (!(c->planes[i] = av_malloc(c->plane_stride * c->full_height)))
      {
         av_log(avctx, AV_LOG_ERROR, "failed to allocate memory for codec.\n");
         return AVERROR(ENOMEM);
      }
   }

   return 0;
}

static av_cold int decode_end(AVCodecContext *avctx)
{
   RmvContext *c = avctx->priv_data;

   if (c->pic.data[0])
   {
      avctx->release_buffer(avctx, &c->pic);
   }

   for (int i = 0; i < c->planes_used; i++)
   {
      if (c->planes[i])
         av_freep(&c->planes[i]);
   }

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

