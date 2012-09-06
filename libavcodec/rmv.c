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

static int decode_intra_plane_direct(RmvContext *c, uint8_t *out_buf, int out_stride, const uint8_t *buffer)
{
   for (int h = 0; h < c->height; h++, out_buf += out_stride, buffer += c->width)
      memcpy(out_buf, buffer, c->width);

   return c->width * c->height;
}

static int decode_intra_plane_pred_up_rle(RmvContext *c, uint8_t *out_buf, int out_stride, const uint8_t *buffer)
{
   const uint8_t *orig_buffer = buffer;

   int x = 0;
   int y = 0;

   while (y < c->height)
   {
      uint8_t key = *buffer++;

      if (key & 0x80) // Run.
      {
         int run = key & 0x7f;
         int run_to_edge    = FFMIN(c->width - x, run);
         int run_after_edge = run - run_to_edge;

         if (run_after_edge >= c->width)
         {
            av_log(c->avctx, AV_LOG_ERROR, "Can't run over two scanlines.\n");
            return -1;
         }

         if (y)
         {
            for (int i = 0; i < run_to_edge; i++)
               out_buf[x + i] = out_buf[x + i - out_stride] + buffer[i];
         }
         else
            memcpy(out_buf + x, buffer, run_to_edge);

         x      += run_to_edge;
         buffer += run_to_edge;

         if (run_after_edge)
         {
            out_buf += out_stride;
            y++;
            x = 0;
            
            for (int i = 0; i < run_after_edge; i++)
               out_buf[x + i] = out_buf[x + i - out_stride] + buffer[i];

            buffer += run_after_edge;
            x      += run_after_edge;
         }

         if (x == c->width)
         {
            out_buf += out_stride;
            y++;
            x = 0;
         }
      }
      else // Run with zeroes.
      {
         int run = key & 0x7f;
         int run_to_edge    = FFMIN(c->width - x, run);
         int run_after_edge = run - run_to_edge;

         if (run_after_edge >= c->width)
         {
            av_log(c->avctx, AV_LOG_ERROR, "Can't run over two scanlines.\n");
            return -1;
         }

         if (y)
            memcpy(out_buf + x, out_buf + x - out_stride, run_to_edge);
         else
            memset(out_buf + x, 0, run_to_edge);

         x += run_to_edge;

         if (run_after_edge)
         {
            out_buf += out_stride;
            y++;
            x = 0;

            memcpy(out_buf + x, out_buf + x - out_stride, run_after_edge);
            x += run_after_edge;
         }

         if (x == c->width)
         {
            out_buf += out_stride;
            y++;
            x = 0;
         }
      }
   }

   return buffer - orig_buffer;
}

static int rmv_decode_intra(RmvContext *c, const uint8_t *buffer)
{
   int buffer_used;
   //av_log(c->avctx, AV_LOG_INFO, "Decoding I-frame.\n");

   buffer_used = 0;
   for (int i = 0; i < c->planes_used; i++)
   {
      uint8_t *out_buf;
      int used, out_stride;
      uint8_t magic = *buffer++;
      uint8_t pred  = *buffer++;
      uint32_t size = *buffer++;
      size |= *buffer++ <<  8;
      size |= *buffer++ << 16;
      size |= *buffer++ << 24;
      buffer_used += 6;

      if (magic != 'P') // Magic check.
      {
         av_log(c->avctx, AV_LOG_ERROR, "P magic failed in intra in plane %d.\n", i);
         return -1;
      }

      out_buf    = c->pic.data[i];
      out_stride = c->pic.linesize[i];

      switch (pred)
      {
         case RMV_INTRA_DIRECT:
            used = decode_intra_plane_direct(c, out_buf, out_stride, buffer);
            break;

         case RMV_INTRA_PRED_UP_RLE:
            used = decode_intra_plane_pred_up_rle(c, out_buf, out_stride, buffer);
            break;

         default:
            av_log(c->avctx, AV_LOG_ERROR, "Invalid intra prediction format.\n");
            return -1;
      }

      if (used < 0)
         return -1;

      //av_log(c->avctx, AV_LOG_INFO, "Intra coded size: %d\n", used);

      buffer_used += used;
      buffer      += used;

      //av_log(c->avctx, AV_LOG_INFO, "Expected intra size: %u\n", size);

      magic = *buffer++;
      if (magic != 'E') // Another magic check.
      {
         av_log(c->avctx, AV_LOG_ERROR, "E magic failed in intra in plane %d.\n", i);
         return -1;
      }

      buffer_used++;
   }

   return buffer_used;
}

