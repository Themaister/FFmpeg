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

#ifndef RMV_H__
#define RMV_H__

#define RMV_ME_RANGE_DEFAULT 4
#define RMV_ME_RANGE_MAX 127

#define RMV_PIX_FMT_GBRP 1

#define RMV_FRAME_INTRA 1
#define RMV_FRAME_INTER 2

#define RMV_INTRA_DIRECT 0
#define RMV_INTRA_PRED_UP_RLE 1

#define RMV_BLOCK_PERFECT 1
#define RMV_BLOCK_ERROR_DIRECT 2
#define RMV_BLOCK_ZERO 4
#define RMV_BLOCK_DIRECT 8
#define RMV_BLOCK_ERROR_INDEX 16

#endif

