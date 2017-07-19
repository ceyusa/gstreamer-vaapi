/*
 *  gstvaapifeiutils_h264_fei.c - Fei related utilities for H264
 *
 *  Copyright (C) 2016-2018 Intel Corporation
 *    Author: Wang, Yi <yi.a.wang@intel.com>
 *    Author: Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */
#include "sysdeps.h"
#include <gst/codecparsers/gsth264parser.h>

#include "gstvaapifeiutils_h264.h"

/* FIXME: This is common fei modes for all codecs, move to a generic
 * header file */
GType
gst_vaapi_fei_mode_get_type (void)
{
  static volatile gsize g_type = 0;

  static const GFlagsValue encoding_mode_values[] = {
    {GST_VAAPI_FEI_MODE_ENC,
        "ENC Mode", "ENC"},
    {GST_VAAPI_FEI_MODE_PAK,
        "PAK Mode", "PAK"},
    {GST_VAAPI_FEI_MODE_ENC_PAK,
        "ENC_PAK Mode", "ENC_PAK"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&g_type)) {
    GType type =
        g_flags_register_static ("GstVaapiFeiMode", encoding_mode_values);
    g_once_init_leave (&g_type, type);
  }
  return g_type;
}

GType
gst_vaapi_fei_h264_search_path_get_type (void)
{
  static volatile gsize g_type = 0;

  static const GEnumValue search_path_values[] = {
    {GST_VAAPI_FEI_H264_FULL_SEARCH_PATH,
        "full search path", "full"},
    {GST_VAAPI_FEI_H264_DIAMOND_SEARCH_PATH,
        "diamond search path", "diamond"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&g_type)) {
    GType type = g_enum_register_static ("GstVaapiFeiH264SearchPath",
        search_path_values);
    g_once_init_leave (&g_type, type);
  }
  return g_type;
}

GType
gst_vaapi_fei_h264_search_window_get_type (void)
{
  static volatile gsize g_type = 0;

  static const GEnumValue search_window_values[] = {
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_NONE,
        "not use predefined search window", "none"},
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_TINY,
        "4 SUs 24x24 window diamond search", "tiny"},
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_SMALL,
        "9 SUs 28x28 window diamond search", "small"},
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_DIAMOND,
        "16 SUs 48x40 window diamond search", "diamond"},
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_LARGE_DIAMOND,
        "32 SUs 48x40 window diamond search", "large diamond"},
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_EXHAUSTIVE,
        "48 SUs 48x40 window full search", "exhaustive"},
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_HORI_DIAMOND,
        "16 SUs 64x32 window diamond search", "horizon diamond"},
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_HORI_LARGE_DIAMOND,
        "32 SUs 64x32 window diamond search", "horizon large diamond"},
    {GST_VAAPI_FEI_H264_SEARCH_WINDOW_HORI_EXHAUSTIVE,
        "48 SUs 64x32 window full search", "horizon exhaustive"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&g_type)) {
    GType type = g_enum_register_static ("GstVaapiFeiH264SearchWindow",
        search_window_values);
    g_once_init_leave (&g_type, type);
  }
  return g_type;
}

GType
gst_vaapi_fei_h264_sub_pel_mode_get_type (void)
{
  static volatile gsize g_type = 0;

  static const GEnumValue sub_pel_mode_values[] = {
    {GST_VAAPI_FEI_H264_INTEGER_ME,
        "integer mode searching", "integer"},
    {GST_VAAPI_FEI_H264_HALF_ME,
        "half-pel mode searching", "half"},
    {GST_VAAPI_FEI_H264_QUARTER_ME,
        "quarter-pel mode searching", "quarter"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&g_type)) {
    GType type = g_enum_register_static ("GstVaapiFeiH264SubPelMode",
        sub_pel_mode_values);
    g_once_init_leave (&g_type, type);
  }
  return g_type;
}

GType
gst_vaapi_fei_h264_sad_mode_get_type (void)
{
  static volatile gsize g_type = 0;

  static const GEnumValue sad_mode_values[] = {
    {GST_VAAPI_FEI_H264_SAD_NONE_TRANS,
        "none transform adjusted", "none"},
    {GST_VAAPI_FEI_H264_SAD_HAAR_TRANS,
        "Haar transform adjusted", "haar"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&g_type)) {
    GType type =
        g_enum_register_static ("GstVaapiFeiH264SadMode", sad_mode_values);
    g_once_init_leave (&g_type, type);
  }
  return g_type;
}

GType
gst_vaapi_fei_h264_intra_part_mask_get_type (void)
{
  static volatile gsize g_type = 0;

  static const GEnumValue intra_part_mask_values[] = {
    {GST_VAAPI_FEI_H264_DISABLE_INTRA_NONE,
        "enable all intra mode", "enable all"},
    {GST_VAAPI_FEI_H264_DISABLE_INTRA_16x16,
        "luma_intra_16x16 disabled", "intra16x16 disabled"},
    {GST_VAAPI_FEI_H264_DISABLE_INTRA_8x8,
        "luma_intra_8x8 disabled", "intra8x8 disabled"},
    {GST_VAAPI_FEI_H264_DISABLE_INTRA_16x16_8x8,
          "luma_intra_8x8 and luma_intra_16x16 disabled",
        "intra8x8/16x16 disabled"},
    {GST_VAAPI_FEI_H264_DISABLE_INTRA_4x4,
        "luma_intra_4x4 disabled", "intra4x4 disabled"},
    {GST_VAAPI_FEI_H264_DISABLE_INTRA_16x16_4x4,
          "luma_intra_4x4 and luma_intra_16x16 disabled",
        "intra4x4/16x16 disabled"},
    {GST_VAAPI_FEI_H264_DISABLE_INTRA_8x8_4x4,
        "luma_intra_4x4 and luma_intra_8x8 disabled", "intra4x4/8x8 disabled"},
    {GST_VAAPI_FEI_H264_DISABLE_INTRA_ALL,
        "intra prediction is disabled", "intra prediction is disabled"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&g_type)) {
    GType type = g_enum_register_static ("GstVaapiFeiH264IntraPartMask",
        intra_part_mask_values);
    g_once_init_leave (&g_type, type);
  }
  return g_type;
}