static int decode_inter_plane(RmvContext *c, uint8_t *out_buf, int out_stride, const uint8_t *buffer,
      const uint8_t *prev, int block_size)
{
   int width  = c->avctx->width;
   int height = c->avctx->height;
   int stride = c->plane_stride;
   int bw = (width + (block_size - 1)) / block_size;
   int bh = (height + (block_size - 1)) / block_size;

   int min_y = 0;
   int max_y = height - block_size;
   int min_x = 0;
   int max_x = width - block_size;

   const uint8_t *mv     = buffer;
   const uint8_t *blocks = mv + bw * bh * 3;

   for (int by = 0; by < bh; by++)
   {
      for (int bx = 0; bx < bw; bx++)
      {
         int x = bx * block_size;
         int y = by * block_size;

         int mv_x         = (int8_t)*mv++;
         int mv_y         = (int8_t)*mv++;
         uint8_t mv_flags = *mv++;

         if (x + mv_x > max_x ||
               x + mv_x < min_x ||
               y + mv_y > max_y ||
               y + mv_y < min_y)
         {
            av_log(c->avctx, AV_LOG_ERROR, "Motion vectors are out of bounds. X = %d, Y = %d, MX = %d, MY = %d\n", x, y, mv_x, mv_y);
            return -1;
         }

         if (mv_flags & RMV_BLOCK_PERFECT)
         {
            uint8_t *dst       = out_buf + x + y * out_stride;
            const uint8_t *src = prev + (x + mv_x) + (y + mv_y) * stride; 

            for (int h = 0; h < block_size; h++, dst += out_stride, src += stride)
               memcpy(dst, src, block_size);
         }
         else if (mv_flags & RMV_BLOCK_ZERO)
         {
            uint8_t *dst = out_buf + x + y * out_stride;
            for (int h = 0; h < block_size; h++, dst += out_stride)
               memset(dst, 0, block_size);
         }
         else if (mv_flags & RMV_BLOCK_ERROR_DIRECT)
         {
            uint8_t *dst       = out_buf + x + y * out_stride;
            const uint8_t *src = prev + (x + mv_x) + (y + mv_y) * stride; 

            for (int h = 0; h < block_size; h++, dst += out_stride, src += stride)
               for (int w = 0; w < block_size; w++)
                  dst[w] = src[w] + *blocks++; 
         }
         else if (mv_flags & RMV_BLOCK_DIRECT)
         {
            uint8_t *dst = out_buf + x + y * out_stride;

            for (int h = 0; h < block_size; h++, dst += out_stride)
               for (int w = 0; w < block_size; w++)
                  dst[w] = *blocks++; 
         }
         else
         {
            av_log(c->avctx, AV_LOG_ERROR, "Block format not supported.\n");
            return -1;
         }
      }
   }

   return blocks - buffer; 
}

static int rmv_decode_inter(RmvContext *c, const uint8_t *buffer, int block_size)
{
   int buffer_used = 0;
   //av_log(c->avctx, AV_LOG_INFO, "Decoding P-frame.\n");

   for (int i = 0; i < c->planes_used; i++)
   {
#if 0
      uint8_t *out_buf = c->pic.data[i];
      int out_stride = c->pic.linesize[i];
      for (int h = 0; h < c->height; h++, out_buf += out_stride)
         memset(out_buf, 0x10, c->width);
#endif

#if 1
      uint8_t *out_buf;
      int used, out_stride;
      uint8_t magic = *buffer++;
      if (magic != 'P')
         return -1;
      buffer_used++;

      out_buf    = c->pic.data[i];
      out_stride = c->pic.linesize[i];

      used = decode_inter_plane(c, out_buf, out_stride, buffer, c->planes[i], block_size);
      if (used < 0)
         return -1;

      buffer      += used;
      buffer_used += used;

      magic = *buffer++;
      if (magic != 'E')
         return -1;

      buffer_used++;
#endif
   }

   return buffer_used;
}

static void copy_frame_internal(uint8_t **planes, uint8_t **in_planes,
      int width, int height, int out_stride, const int *in_stride, int num_planes)
{
   for (int i = 0; i < num_planes; i++)
   {
      uint8_t *out      = planes[i];
      const uint8_t *in = in_planes[i];

      for (int h = 0; h < height; h++, out += out_stride, in += in_stride[i])
         memcpy(out, in, width);
   }
}

static int decode_frame(AVCodecContext *avctx, void *data, int *data_size, AVPacket *avpkt)
{
   uint8_t frame_type, pix_type, block_size;
   int ret = 0;
   const uint8_t *buf = avpkt->data;
   const uint8_t buf_size = avpkt->size;

   RmvContext *c = avctx->priv_data;

   if (buf_size < 6)
   {
      av_log(avctx, AV_LOG_ERROR, "invalid packet (got size: %d).\n", buf_size);
      *data_size      = sizeof(AVFrame);
      *(AVFrame*)data = c->pic;
      return 0;
   }

   if (memcmp(buf, "RMV", 3))
   {
      av_log(avctx, AV_LOG_ERROR, "packet is not RMV.\n");
      return AVERROR(EINVAL);
   }
   buf += 3;

   frame_type = *buf++;
   pix_type   = *buf++;
   block_size = *buf++;

   //av_log(avctx, AV_LOG_INFO, "Frame type = %d, Pix type = %d, Block size = %d\n", frame_type, pix_type, block_size);

   if (pix_type != RMV_PIX_FMT_GBRP)
   {
      av_log(avctx, AV_LOG_ERROR, "unsupported pixel format.\n");
      return AVERROR(EINVAL);
   }

   switch (frame_type)
   {
      case RMV_FRAME_INTRA:
         c->pic.key_frame = 1;
         c->pic.pict_type = AV_PICTURE_TYPE_I;
         ret = rmv_decode_intra(c, buf);
         if (ret < 0)
         {
            av_log(avctx, AV_LOG_ERROR, "failed to decode intra.\n");
            return AVERROR(EINVAL);
         }
         buf += ret;
         break;

      case RMV_FRAME_INTER:
         c->pic.key_frame = 0;
         c->pic.pict_type = AV_PICTURE_TYPE_P;
         ret = rmv_decode_inter(c, buf, block_size);
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

   //av_log(avctx, AV_LOG_INFO, "Decoded %d bytes from %d packet bytes.\n", (int)(buf - avpkt->data), avpkt->size);

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
   int ret;
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

      memset(c->planes[i], 0, c->plane_stride * c->full_height);
   }

   ret = avctx->get_buffer(avctx, &c->pic);
   if (ret < 0)
   {
      av_log(avctx, AV_LOG_ERROR, "get_buffer() failed.\n");
      return ret;
   }

   return 0;
}

static av_cold int decode_end(AVCodecContext *avctx)
{
   RmvContext *c = avctx->priv_data;

   if (c->pic.data[0])
      avctx->release_buffer(avctx, &c->pic);

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

