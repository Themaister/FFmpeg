/*
 * Retro Motion Video (RMV) encoder
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
#include <stdbool.h>
#include <stdint.h>

#include "libavutil/common.h"
#include "avcodec.h"
#include "internal.h"
#include "rmv.h"

#define RMV_BLOCK_SIZE 16

typedef struct RmvEncContext
{
   AVCodecContext *avctx;
   AVFrame pic;

   int frame_cnt;
   int frame_per_key;

   int full_width, full_height;

   int me_range;

   int pred_perfect;
   int pred_error;

   uint8_t *planes[4];
   uint8_t *planes_prev[4];

   uint8_t *comp_buf;
   uint8_t *comp_ptr;
   int comp_size;

   int planes_used;
   int plane_stride;
} RmvEncContext;

static void interleave_frame_bgr24(RmvEncContext *c, uint8_t **planes, const uint8_t *input, int width, int height, int stride)
{
   uint8_t *out_g = planes[0];
   uint8_t *out_b = planes[1];
   uint8_t *out_r = planes[2];

   for (int y = 0; y < height; y++,
         input += stride, out_g += c->plane_stride, out_b += c->plane_stride, out_r += c->plane_stride)
   {
      for (int x = 0; x < width; x++)
      {
         out_b[x] = input[3 * x + 0];
         out_g[x] = input[3 * x + 1];
         out_r[x] = input[3 * x + 2];
      }
   }
}

static void encode_intra_plane(RmvEncContext *c, const uint8_t *plane, uint8_t *plane_buf)
{
   int i, len;
   size_t size;
   uint8_t *rle_buffer = plane_buf;
   int width  = c->avctx->width;
   int height = c->avctx->height;
   int stride = c->plane_stride;
   uint8_t *size_buf;

   uint8_t *init_comp_ptr = c->comp_ptr;

   *c->comp_ptr++ = 'P';
   *c->comp_ptr++ = RMV_INTRA_PRED_UP_RLE;

   // Reserve 4 bytes to use later for intra plane size encoding.
   size_buf = c->comp_ptr;
   c->comp_ptr      += 4;

   // Very simple prediction. Assume that each pixel is equal to pixel above. Encode error.
   // Encode error data into planes_prev, as it contains useless data anyways.
   // From this data, simple RLE coding is performed.

   // First scanline can't be predicted.
   memcpy(plane_buf, plane, width);

   plane_buf += width;
   plane     += stride;

   for (int h = 1; h < height; h++, plane += stride, plane_buf += width)
      for (int w = 0; w < width; w++)
         plane_buf[w] = plane[w] - plane[w - stride];

   i = 0;
   len = height * width;

   // Simple RLE encoding.
   // A bunch of zeros after each other are encoded as number of zeros with MSB set to 0.
   // A run of raw data is encoded as a single byte, with MSB set to 1.
   // The 7 LSB encode how many bytes should be read directly.
   while (i < len)
   {
      if (rle_buffer[i] == 0) // Check how far we can go with zeros ...
      {
         uint8_t max_len = FFMIN(len - i, 127);

         uint8_t j;
         for (j = 0; rle_buffer[i + j] == 0 && j < max_len; j++);

         *c->comp_ptr++ = j;
         i += j;
      }
      else // Check how far we should go with non-zeros ...
      {
         uint8_t max_len = FFMIN(len - i, 127);

         uint8_t j;
         for (j = 0; rle_buffer[i + j] != 0 && j < max_len; j++);
         
         *c->comp_ptr++ = 0x80 | j;
         memcpy(c->comp_ptr, rle_buffer + i, j);

         i += j;
         c->comp_ptr += j;
      }
   }

   // Make sure that plane_prev stays pristine when it's later used as a real prev frame.
   memset(rle_buffer, 0, height * stride);

   *c->comp_ptr++ = 'E';

   size = c->comp_ptr - init_comp_ptr; 
   *size_buf++ = (size >>  0) & 0xff;
   *size_buf++ = (size >>  8) & 0xff;
   *size_buf++ = (size >> 16) & 0xff;
   *size_buf++ = (size >> 24) & 0xff;
}

static void encode_intra(RmvEncContext *c)
{
   //av_log(c->avctx, AV_LOG_INFO, "encode I-frame\n");
   *c->comp_ptr++ = 'R';
   *c->comp_ptr++ = 'M';
   *c->comp_ptr++ = 'V';
   *c->comp_ptr++ = RMV_FRAME_INTRA;
   *c->comp_ptr++ = RMV_PIX_FMT_GBRP;
   *c->comp_ptr++ = RMV_BLOCK_SIZE;

   for (int i = 0; i < c->planes_used; i++)
      encode_intra_plane(c, c->planes[i], c->planes_prev[i]);
}

static int calc_block_sum(const uint8_t *data, int stride)
{
   int sum = 0;
   for (int h = 0; h < RMV_BLOCK_SIZE; h++, data += stride)
      for (int w = 0; w < RMV_BLOCK_SIZE; w++)
         sum += data[w];

   return sum;
}

static void calc_block_error(uint8_t *error, const uint8_t *ref, const uint8_t *ref_prev, int stride)
{
   for (int h = 0; h < RMV_BLOCK_SIZE; h++, ref += stride, ref_prev += stride)
      for (int w = 0; w < RMV_BLOCK_SIZE; w++)
         *error++ = ref[w] - ref_prev[w];
}

#ifdef __SSE2__
#include <emmintrin.h>
static int block_sad(const uint8_t *ref, const uint8_t *ref_prev, int stride)
{
   int sad = 0;
   for (int h = 0; h < RMV_BLOCK_SIZE; h++, ref += stride, ref_prev += stride)
   {
      __m128i res = _mm_sad_epu8(_mm_load_si128((const __m128i*)ref), _mm_loadu_si128((const __m128i*)ref_prev));
      sad += _mm_extract_epi16(res, 0) + _mm_extract_epi16(res, 4);
   }

   return sad;
}
#else
// TODO: GREAT assembly target :D
static int block_sad(const uint8_t *ref, const uint8_t *ref_prev, int stride)
{
   int sad = 0;
   for (int h = 0; h < RMV_BLOCK_SIZE; h++, ref += stride, ref_prev += stride)
      for (int w = 0; w < RMV_BLOCK_SIZE; w++)
         sad += abs(ref[w] - ref_prev[w]);

   return sad;
}
#endif

static int encode_inter_block(RmvEncContext *c, uint8_t *mv, uint8_t *comp,
      int bx, int by, const uint8_t *cur, const uint8_t *prev)
{
   const uint8_t *ref_prev;
   int x = bx * RMV_BLOCK_SIZE;
   int y = by * RMV_BLOCK_SIZE;
   int sad;
   int sx, sy;

   int min_sy = FFMAX(y - c->me_range, 0);
   int max_sy = FFMIN(y + c->me_range, c->full_height - RMV_BLOCK_SIZE);
   int min_sx = FFMAX(x - c->me_range, 0);
   int max_sx = FFMIN(x + c->me_range, c->full_width - RMV_BLOCK_SIZE);

   const uint8_t *ref = cur  + x + y * c->plane_stride;
   int block_sum = calc_block_sum(ref, c->plane_stride);

   int mv_x = 0;
   int mv_y = 0;

   if (block_sum == 0) // Can encode block as all 0. Yay.
   {
      mv[0] = 0;
      mv[1] = 0;
      mv[2] = RMV_BLOCK_ZERO;
      return 0;
   }
   else // Motion estimate. Use very computationally simple SAD (sum absolute differences) to estimate.
   {
      int min_sad = block_sad(ref, prev + x + y * c->plane_stride, c->plane_stride);
      if (min_sad)
      {
         for (sy = min_sy; sy <= max_sy; sy++)
         {
            for (sx = min_sx; sx < max_sx; sx++)
            {
               if (sx == 0 && sy == 0)
                  continue;

               ref_prev = prev + sx + sy * c->plane_stride;

               sad = block_sad(ref, ref_prev, c->plane_stride);

               if (sad < min_sad)
               {
                  mv_x = sx - x;
                  mv_y = sy - y;

                  min_sad = sad;
               }

               if (min_sad == 0) // We've found a perfect target.
                  goto out;
            }
         }
      }
out:
      if (min_sad == 0) // We've perfectly predicted the block. Pop the champangne! :D
      {
         c->pred_perfect++;
         mv[0] = mv_x;
         mv[1] = mv_y;
         mv[2] = RMV_BLOCK_PERFECT;
         return 0;
      }
      else // Entropy code this 128 pixel beast.
      {
         c->pred_error++;
         mv[0] = mv_x;
         mv[1] = mv_y;
#if 0
         mv[2] = RMV_BLOCK_DIRECT; // Just bang out all 128 pixels for now :(

         for (int h = 0; h < RMV_BLOCK_SIZE; h++, ref += c->plane_stride)
            for (int w = 0; w < RMV_BLOCK_SIZE; w++)
               *comp++ = ref[w];
         return RMV_BLOCK_SIZE * RMV_BLOCK_SIZE;
#else
         mv[2] = RMV_BLOCK_ERROR_DIRECT; // Just bang out all 128 pixels for now :(

         sx = x + mv_x;
         sy = y + mv_y;
         ref_prev = prev + sx + sy * c->plane_stride;

         calc_block_error(comp, ref, ref_prev, c->plane_stride);
         return RMV_BLOCK_SIZE * RMV_BLOCK_SIZE;
#endif
      }
   }
}

static void encode_inter_plane(RmvEncContext *c, const uint8_t *cur, const uint8_t *prev)
{
   int width  = c->avctx->width;
   int height = c->avctx->height;
   int bw = (width + (RMV_BLOCK_SIZE - 1)) / RMV_BLOCK_SIZE;
   int bh = (height + (RMV_BLOCK_SIZE - 1)) / RMV_BLOCK_SIZE;
   uint8_t *mv;

   //av_log(c->avctx, AV_LOG_INFO, "Encode inter plane ...\n");
   *c->comp_ptr++ = 'P';

   // Motion vectors are stored right after each other. Transformed error data comes after in chunks.
   // Motion vectors are organized as:
   // mv[0] = offset in X to use as reference (signed 8-bit).
   // mv[1] = offset in Y to use as reference (signed 8-bit).
   // mv[2] = block flags. Determines entropy type of block.
   mv = c->comp_ptr;

   c->comp_ptr += bw * bh * 3;

   for (int by = 0; by < bh; by++)
      for (int bx = 0; bx < bw; bx++, mv += 3)
         c->comp_ptr += encode_inter_block(c, mv, c->comp_ptr, bx, by, cur, prev);

   *c->comp_ptr++ = 'E';
}

static void encode_inter(RmvEncContext *c)
{
   //av_log(c->avctx, AV_LOG_INFO, "encode P-frame\n");
   *c->comp_ptr++ = 'R';
   *c->comp_ptr++ = 'M';
   *c->comp_ptr++ = 'V';
   *c->comp_ptr++ = RMV_FRAME_INTER;
   *c->comp_ptr++ = RMV_PIX_FMT_GBRP;
   *c->comp_ptr++ = RMV_BLOCK_SIZE;

   for (int i = 0; i < c->planes_used; i++)
      encode_inter_plane(c, c->planes[i], c->planes_prev[i]);
}

static int encode_frame(AVCodecContext *avctx, AVPacket *pkt,
      const AVFrame *pict, int *got_packet)
{
   int ret;
   bool keyframe;
   RmvEncContext *c = avctx->priv_data;

   AVFrame *p = &c->pic;
   *p         = *pict;

   keyframe = c->frame_cnt == 0;
   c->frame_cnt++;
   if (c->frame_cnt >= c->frame_per_key)
      c->frame_cnt = 0;

   p->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;

   interleave_frame_bgr24(c, c->planes, p->data[0], c->avctx->width, c->avctx->height, p->linesize[0]);

   c->pred_perfect = 0;
   c->pred_error   = 0;

   c->comp_ptr = c->comp_buf;
   if (keyframe)
      encode_intra(c);
   else
      encode_inter(c);

   interleave_frame_bgr24(c, c->planes_prev, p->data[0], c->avctx->width, c->avctx->height, p->linesize[0]);

   //av_log(c->avctx, AV_LOG_INFO, "Perfect: %d, Error: %d, Packet size: %d\n",
   //      c->pred_perfect, c->pred_error, (int)(c->comp_ptr - c->comp_buf));

   //av_log(c->avctx, AV_LOG_INFO, "Estimated size: %d\n",
   //      6 + 2 * 3 + (c->avctx->width * c->avctx->height) / (RMV_BLOCK_SIZE * RMV_BLOCK_SIZE) * 3 * 3 + RMV_BLOCK_SIZE * RMV_BLOCK_SIZE * c->pred_error);

   if (keyframe)
      pkt->flags |= AV_PKT_FLAG_KEY;

   ret = 0;
   if ((ret = ff_alloc_packet2(avctx, pkt, c->comp_ptr - c->comp_buf)) < 0)
      return ret;
   memcpy(pkt->data, c->comp_buf, c->comp_ptr - c->comp_buf);

   *got_packet = 1;
   return 0;
}


static av_cold int encode_init(AVCodecContext *avctx)
{
   RmvEncContext *c = avctx->priv_data;
   memset(c, 0, sizeof(*c));

   c->avctx = avctx;

   // First frame is always intra.
   c->frame_cnt     = 0;
   c->frame_per_key = avctx->keyint_min;

   c->me_range = FFMIN(avctx->me_range > 0 ? avctx->me_range : RMV_ME_RANGE_DEFAULT, RMV_ME_RANGE_MAX);

   switch (avctx->pix_fmt)
   {
      case PIX_FMT_BGR24:
         c->planes_used = 3;
         break;

      default:
         av_log(avctx, AV_LOG_ERROR, "Invalid pixel format used.\n");
         return AVERROR(EINVAL);
   }

   c->full_width   = FFALIGN(avctx->width, RMV_BLOCK_SIZE);
   c->plane_stride = FFALIGN(c->full_width, 16);
   c->full_height  = FFALIGN(avctx->height, RMV_BLOCK_SIZE);

   for (int i = 0; i < c->planes_used; i++)
   {
      if (!(c->planes[i] = av_malloc(c->plane_stride * c->full_height)))
      {
         av_log(avctx, AV_LOG_ERROR, "Can't allocate plane buffers.\n");
         return AVERROR(ENOMEM);
      }

      if (!(c->planes_prev[i] = av_malloc(c->plane_stride * c->full_height)))
      {
         av_log(avctx, AV_LOG_ERROR, "Can't allocate plane buffers.\n");
         return AVERROR(ENOMEM);
      }

      memset(c->planes[i], c->plane_stride, c->full_height);
      memset(c->planes_prev[i], c->plane_stride, c->full_height);
   }

   c->comp_size = 4 * c->plane_stride * c->full_height; // Be conservative.
   if (!(c->comp_buf = av_malloc(c->comp_size)))
   {
      av_log(avctx, AV_LOG_ERROR, "Can't allocate compression buffer.\n");
      return AVERROR(ENOMEM);
   }

   memset(c->comp_buf, 0, c->comp_size);

   avctx->coded_frame = &c->pic;

   return 0;
}

static av_cold int encode_end(AVCodecContext *avctx)
{
   RmvEncContext *c = avctx->priv_data;

   for (int i = 0; i < c->planes_used; i++)
   {
      if (c->planes[i])
         av_freep(&c->planes[i]);
      if (c->planes_prev[i])
         av_freep(&c->planes_prev[i]);
   }

   if (c->comp_buf)
      av_freep(&c->comp_buf);

   return 0;
}

AVCodec ff_rmv_encoder = {
   .name           = "rmv",
   .type           = AVMEDIA_TYPE_VIDEO,
   .id             = AV_CODEC_ID_RMV,
   .priv_data_size = sizeof(RmvEncContext),
   .init           = encode_init,
   .encode2        = encode_frame,
   .close          = encode_end,
   .pix_fmts       = (const enum PixelFormat[]){ PIX_FMT_BGR24, PIX_FMT_NONE },
   .long_name      = NULL_IF_CONFIG_SMALL("Retro Motion Video"),
};
