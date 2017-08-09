/*
 *  gstvaapiencoder_h264_fei.c - H.264 FEI encoder
 *
 *  Copyright (C) 2016-2017 Intel Corporation
 *    Author: Yi A Wang <yi.a.wang@intel.com>
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

/* GValueArray has deprecated without providing an alternative in glib >= 2.32
 * See https://bugzilla.gnome.org/show_bug.cgi?id=667228
 */
#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include "sysdeps.h"
#include <va/va.h>
#include <va/va_enc_h264.h>
#include <gst/base/gstbitwriter.h>
#include <gst/codecparsers/gsth264parser.h>
#include "gstvaapicompat.h"
#include "gstvaapiencoder_priv.h"
#include "gstvaapiutils_h264_priv.h"
#include "gstvaapicodedbufferproxy_priv.h"
#include "gstvaapisurfaceproxy_priv.h"
#include "gstvaapisurface.h"
#include "gstvaapifeiutils_h264.h"
#include "gstvaapiencoder_h264_fei.h"
#include "gstvaapifeienc_h264.h"
#include "gstvaapifeipak_h264.h"
#include "gstvaapiutils.h"
#include "gstvaapiutils_core.h"
#include "gstvaapifei_objects_priv.h"
#include <va/va_fei_h264.h>
#define DEBUG 1
#include "gstvaapidebug.h"

GPtrArray *gst_vaapi_encoder_h264_fei_get_default_properties (void);
static gboolean
gst_vaapi_encoder_h264_fei_ensure_secondary_context (GstVaapiEncoder *
    base_encoder);

/* Define the maximum number of views supported */
#define MAX_NUM_VIEWS 10

/* Define the maximum value for view-id */
#define MAX_VIEW_ID 1023

/* Define the maximum IDR period */
#define MAX_IDR_PERIOD 512

/* Default CPB length (in milliseconds) */
#define DEFAULT_CPB_LENGTH 1500

/* Scale factor for CPB size (HRD cpb_size_scale: min = 4) */
#define SX_CPB_SIZE 4

/* Scale factor for bitrate (HRD bit_rate_scale: min = 6) */
#define SX_BITRATE 6

/* Define default rate control mode ("constant-qp") */
#define DEFAULT_RATECONTROL GST_VAAPI_RATECONTROL_CQP

/* Supported set of VA rate controls, within this implementation */
#define SUPPORTED_RATECONTROLS                          \
  (GST_VAAPI_RATECONTROL_MASK (CQP)  |                  \
   GST_VAAPI_RATECONTROL_MASK (CBR)  |                  \
   GST_VAAPI_RATECONTROL_MASK (VBR)  |                  \
   GST_VAAPI_RATECONTROL_MASK (VBR_CONSTRAINED))

/* Supported set of tuning options, within this implementation */
#define SUPPORTED_TUNE_OPTIONS                          \
  (GST_VAAPI_ENCODER_TUNE_MASK (NONE) |                 \
   GST_VAAPI_ENCODER_TUNE_MASK (HIGH_COMPRESSION) |     \
   GST_VAAPI_ENCODER_TUNE_MASK (LOW_POWER))

/* Supported set of VA packed headers, within this implementation */
#define SUPPORTED_PACKED_HEADERS                \
  (VA_ENC_PACKED_HEADER_SEQUENCE |              \
   VA_ENC_PACKED_HEADER_PICTURE  |              \
   VA_ENC_PACKED_HEADER_SLICE    |              \
   VA_ENC_PACKED_HEADER_RAW_DATA |              \
   VA_ENC_PACKED_HEADER_MISC)

#define GST_H264_NAL_REF_IDC_NONE        0
#define GST_H264_NAL_REF_IDC_LOW         1
#define GST_H264_NAL_REF_IDC_MEDIUM      2
#define GST_H264_NAL_REF_IDC_HIGH        3

/* only for internal usage, values won't be equal to actual payload type */
typedef enum
{
  GST_VAAPI_H264_SEI_UNKNOWN = 0,
  GST_VAAPI_H264_SEI_BUF_PERIOD = (1 << 0),
  GST_VAAPI_H264_SEI_PIC_TIMING = (1 << 1)
} GstVaapiH264SeiPayloadType;

typedef struct
{
  GstVaapiSurfaceProxy *pic;
  guint poc;
  guint frame_num;
} GstVaapiEncoderH264FeiRef;

typedef enum
{
  GST_VAAPI_ENC_H264_REORD_NONE = 0,
  GST_VAAPI_ENC_H264_REORD_DUMP_FRAMES = 1,
  GST_VAAPI_ENC_H264_REORD_WAIT_FRAMES = 2
} GstVaapiEncH264ReorderState;

typedef struct _GstVaapiH264ViewRefPool
{
  GQueue ref_list;
  guint max_ref_frames;
  guint max_reflist0_count;
  guint max_reflist1_count;
} GstVaapiH264ViewRefPool;

typedef struct _GstVaapiH264ViewReorderPool
{
  GQueue reorder_frame_list;
  guint reorder_state;
  guint frame_index;
  guint frame_count;            /* monotonically increasing with in every idr period */
  guint cur_frame_num;
  guint cur_present_index;
} GstVaapiH264ViewReorderPool;

static inline gboolean
_poc_greater_than (guint poc1, guint poc2, guint max_poc)
{
  return (((poc1 - poc2) & (max_poc - 1)) < max_poc / 2);
}

/* Get slice_type value for H.264 specification */
static guint8
h264_get_slice_type (GstVaapiPictureType type)
{
  switch (type) {
    case GST_VAAPI_PICTURE_TYPE_I:
      return GST_H264_I_SLICE;
    case GST_VAAPI_PICTURE_TYPE_P:
      return GST_H264_P_SLICE;
    case GST_VAAPI_PICTURE_TYPE_B:
      return GST_H264_B_SLICE;
    default:
      break;
  }
  return -1;
}

/* Get log2_max_frame_num value for H.264 specification */
static guint
h264_get_log2_max_frame_num (guint num)
{
  guint ret = 0;

  while (num) {
    ++ret;
    num >>= 1;
  }
  if (ret <= 4)
    ret = 4;
  else if (ret > 10)
    ret = 10;
  /* must be greater than 4 */
  return ret;
}

/* Determines the cpbBrNalFactor based on the supplied profile */
static guint
h264_get_cpb_nal_factor (GstVaapiProfile profile)
{
  guint f;

  /* Table A-2 */
  switch (profile) {
    case GST_VAAPI_PROFILE_H264_HIGH:
      f = 1500;
      break;
    case GST_VAAPI_PROFILE_H264_HIGH10:
      f = 3600;
      break;
    case GST_VAAPI_PROFILE_H264_HIGH_422:
    case GST_VAAPI_PROFILE_H264_HIGH_444:
      f = 4800;
      break;
    case GST_VAAPI_PROFILE_H264_MULTIVIEW_HIGH:
    case GST_VAAPI_PROFILE_H264_STEREO_HIGH:
      f = 1500;                 /* H.10.2.1 (r) */
      break;
    default:
      f = 1200;
      break;
  }
  return f;
}

/* ------------------------------------------------------------------------- */
/* --- H.264 Bitstream Writer                                            --- */
/* ------------------------------------------------------------------------- */

#define WRITE_UINT32(bs, val, nbits) do {                       \
    if (!gst_bit_writer_put_bits_uint32 (bs, val, nbits)) {     \
      GST_WARNING ("failed to write uint32, nbits: %d", nbits); \
      goto bs_error;                                            \
    }                                                           \
  } while (0)

#define WRITE_UE(bs, val) do {                  \
    if (!bs_write_ue (bs, val)) {               \
      GST_WARNING ("failed to write ue(v)");    \
      goto bs_error;                            \
    }                                           \
  } while (0)

#define WRITE_SE(bs, val) do {                  \
    if (!bs_write_se (bs, val)) {               \
      GST_WARNING ("failed to write se(v)");    \
      goto bs_error;                            \
    }                                           \
  } while (0)

/* Write an unsigned integer Exp-Golomb-coded syntax element. i.e. ue(v) */
static gboolean
bs_write_ue (GstBitWriter * bs, guint32 value)
{
  guint32 size_in_bits = 0;
  guint32 tmp_value = ++value;

  while (tmp_value) {
    ++size_in_bits;
    tmp_value >>= 1;
  }
  if (size_in_bits > 1
      && !gst_bit_writer_put_bits_uint32 (bs, 0, size_in_bits - 1))
    return FALSE;
  if (!gst_bit_writer_put_bits_uint32 (bs, value, size_in_bits))
    return FALSE;
  return TRUE;
}

/* Write a signed integer Exp-Golomb-coded syntax element. i.e. se(v) */
static gboolean
bs_write_se (GstBitWriter * bs, gint32 value)
{
  guint32 new_val;

  if (value <= 0)
    new_val = -(value << 1);
  else
    new_val = (value << 1) - 1;

  if (!bs_write_ue (bs, new_val))
    return FALSE;
  return TRUE;
}

/* Write the NAL unit header */
static gboolean
bs_write_nal_header (GstBitWriter * bs, guint32 nal_ref_idc,
    guint32 nal_unit_type)
{
  WRITE_UINT32 (bs, 0, 1);
  WRITE_UINT32 (bs, nal_ref_idc, 2);
  WRITE_UINT32 (bs, nal_unit_type, 5);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write NAL unit header");
    return FALSE;
  }
}

/* Write the MVC NAL unit header extension */
static gboolean
bs_write_nal_header_mvc_extension (GstBitWriter * bs,
    GstVaapiEncPicture * picture, guint32 view_id)
{
  guint32 svc_extension_flag = 0;
  guint32 non_idr_flag = 1;
  guint32 priority_id = 0;
  guint32 temporal_id = 0;
  guint32 anchor_pic_flag = 0;
  guint32 inter_view_flag = 0;

  if (GST_VAAPI_ENC_PICTURE_IS_IDR (picture))
    non_idr_flag = 0;

  if (picture->type == GST_VAAPI_PICTURE_TYPE_I)
    anchor_pic_flag = 1;
  /* svc_extension_flag == 0 for mvc stream */
  WRITE_UINT32 (bs, svc_extension_flag, 1);

  WRITE_UINT32 (bs, non_idr_flag, 1);
  WRITE_UINT32 (bs, priority_id, 6);
  WRITE_UINT32 (bs, view_id, 10);
  WRITE_UINT32 (bs, temporal_id, 3);
  WRITE_UINT32 (bs, anchor_pic_flag, 1);
  WRITE_UINT32 (bs, inter_view_flag, 1);
  WRITE_UINT32 (bs, 1, 1);

  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write NAL unit header");
    return FALSE;
  }
}

/* Write the NAL unit trailing bits */
static gboolean
bs_write_trailing_bits (GstBitWriter * bs)
{
  if (!gst_bit_writer_put_bits_uint32 (bs, 1, 1))
    goto bs_error;
  gst_bit_writer_align_bytes_unchecked (bs, 0);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write NAL unit trailing bits");
    return FALSE;
  }
}

/* Write an SPS NAL unit */
static gboolean
bs_write_sps_data (GstBitWriter * bs,
    const VAEncSequenceParameterBufferH264 * seq_param, GstVaapiProfile profile,
    const VAEncMiscParameterHRD * hrd_params)
{
  guint8 profile_idc;
  guint32 constraint_set0_flag, constraint_set1_flag;
  guint32 constraint_set2_flag, constraint_set3_flag;
  guint32 gaps_in_frame_num_value_allowed_flag = 0;     // ??
  gboolean nal_hrd_parameters_present_flag;

  guint32 b_qpprime_y_zero_transform_bypass = 0;
  guint32 residual_color_transform_flag = 0;
  guint32 pic_height_in_map_units =
      (seq_param->seq_fields.bits.frame_mbs_only_flag ?
      seq_param->picture_height_in_mbs : seq_param->picture_height_in_mbs / 2);
  guint32 mb_adaptive_frame_field =
      !seq_param->seq_fields.bits.frame_mbs_only_flag;
  guint32 i = 0;

  profile_idc = gst_vaapi_utils_h264_get_profile_idc (profile);
  constraint_set0_flag =        /* A.2.1 (baseline profile constraints) */
      profile == GST_VAAPI_PROFILE_H264_BASELINE ||
      profile == GST_VAAPI_PROFILE_H264_CONSTRAINED_BASELINE;
  constraint_set1_flag =        /* A.2.2 (main profile constraints) */
      profile == GST_VAAPI_PROFILE_H264_MAIN ||
      profile == GST_VAAPI_PROFILE_H264_CONSTRAINED_BASELINE;
  constraint_set2_flag = 0;
  constraint_set3_flag = 0;

  /* profile_idc */
  WRITE_UINT32 (bs, profile_idc, 8);
  /* constraint_set0_flag */
  WRITE_UINT32 (bs, constraint_set0_flag, 1);
  /* constraint_set1_flag */
  WRITE_UINT32 (bs, constraint_set1_flag, 1);
  /* constraint_set2_flag */
  WRITE_UINT32 (bs, constraint_set2_flag, 1);
  /* constraint_set3_flag */
  WRITE_UINT32 (bs, constraint_set3_flag, 1);
  /* reserved_zero_4bits */
  WRITE_UINT32 (bs, 0, 4);
  /* level_idc */
  WRITE_UINT32 (bs, seq_param->level_idc, 8);
  /* seq_parameter_set_id */
  WRITE_UE (bs, seq_param->seq_parameter_set_id);

  if (profile == GST_VAAPI_PROFILE_H264_HIGH ||
      profile == GST_VAAPI_PROFILE_H264_MULTIVIEW_HIGH ||
      profile == GST_VAAPI_PROFILE_H264_STEREO_HIGH) {
    /* for high profile */
    /* chroma_format_idc  = 1, 4:2:0 */
    WRITE_UE (bs, seq_param->seq_fields.bits.chroma_format_idc);
    if (3 == seq_param->seq_fields.bits.chroma_format_idc) {
      WRITE_UINT32 (bs, residual_color_transform_flag, 1);
    }
    /* bit_depth_luma_minus8 */
    WRITE_UE (bs, seq_param->bit_depth_luma_minus8);
    /* bit_depth_chroma_minus8 */
    WRITE_UE (bs, seq_param->bit_depth_chroma_minus8);
    /* b_qpprime_y_zero_transform_bypass */
    WRITE_UINT32 (bs, b_qpprime_y_zero_transform_bypass, 1);

    /* seq_scaling_matrix_present_flag  */
    g_assert (seq_param->seq_fields.bits.seq_scaling_matrix_present_flag == 0);
    WRITE_UINT32 (bs,
        seq_param->seq_fields.bits.seq_scaling_matrix_present_flag, 1);

#if 0
    if (seq_param->seq_fields.bits.seq_scaling_matrix_present_flag) {
      for (i = 0;
          i < (seq_param->seq_fields.bits.chroma_format_idc != 3 ? 8 : 12);
          i++) {
        gst_bit_writer_put_bits_uint8 (bs,
            seq_param->seq_fields.bits.seq_scaling_list_present_flag, 1);
        if (seq_param->seq_fields.bits.seq_scaling_list_present_flag) {
          g_assert (0);
          /* FIXME, need write scaling list if seq_scaling_matrix_present_flag ==1 */
        }
      }
    }
#endif
  }

  /* log2_max_frame_num_minus4 */
  WRITE_UE (bs, seq_param->seq_fields.bits.log2_max_frame_num_minus4);
  /* pic_order_cnt_type */
  WRITE_UE (bs, seq_param->seq_fields.bits.pic_order_cnt_type);

  if (seq_param->seq_fields.bits.pic_order_cnt_type == 0) {
    /* log2_max_pic_order_cnt_lsb_minus4 */
    WRITE_UE (bs, seq_param->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
  } else if (seq_param->seq_fields.bits.pic_order_cnt_type == 1) {
    g_assert (0 && "only POC type 0 is supported");
    WRITE_UINT32 (bs,
        seq_param->seq_fields.bits.delta_pic_order_always_zero_flag, 1);
    WRITE_SE (bs, seq_param->offset_for_non_ref_pic);
    WRITE_SE (bs, seq_param->offset_for_top_to_bottom_field);
    WRITE_UE (bs, seq_param->num_ref_frames_in_pic_order_cnt_cycle);
    for (i = 0; i < seq_param->num_ref_frames_in_pic_order_cnt_cycle; i++) {
      WRITE_SE (bs, seq_param->offset_for_ref_frame[i]);
    }
  }

  /* num_ref_frames */
  WRITE_UE (bs, seq_param->max_num_ref_frames);
  /* gaps_in_frame_num_value_allowed_flag */
  WRITE_UINT32 (bs, gaps_in_frame_num_value_allowed_flag, 1);

  /* pic_width_in_mbs_minus1 */
  WRITE_UE (bs, seq_param->picture_width_in_mbs - 1);
  /* pic_height_in_map_units_minus1 */
  WRITE_UE (bs, pic_height_in_map_units - 1);
  /* frame_mbs_only_flag */
  WRITE_UINT32 (bs, seq_param->seq_fields.bits.frame_mbs_only_flag, 1);

  if (!seq_param->seq_fields.bits.frame_mbs_only_flag) {        //ONLY mbs
    g_assert (0 && "only progressive frames encoding is supported");
    WRITE_UINT32 (bs, mb_adaptive_frame_field, 1);
  }

  /* direct_8x8_inference_flag */
  WRITE_UINT32 (bs, 0, 1);
  /* frame_cropping_flag */
  WRITE_UINT32 (bs, seq_param->frame_cropping_flag, 1);

  if (seq_param->frame_cropping_flag) {
    /* frame_crop_left_offset */
    WRITE_UE (bs, seq_param->frame_crop_left_offset);
    /* frame_crop_right_offset */
    WRITE_UE (bs, seq_param->frame_crop_right_offset);
    /* frame_crop_top_offset */
    WRITE_UE (bs, seq_param->frame_crop_top_offset);
    /* frame_crop_bottom_offset */
    WRITE_UE (bs, seq_param->frame_crop_bottom_offset);
  }

  /* vui_parameters_present_flag */
  WRITE_UINT32 (bs, seq_param->vui_parameters_present_flag, 1);
  if (seq_param->vui_parameters_present_flag) {
    /* aspect_ratio_info_present_flag */
    WRITE_UINT32 (bs,
        seq_param->vui_fields.bits.aspect_ratio_info_present_flag, 1);
    if (seq_param->vui_fields.bits.aspect_ratio_info_present_flag) {
      WRITE_UINT32 (bs, seq_param->aspect_ratio_idc, 8);
      if (seq_param->aspect_ratio_idc == 0xFF) {
        WRITE_UINT32 (bs, seq_param->sar_width, 16);
        WRITE_UINT32 (bs, seq_param->sar_height, 16);
      }
    }

    /* overscan_info_present_flag */
    WRITE_UINT32 (bs, 0, 1);
    /* video_signal_type_present_flag */
    WRITE_UINT32 (bs, 0, 1);
    /* chroma_loc_info_present_flag */
    WRITE_UINT32 (bs, 0, 1);

    /* timing_info_present_flag */
    WRITE_UINT32 (bs, seq_param->vui_fields.bits.timing_info_present_flag, 1);
    if (seq_param->vui_fields.bits.timing_info_present_flag) {
      WRITE_UINT32 (bs, seq_param->num_units_in_tick, 32);
      WRITE_UINT32 (bs, seq_param->time_scale, 32);
      WRITE_UINT32 (bs, 1, 1);  /* fixed_frame_rate_flag */
    }

    /* nal_hrd_parameters_present_flag */
    nal_hrd_parameters_present_flag = seq_param->bits_per_second > 0;
    WRITE_UINT32 (bs, nal_hrd_parameters_present_flag, 1);
    if (nal_hrd_parameters_present_flag) {
      /* hrd_parameters */
      /* cpb_cnt_minus1 */
      WRITE_UE (bs, 0);
      WRITE_UINT32 (bs, SX_BITRATE - 6, 4);     /* bit_rate_scale */
      WRITE_UINT32 (bs, SX_CPB_SIZE - 4, 4);    /* cpb_size_scale */

      for (i = 0; i < 1; ++i) {
        /* bit_rate_value_minus1[0] */
        WRITE_UE (bs, (seq_param->bits_per_second >> SX_BITRATE) - 1);
        /* cpb_size_value_minus1[0] */
        WRITE_UE (bs, (hrd_params->buffer_size >> SX_CPB_SIZE) - 1);
        /* cbr_flag[0] */
        WRITE_UINT32 (bs, 1, 1);
      }
      /* initial_cpb_removal_delay_length_minus1 */
      WRITE_UINT32 (bs, 23, 5);
      /* cpb_removal_delay_length_minus1 */
      WRITE_UINT32 (bs, 23, 5);
      /* dpb_output_delay_length_minus1 */
      WRITE_UINT32 (bs, 23, 5);
      /* time_offset_length  */
      WRITE_UINT32 (bs, 23, 5);
    }

    /* vcl_hrd_parameters_present_flag */
    WRITE_UINT32 (bs, 0, 1);

    if (nal_hrd_parameters_present_flag
        || 0 /*vcl_hrd_parameters_present_flag */ ) {
      /* low_delay_hrd_flag */
      WRITE_UINT32 (bs, 0, 1);
    }
    /* pic_struct_present_flag */
    WRITE_UINT32 (bs, 1, 1);
    /* bs_restriction_flag */
    WRITE_UINT32 (bs, 0, 1);
  }
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write SPS NAL unit");
    return FALSE;
  }
}

static gboolean
bs_write_sps (GstBitWriter * bs,
    const VAEncSequenceParameterBufferH264 * seq_param, GstVaapiProfile profile,
    const VAEncMiscParameterHRD * hrd_params)
{
  if (!bs_write_sps_data (bs, seq_param, profile, hrd_params))
    return FALSE;

  /* rbsp_trailing_bits */
  bs_write_trailing_bits (bs);

  return FALSE;
}

static gboolean
bs_write_subset_sps (GstBitWriter * bs,
    const VAEncSequenceParameterBufferH264 * seq_param, GstVaapiProfile profile,
    guint num_views, guint16 * view_ids,
    const VAEncMiscParameterHRD * hrd_params)
{
  guint32 i, j, k;

  if (!bs_write_sps_data (bs, seq_param, profile, hrd_params))
    return FALSE;

  if (profile == GST_VAAPI_PROFILE_H264_STEREO_HIGH ||
      profile == GST_VAAPI_PROFILE_H264_MULTIVIEW_HIGH) {
    guint32 num_views_minus1, num_level_values_signalled_minus1;

    num_views_minus1 = num_views - 1;
    g_assert (num_views_minus1 < 1024);

    /* bit equal to one */
    WRITE_UINT32 (bs, 1, 1);

    WRITE_UE (bs, num_views_minus1);

    for (i = 0; i <= num_views_minus1; i++)
      WRITE_UE (bs, view_ids[i]);

    for (i = 1; i <= num_views_minus1; i++) {
      guint32 num_anchor_refs_l0 = 0;
      guint32 num_anchor_refs_l1 = 0;

      WRITE_UE (bs, num_anchor_refs_l0);
      for (j = 0; j < num_anchor_refs_l0; j++)
        WRITE_UE (bs, 0);

      WRITE_UE (bs, num_anchor_refs_l1);
      for (j = 0; j < num_anchor_refs_l1; j++)
        WRITE_UE (bs, 0);
    }

    for (i = 1; i <= num_views_minus1; i++) {
      guint32 num_non_anchor_refs_l0 = 0;
      guint32 num_non_anchor_refs_l1 = 0;

      WRITE_UE (bs, num_non_anchor_refs_l0);
      for (j = 0; j < num_non_anchor_refs_l0; j++)
        WRITE_UE (bs, 0);

      WRITE_UE (bs, num_non_anchor_refs_l1);
      for (j = 0; j < num_non_anchor_refs_l1; j++)
        WRITE_UE (bs, 0);
    }

    /* num level values signalled minus1 */
    num_level_values_signalled_minus1 = 0;
    g_assert (num_level_values_signalled_minus1 < 64);
    WRITE_UE (bs, num_level_values_signalled_minus1);

    for (i = 0; i <= num_level_values_signalled_minus1; i++) {
      guint16 num_applicable_ops_minus1 = 0;
      g_assert (num_applicable_ops_minus1 < 1024);

      WRITE_UINT32 (bs, seq_param->level_idc, 8);
      WRITE_UE (bs, num_applicable_ops_minus1);

      for (j = 0; j <= num_applicable_ops_minus1; j++) {
        guint8 temporal_id = 0;
        guint16 num_target_views_minus1 = 1;

        WRITE_UINT32 (bs, temporal_id, 3);
        WRITE_UE (bs, num_target_views_minus1);

        for (k = 0; k <= num_target_views_minus1; k++)
          WRITE_UE (bs, k);

        WRITE_UE (bs, num_views_minus1);
      }
    }

    /* mvc_vui_parameters_present_flag */
    WRITE_UINT32 (bs, 0, 1);
  }

  /* additional_extension2_flag */
  WRITE_UINT32 (bs, 0, 1);

  /* rbsp_trailing_bits */
  bs_write_trailing_bits (bs);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write subset SPS NAL unit");
    return FALSE;
  }
  return FALSE;
}

/* Write a PPS NAL unit */
static gboolean
bs_write_pps (GstBitWriter * bs,
    const VAEncPictureParameterBufferH264 * pic_param, GstVaapiProfile profile)
{
  guint32 num_slice_groups_minus1 = 0;
  guint32 pic_init_qs_minus26 = 0;
  guint32 redundant_pic_cnt_present_flag = 0;

  /* pic_parameter_set_id */
  WRITE_UE (bs, pic_param->pic_parameter_set_id);
  /* seq_parameter_set_id */
  WRITE_UE (bs, pic_param->seq_parameter_set_id);
  /* entropy_coding_mode_flag */
  WRITE_UINT32 (bs, pic_param->pic_fields.bits.entropy_coding_mode_flag, 1);
  /* pic_order_present_flag */
  WRITE_UINT32 (bs, pic_param->pic_fields.bits.pic_order_present_flag, 1);
  /* slice_groups-1 */
  WRITE_UE (bs, num_slice_groups_minus1);

  if (num_slice_groups_minus1 > 0) {
     /*FIXME*/ g_assert (0 && "unsupported arbitrary slice ordering (ASO)");
  }
  WRITE_UE (bs, pic_param->num_ref_idx_l0_active_minus1);
  WRITE_UE (bs, pic_param->num_ref_idx_l1_active_minus1);
  WRITE_UINT32 (bs, pic_param->pic_fields.bits.weighted_pred_flag, 1);
  WRITE_UINT32 (bs, pic_param->pic_fields.bits.weighted_bipred_idc, 2);
  /* pic_init_qp_minus26 */
  WRITE_SE (bs, pic_param->pic_init_qp - 26);
  /* pic_init_qs_minus26 */
  WRITE_SE (bs, pic_init_qs_minus26);
  /* chroma_qp_index_offset */
  WRITE_SE (bs, pic_param->chroma_qp_index_offset);

  WRITE_UINT32 (bs,
      pic_param->pic_fields.bits.deblocking_filter_control_present_flag, 1);
  WRITE_UINT32 (bs, pic_param->pic_fields.bits.constrained_intra_pred_flag, 1);
  WRITE_UINT32 (bs, redundant_pic_cnt_present_flag, 1);

  /* more_rbsp_data */
  if (profile == GST_VAAPI_PROFILE_H264_HIGH
      || profile == GST_VAAPI_PROFILE_H264_MULTIVIEW_HIGH
      || profile == GST_VAAPI_PROFILE_H264_STEREO_HIGH) {
    WRITE_UINT32 (bs, pic_param->pic_fields.bits.transform_8x8_mode_flag, 1);
    WRITE_UINT32 (bs,
        pic_param->pic_fields.bits.pic_scaling_matrix_present_flag, 1);
    if (pic_param->pic_fields.bits.pic_scaling_matrix_present_flag) {
      g_assert (0 && "unsupported scaling lists");
      /* FIXME */
      /*
         for (i = 0; i <
         (6+(-( (chroma_format_idc ! = 3) ? 2 : 6) * -pic_param->pic_fields.bits.transform_8x8_mode_flag));
         i++) {
         gst_bit_writer_put_bits_uint8(bs, pic_param->pic_fields.bits.pic_scaling_list_present_flag, 1);
         }
       */
    }
    WRITE_SE (bs, pic_param->second_chroma_qp_index_offset);
  }

  /* rbsp_trailing_bits */
  bs_write_trailing_bits (bs);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write PPS NAL unit");
    return FALSE;
  }
}

/* ------------------------------------------------------------------------- */
/* --- H.264 Encoder                                                     --- */
/* ------------------------------------------------------------------------- */

#define GST_VAAPI_ENCODER_H264_FEI_CAST(encoder) \
    ((GstVaapiEncoderH264Fei *)(encoder))

struct _GstVaapiEncoderH264Fei
{
  GstVaapiEncoder parent_instance;
  GstVaapiFeiEncH264 *feienc;
  GstVaapiFEIPakH264 *feipak;

  GstVaapiProfile profile;
  GstVaapiLevelH264 level;
  GstVaapiEntrypoint entrypoint;
  VAConfigID va_config;
  guint8 profile_idc;
  VABufferID coded_buf;
  guint8 max_profile_idc;
  guint8 hw_max_profile_idc;
  guint8 level_idc;
  guint32 idr_period;
  guint32 init_qp;
  guint32 min_qp;
  guint32 num_slices;
  guint32 num_bframes;
  guint32 mb_width;
  guint32 mb_height;
  gboolean use_cabac;
  gboolean use_dct8x8;
  GstClockTime cts_offset;
  gboolean config_changed;

  /* frame, poc */
  guint32 max_frame_num;
  guint32 log2_max_frame_num;
  guint32 max_pic_order_cnt;
  guint32 log2_max_pic_order_cnt;
  guint32 idr_num;
  guint8 pic_order_cnt_type;
  guint8 delta_pic_order_always_zero_flag;

  GstBuffer *sps_data;
  GstBuffer *subset_sps_data;
  GstBuffer *pps_data;

  guint bitrate_bits;           // bitrate (bits)
  guint cpb_length;             // length of CPB buffer (ms)
  guint cpb_length_bits;        // length of CPB buffer (bits)
  guint num_ref_frames;

  /* MVC */
  gboolean is_mvc;
  guint32 view_idx;             /* View Order Index (VOIdx) */
  guint32 num_views;
  guint16 view_ids[MAX_NUM_VIEWS];
  GstVaapiH264ViewRefPool ref_pools[MAX_NUM_VIEWS];
  GstVaapiH264ViewReorderPool reorder_pools[MAX_NUM_VIEWS];
  gpointer ref_pool_ptr;
  /*Fei frame level control */
  gboolean is_fei_disabled;
  gboolean is_stats_out_enabled;
  guint search_window;
  guint len_sp;
  guint search_path;
  guint ref_width;
  guint ref_height;
  guint submb_part_mask;
  guint subpel_mode;
  guint intra_part_mask;
  guint intra_sad;
  guint inter_sad;
  guint num_mv_predictors_l0;
  guint num_mv_predictors_l1;
  guint adaptive_search;
  guint multi_predL0;
  guint multi_predL1;
  guint fei_mode;

};

/* Write a SEI buffering period payload */
static gboolean
bs_write_sei_buf_period (GstBitWriter * bs,
    GstVaapiEncoderH264Fei * encoder, GstVaapiEncPicture * picture)
{
  guint initial_cpb_removal_delay = 0;
  guint initial_cpb_removal_delay_offset = 0;
  guint8 initial_cpb_removal_delay_length = 24;

  /* sequence_parameter_set_id */
  WRITE_UE (bs, encoder->view_idx);
  /* NalHrdBpPresentFlag == TRUE */
  /* cpb_cnt_minus1 == 0 */

  /* decoding should start when the CPB fullness reaches half of cpb size
   * initial_cpb_remvoal_delay = (((cpb_length / 2) * 90000) / 1000) */
  initial_cpb_removal_delay = encoder->cpb_length * 45;

  /* initial_cpb_remvoal_dealy */
  WRITE_UINT32 (bs, initial_cpb_removal_delay,
      initial_cpb_removal_delay_length);

  /* initial_cpb_removal_delay_offset */
  WRITE_UINT32 (bs, initial_cpb_removal_delay_offset,
      initial_cpb_removal_delay_length);

  /* VclHrdBpPresentFlag == FALSE */
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write Buffering Period SEI message");
    return FALSE;
  }
}

/* Write a SEI picture timing payload */
static gboolean
bs_write_sei_pic_timing (GstBitWriter * bs,
    GstVaapiEncoderH264Fei * encoder, GstVaapiEncPicture * picture)
{
  GstVaapiH264ViewReorderPool *reorder_pool = NULL;
  guint cpb_removal_delay;
  guint dpb_output_delay;
  guint8 cpb_removal_delay_length = 24;
  guint8 dpb_output_delay_length = 24;
  guint pic_struct = 0;
  guint clock_timestamp_flag = 0;

  reorder_pool = &encoder->reorder_pools[encoder->view_idx];
  if (GST_VAAPI_ENC_PICTURE_IS_IDR (picture))
    reorder_pool->frame_count = 0;
  else
    reorder_pool->frame_count++;

  /* clock-tick = no_units_in_tick/time_scale (C-1)
   * time_scale = FPS_N * 2  (E.2.1)
   * num_units_in_tick = FPS_D (E.2.1)
   * frame_duration = clock-tick * 2
   * so removal time for one frame is 2 clock-ticks.
   * but adding a tolerance of one frame duration,
   * which is 2 more clock-ticks */
  cpb_removal_delay = (reorder_pool->frame_count * 2 + 2);

  if (picture->type == GST_VAAPI_PICTURE_TYPE_B)
    dpb_output_delay = 0;
  else
    dpb_output_delay = picture->poc - reorder_pool->frame_count * 2;

  /* CpbDpbDelaysPresentFlag == 1 */
  WRITE_UINT32 (bs, cpb_removal_delay, cpb_removal_delay_length);
  WRITE_UINT32 (bs, dpb_output_delay, dpb_output_delay_length);

  /* pic_struct_present_flag == 1 */
  /* pic_struct */
  WRITE_UINT32 (bs, pic_struct, 4);
  /* clock_timestamp_flag */
  WRITE_UINT32 (bs, clock_timestamp_flag, 1);

  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write Picture Timing SEI message");
    return FALSE;
  }
}

/* Write a Slice NAL unit */
static gboolean
bs_write_slice (GstBitWriter * bs,
    const VAEncSliceParameterBufferH264 * slice_param,
    GstVaapiEncoderH264Fei * encoder, GstVaapiEncPicture * picture)
{
  const VAEncPictureParameterBufferH264 *const pic_param = picture->param;
  guint32 field_pic_flag = 0;
  guint32 ref_pic_list_modification_flag_l0 = 0;
  guint32 ref_pic_list_modification_flag_l1 = 0;
  guint32 no_output_of_prior_pics_flag = 0;
  guint32 long_term_reference_flag = 0;
  guint32 adaptive_ref_pic_marking_mode_flag = 0;

  /* first_mb_in_slice */
  WRITE_UE (bs, slice_param->macroblock_address);
  /* slice_type */
  WRITE_UE (bs, slice_param->slice_type);
  /* pic_parameter_set_id */
  WRITE_UE (bs, slice_param->pic_parameter_set_id);
  /* frame_num */
  WRITE_UINT32 (bs, picture->frame_num, encoder->log2_max_frame_num);

  /* XXX: only frames (i.e. non-interlaced) are supported for now */
  /* frame_mbs_only_flag == 0 */

  /* idr_pic_id */
  if (GST_VAAPI_ENC_PICTURE_IS_IDR (picture))
    WRITE_UE (bs, slice_param->idr_pic_id);

  /* XXX: only POC type 0 is supported */
  if (!encoder->pic_order_cnt_type) {
    WRITE_UINT32 (bs, slice_param->pic_order_cnt_lsb,
        encoder->log2_max_pic_order_cnt);
    /* bottom_field_pic_order_in_frame_present_flag is FALSE */
    if (pic_param->pic_fields.bits.pic_order_present_flag && !field_pic_flag)
      WRITE_SE (bs, slice_param->delta_pic_order_cnt_bottom);
  } else if (encoder->pic_order_cnt_type == 1 &&
      !encoder->delta_pic_order_always_zero_flag) {
    WRITE_SE (bs, slice_param->delta_pic_order_cnt[0]);
    if (pic_param->pic_fields.bits.pic_order_present_flag && !field_pic_flag)
      WRITE_SE (bs, slice_param->delta_pic_order_cnt[1]);
  }
  /* redundant_pic_cnt_present_flag is FALSE, no redundant coded pictures */

  /* only works for B-frames */
  if (slice_param->slice_type == GST_H264_B_SLICE)
    WRITE_UINT32 (bs, slice_param->direct_spatial_mv_pred_flag, 1);

  /* not supporting SP slices */
  if (slice_param->slice_type == 0 || slice_param->slice_type == 1) {
    WRITE_UINT32 (bs, slice_param->num_ref_idx_active_override_flag, 1);
    if (slice_param->num_ref_idx_active_override_flag) {
      WRITE_UE (bs, slice_param->num_ref_idx_l0_active_minus1);
      if (slice_param->slice_type == 1)
        WRITE_UE (bs, slice_param->num_ref_idx_l1_active_minus1);
    }
  }
  /* XXX: not supporting custom reference picture list modifications */
  if ((slice_param->slice_type != 2) && (slice_param->slice_type != 4))
    WRITE_UINT32 (bs, ref_pic_list_modification_flag_l0, 1);
  if (slice_param->slice_type == 1)
    WRITE_UINT32 (bs, ref_pic_list_modification_flag_l1, 1);

  /* we have: weighted_pred_flag == FALSE and */
  /*        : weighted_bipred_idc == FALSE */
  if ((pic_param->pic_fields.bits.weighted_pred_flag &&
          (slice_param->slice_type == 0)) ||
      ((pic_param->pic_fields.bits.weighted_bipred_idc == 1) &&
          (slice_param->slice_type == 1))) {
    /* XXXX: add pred_weight_table() */
  }

  /* dec_ref_pic_marking() */
  if (slice_param->slice_type == 0 || slice_param->slice_type == 2) {
    if (GST_VAAPI_ENC_PICTURE_IS_IDR (picture)) {
      /* no_output_of_prior_pics_flag = 0 */
      WRITE_UINT32 (bs, no_output_of_prior_pics_flag, 1);
      /* long_term_reference_flag = 0 */
      WRITE_UINT32 (bs, long_term_reference_flag, 1);
    } else {
      /* only sliding_window reference picture marking mode is supported */
      /* adpative_ref_pic_marking_mode_flag = 0 */
      WRITE_UINT32 (bs, adaptive_ref_pic_marking_mode_flag, 1);
    }
  }

  /* cabac_init_idc */
  if (pic_param->pic_fields.bits.entropy_coding_mode_flag &&
      slice_param->slice_type != 2)
    WRITE_UE (bs, slice_param->cabac_init_idc);
  /*slice_qp_delta */
  WRITE_SE (bs, slice_param->slice_qp_delta);

  /* XXX: only supporting I, P and B type slices */
  /* no sp_for_switch_flag and no slice_qs_delta */

  if (pic_param->pic_fields.bits.deblocking_filter_control_present_flag) {
    /* disable_deblocking_filter_idc */
    WRITE_UE (bs, slice_param->disable_deblocking_filter_idc);
    if (slice_param->disable_deblocking_filter_idc != 1) {
      WRITE_SE (bs, slice_param->slice_alpha_c0_offset_div2);
      WRITE_SE (bs, slice_param->slice_beta_offset_div2);
    }
  }

  /* XXX: unsupported arbitrary slice ordering (ASO) */
  /* num_slic_groups_minus1 should be zero */
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write Slice NAL unit");
    return FALSE;
  }
}

static inline void
_check_sps_pps_status (GstVaapiEncoderH264Fei * encoder,
    const guint8 * nal, guint32 size)
{
  guint8 nal_type;
  G_GNUC_UNUSED gsize ret;      /* FIXME */
  gboolean has_subset_sps;

  g_assert (size);

  has_subset_sps = !encoder->is_mvc || (encoder->subset_sps_data != NULL);
  if (encoder->sps_data && encoder->pps_data && has_subset_sps)
    return;

  nal_type = nal[0] & 0x1F;
  switch (nal_type) {
    case GST_H264_NAL_SPS:
      encoder->sps_data = gst_buffer_new_allocate (NULL, size, NULL);
      ret = gst_buffer_fill (encoder->sps_data, 0, nal, size);
      g_assert (ret == size);
      break;
    case GST_H264_NAL_SUBSET_SPS:
      encoder->subset_sps_data = gst_buffer_new_allocate (NULL, size, NULL);
      ret = gst_buffer_fill (encoder->subset_sps_data, 0, nal, size);
      g_assert (ret == size);
      break;
    case GST_H264_NAL_PPS:
      encoder->pps_data = gst_buffer_new_allocate (NULL, size, NULL);
      ret = gst_buffer_fill (encoder->pps_data, 0, nal, size);
      g_assert (ret == size);
      break;
    default:
      break;
  }
}

/* Determines the largest supported profile by the underlying hardware */
static gboolean
ensure_hw_profile_limits (GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiDisplay *const display = GST_VAAPI_ENCODER_DISPLAY (encoder);
  GArray *profiles;
  guint i, profile_idc, max_profile_idc;

  if (encoder->hw_max_profile_idc)
    return TRUE;

  profiles = gst_vaapi_display_get_encode_profiles (display);
  if (!profiles)
    return FALSE;

  max_profile_idc = 0;
  for (i = 0; i < profiles->len; i++) {
    const GstVaapiProfile profile =
        g_array_index (profiles, GstVaapiProfile, i);
    profile_idc = gst_vaapi_utils_h264_get_profile_idc (profile);
    if (!profile_idc)
      continue;
    if (max_profile_idc < profile_idc)
      max_profile_idc = profile_idc;
  }
  g_array_unref (profiles);

  encoder->hw_max_profile_idc = max_profile_idc;
  return TRUE;
}

/* Derives the profile supported by the underlying hardware */
static gboolean
ensure_hw_profile (GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiDisplay *const display = GST_VAAPI_ENCODER_DISPLAY (encoder);
  GstVaapiEntrypoint entrypoint = encoder->entrypoint;
  GstVaapiProfile profile, profiles[4];
  guint i, num_profiles = 0;

  profiles[num_profiles++] = encoder->profile;
  switch (encoder->profile) {
    case GST_VAAPI_PROFILE_H264_CONSTRAINED_BASELINE:
      profiles[num_profiles++] = GST_VAAPI_PROFILE_H264_BASELINE;
      profiles[num_profiles++] = GST_VAAPI_PROFILE_H264_MAIN;
      // fall-through
    case GST_VAAPI_PROFILE_H264_MAIN:
      profiles[num_profiles++] = GST_VAAPI_PROFILE_H264_HIGH;
      break;
    default:
      break;
  }

  profile = GST_VAAPI_PROFILE_UNKNOWN;
  for (i = 0; i < num_profiles; i++) {
    if (gst_vaapi_display_has_encoder (display, profiles[i], entrypoint)) {
      profile = profiles[i];
      break;
    }
  }
  if (profile == GST_VAAPI_PROFILE_UNKNOWN)
    goto error_unsupported_profile;

  GST_VAAPI_ENCODER_CAST (encoder)->profile = profile;
  return TRUE;

  /* ERRORS */
error_unsupported_profile:
  {
    GST_ERROR ("unsupported HW profile (0x%08x)", encoder->profile);
    return FALSE;
  }
}

/* Check target decoder constraints */
static gboolean
ensure_profile_limits (GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiProfile profile;

  if (!encoder->max_profile_idc
      || encoder->profile_idc <= encoder->max_profile_idc)
    return TRUE;

  GST_WARNING ("lowering coding tools to meet target decoder constraints");

  profile = GST_VAAPI_PROFILE_UNKNOWN;

  /* Try Main profile coding tools */
  if (encoder->max_profile_idc < 100) {
    encoder->use_dct8x8 = FALSE;
    profile = GST_VAAPI_PROFILE_H264_MAIN;
  }

  /* Try Constrained Baseline profile coding tools */
  if (encoder->max_profile_idc < 77) {
    encoder->num_bframes = 0;
    encoder->use_cabac = FALSE;
    profile = GST_VAAPI_PROFILE_H264_CONSTRAINED_BASELINE;
  }

  if (profile) {
    encoder->profile = profile;
    encoder->profile_idc = encoder->max_profile_idc;
  }
  return TRUE;
}

/* Derives the minimum profile from the active coding tools */
static gboolean
ensure_profile (GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiProfile profile;

  /* Always start from "constrained-baseline" profile for maximum
     compatibility */
  profile = GST_VAAPI_PROFILE_H264_CONSTRAINED_BASELINE;

  /* Main profile coding tools */
  if (encoder->num_bframes > 0 || encoder->use_cabac)
    profile = GST_VAAPI_PROFILE_H264_MAIN;

  /* High profile coding tools */
  if (encoder->use_dct8x8)
    profile = GST_VAAPI_PROFILE_H264_HIGH;

  /* MVC profiles coding tools */
  if (encoder->num_views == 2)
    profile = GST_VAAPI_PROFILE_H264_STEREO_HIGH;
  else if (encoder->num_views > 2)
    profile = GST_VAAPI_PROFILE_H264_MULTIVIEW_HIGH;

  encoder->profile = profile;
  encoder->profile_idc = gst_vaapi_utils_h264_get_profile_idc (profile);
  return TRUE;
}

/* Derives the level from the currently set limits */
static gboolean
ensure_level (GstVaapiEncoderH264Fei * encoder)
{
  const guint cpb_factor = h264_get_cpb_nal_factor (encoder->profile);
  const GstVaapiH264LevelLimits *limits_table;
  guint i, num_limits, PicSizeMbs, MaxDpbMbs, MaxMBPS;

  PicSizeMbs = encoder->mb_width * encoder->mb_height;
  MaxDpbMbs = PicSizeMbs * ((encoder->num_bframes) ? 2 : 1);
  MaxMBPS = gst_util_uint64_scale_int_ceil (PicSizeMbs,
      GST_VAAPI_ENCODER_FPS_N (encoder), GST_VAAPI_ENCODER_FPS_D (encoder));

  limits_table = gst_vaapi_utils_h264_get_level_limits_table (&num_limits);
  for (i = 0; i < num_limits; i++) {
    const GstVaapiH264LevelLimits *const limits = &limits_table[i];
    if (PicSizeMbs <= limits->MaxFS &&
        MaxDpbMbs <= limits->MaxDpbMbs &&
        MaxMBPS <= limits->MaxMBPS && (!encoder->bitrate_bits
            || encoder->bitrate_bits <= (limits->MaxBR * cpb_factor)) &&
        (!encoder->cpb_length_bits ||
            encoder->cpb_length_bits <= (limits->MaxCPB * cpb_factor)))
      break;
  }
  if (i == num_limits)
    goto error_unsupported_level;

  encoder->level = limits_table[i].level;
  encoder->level_idc = limits_table[i].level_idc;
  return TRUE;

  /* ERRORS */
error_unsupported_level:
  {
    GST_ERROR ("failed to find a suitable level matching codec config");
    return FALSE;
  }
}

/* Enable "high-compression" tuning options */
static gboolean
ensure_tuning_high_compression (GstVaapiEncoderH264Fei * encoder)
{
  guint8 profile_idc;

  if (!ensure_hw_profile_limits (encoder))
    return FALSE;

  profile_idc = encoder->hw_max_profile_idc;
  if (encoder->max_profile_idc && encoder->max_profile_idc < profile_idc)
    profile_idc = encoder->max_profile_idc;

  /* Tuning options to enable Main profile */
  if (profile_idc >= 77 && profile_idc != 88) {
    encoder->use_cabac = TRUE;
    if (!encoder->num_bframes)
      encoder->num_bframes = 1;
  }

  /* Tuning options to enable High profile */
  if (profile_idc >= 100) {
    encoder->use_dct8x8 = TRUE;
  }
  return TRUE;
}

/* Ensure tuning options */
static gboolean
ensure_tuning (GstVaapiEncoderH264Fei * encoder)
{
  gboolean success;

  switch (GST_VAAPI_ENCODER_TUNE (encoder)) {
    case GST_VAAPI_ENCODER_TUNE_HIGH_COMPRESSION:
      success = ensure_tuning_high_compression (encoder);
      break;
    case GST_VAAPI_ENCODER_TUNE_LOW_POWER:
      /* Set low-power encode entry point. If hardware doesn't have
       * support, it will fail in ensure_hw_profile() in later stage.
       * So not duplicating the profile/entrypont query mechanism
       * here as a part of optimization */
      encoder->entrypoint = GST_VAAPI_ENTRYPOINT_SLICE_ENCODE_LP;
      success = TRUE;
      break;
    default:
      success = TRUE;
      break;
  }
  return success;
}

/* Handle new GOP starts */
static void
reset_gop_start (GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiH264ViewReorderPool *const reorder_pool =
      &encoder->reorder_pools[encoder->view_idx];

  reorder_pool->frame_index = 1;
  reorder_pool->cur_frame_num = 0;
  reorder_pool->cur_present_index = 0;
  ++encoder->idr_num;
}

/* Marks the supplied picture as a B-frame */
static void
set_b_frame (GstVaapiEncPicture * pic, GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiH264ViewReorderPool *const reorder_pool =
      &encoder->reorder_pools[encoder->view_idx];

  g_assert (pic && encoder);
  g_return_if_fail (pic->type == GST_VAAPI_PICTURE_TYPE_NONE);
  pic->type = GST_VAAPI_PICTURE_TYPE_B;
  pic->frame_num = (reorder_pool->cur_frame_num % encoder->max_frame_num);
}

/* Marks the supplied picture as a P-frame */
static void
set_p_frame (GstVaapiEncPicture * pic, GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiH264ViewReorderPool *const reorder_pool =
      &encoder->reorder_pools[encoder->view_idx];

  g_return_if_fail (pic->type == GST_VAAPI_PICTURE_TYPE_NONE);
  pic->type = GST_VAAPI_PICTURE_TYPE_P;
  pic->frame_num = (reorder_pool->cur_frame_num % encoder->max_frame_num);
}

/* Marks the supplied picture as an I-frame */
static void
set_i_frame (GstVaapiEncPicture * pic, GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiH264ViewReorderPool *const reorder_pool =
      &encoder->reorder_pools[encoder->view_idx];

  g_return_if_fail (pic->type == GST_VAAPI_PICTURE_TYPE_NONE);
  pic->type = GST_VAAPI_PICTURE_TYPE_I;
  pic->frame_num = (reorder_pool->cur_frame_num % encoder->max_frame_num);

  g_assert (pic->frame);
  GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (pic->frame);
}

/* Marks the supplied picture as an IDR frame */
static void
set_idr_frame (GstVaapiEncPicture * pic, GstVaapiEncoderH264Fei * encoder)
{
  g_return_if_fail (pic->type == GST_VAAPI_PICTURE_TYPE_NONE);
  pic->type = GST_VAAPI_PICTURE_TYPE_I;
  pic->frame_num = 0;
  pic->poc = 0;
  GST_VAAPI_ENC_PICTURE_FLAG_SET (pic, GST_VAAPI_ENC_PICTURE_FLAG_IDR);

  g_assert (pic->frame);
  GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (pic->frame);
}

/* Marks the supplied picture a a key-frame */
static void
set_key_frame (GstVaapiEncPicture * picture,
    GstVaapiEncoderH264Fei * encoder, gboolean is_idr)
{
  if (is_idr) {
    reset_gop_start (encoder);
    set_idr_frame (picture, encoder);
  } else
    set_i_frame (picture, encoder);
}

/* Fills in VA HRD parameters */
static void
fill_hrd_params (GstVaapiEncoderH264Fei * encoder, VAEncMiscParameterHRD * hrd)
{
  if (encoder->bitrate_bits > 0) {
    hrd->buffer_size = encoder->cpb_length_bits;
    hrd->initial_buffer_fullness = hrd->buffer_size / 2;
  } else {
    hrd->buffer_size = 0;
    hrd->initial_buffer_fullness = 0;
  }
}

/* Adds the supplied sequence header (SPS) to the list of packed
   headers to pass down as-is to the encoder */
static gboolean
add_packed_sequence_header (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiEncSequence * sequence)
{
  GstVaapiEncPackedHeader *packed_seq;
  GstBitWriter bs;
  VAEncPackedHeaderParameterBuffer packed_seq_param = { 0 };
  const VAEncSequenceParameterBufferH264 *const seq_param = sequence->param;
  GstVaapiProfile profile = encoder->profile;

  VAEncMiscParameterHRD hrd_params;
  guint32 data_bit_size;
  guint8 *data;

  fill_hrd_params (encoder, &hrd_params);

  gst_bit_writer_init (&bs, 128 * 8);
  WRITE_UINT32 (&bs, 0x00000001, 32);   /* start code */
  bs_write_nal_header (&bs, GST_H264_NAL_REF_IDC_HIGH, GST_H264_NAL_SPS);

  /* Set High profile for encoding the MVC base view. Otherwise, some
     traditional decoder cannot recognize MVC profile streams with
     only the base view in there */
  if (profile == GST_VAAPI_PROFILE_H264_MULTIVIEW_HIGH ||
      profile == GST_VAAPI_PROFILE_H264_STEREO_HIGH)
    profile = GST_VAAPI_PROFILE_H264_HIGH;

  bs_write_sps (&bs, seq_param, profile, &hrd_params);

  g_assert (GST_BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
  data_bit_size = GST_BIT_WRITER_BIT_SIZE (&bs);
  data = GST_BIT_WRITER_DATA (&bs);

  packed_seq_param.type = VAEncPackedHeaderSequence;
  packed_seq_param.bit_length = data_bit_size;
  packed_seq_param.has_emulation_bytes = 0;

  packed_seq = gst_vaapi_enc_packed_header_new (GST_VAAPI_ENCODER (encoder),
      &packed_seq_param, sizeof (packed_seq_param),
      data, (data_bit_size + 7) / 8);
  g_assert (packed_seq);

  gst_vaapi_enc_picture_add_packed_header (picture, packed_seq);
  gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & packed_seq, NULL);

  /* store sps data */
  _check_sps_pps_status (encoder, data + 4, data_bit_size / 8 - 4);
  gst_bit_writer_clear (&bs, TRUE);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write SPS NAL unit");
    gst_bit_writer_clear (&bs, TRUE);
    return FALSE;
  }
}

static gboolean
add_packed_sequence_header_mvc (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiEncSequence * sequence)
{
  GstVaapiEncPackedHeader *packed_seq;
  GstBitWriter bs;
  VAEncPackedHeaderParameterBuffer packed_header_param_buffer = { 0 };
  const VAEncSequenceParameterBufferH264 *const seq_param = sequence->param;
  VAEncMiscParameterHRD hrd_params;
  guint32 data_bit_size;
  guint8 *data;

  fill_hrd_params (encoder, &hrd_params);

  /* non-base layer, pack one subset sps */
  gst_bit_writer_init (&bs, 128 * 8);
  WRITE_UINT32 (&bs, 0x00000001, 32);   /* start code */
  bs_write_nal_header (&bs, GST_H264_NAL_REF_IDC_HIGH, GST_H264_NAL_SUBSET_SPS);

  bs_write_subset_sps (&bs, seq_param, encoder->profile, encoder->num_views,
      encoder->view_ids, &hrd_params);

  g_assert (GST_BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
  data_bit_size = GST_BIT_WRITER_BIT_SIZE (&bs);
  data = GST_BIT_WRITER_DATA (&bs);

  packed_header_param_buffer.type = VAEncPackedHeaderSequence;
  packed_header_param_buffer.bit_length = data_bit_size;
  packed_header_param_buffer.has_emulation_bytes = 0;

  packed_seq = gst_vaapi_enc_packed_header_new (GST_VAAPI_ENCODER (encoder),
      &packed_header_param_buffer, sizeof (packed_header_param_buffer),
      data, (data_bit_size + 7) / 8);
  g_assert (packed_seq);

  gst_vaapi_enc_picture_add_packed_header (picture, packed_seq);
  gst_vaapi_mini_object_replace ((GstVaapiMiniObject **) & packed_seq, NULL);

  /* store subset sps data */
  _check_sps_pps_status (encoder, data + 4, data_bit_size / 8 - 4);
  gst_bit_writer_clear (&bs, TRUE);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write SPS NAL unit");
    gst_bit_writer_clear (&bs, TRUE);
    return FALSE;
  }
}

/* Adds the supplied picture header (PPS) to the list of packed
   headers to pass down as-is to the encoder */
static gboolean
add_packed_picture_header (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture)
{
  GstVaapiEncPackedHeader *packed_pic;
  GstBitWriter bs;
  VAEncPackedHeaderParameterBuffer packed_pic_param = { 0 };
  const VAEncPictureParameterBufferH264 *const pic_param = picture->param;
  guint32 data_bit_size;
  guint8 *data;

  gst_bit_writer_init (&bs, 128 * 8);
  WRITE_UINT32 (&bs, 0x00000001, 32);   /* start code */
  bs_write_nal_header (&bs, GST_H264_NAL_REF_IDC_HIGH, GST_H264_NAL_PPS);
  bs_write_pps (&bs, pic_param, encoder->profile);
  g_assert (GST_BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
  data_bit_size = GST_BIT_WRITER_BIT_SIZE (&bs);
  data = GST_BIT_WRITER_DATA (&bs);

  packed_pic_param.type = VAEncPackedHeaderPicture;
  packed_pic_param.bit_length = data_bit_size;
  packed_pic_param.has_emulation_bytes = 0;

  packed_pic = gst_vaapi_enc_packed_header_new (GST_VAAPI_ENCODER (encoder),
      &packed_pic_param, sizeof (packed_pic_param),
      data, (data_bit_size + 7) / 8);
  g_assert (packed_pic);

  gst_vaapi_enc_picture_add_packed_header (picture, packed_pic);
  gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & packed_pic, NULL);

  /* store pps data */
  _check_sps_pps_status (encoder, data + 4, data_bit_size / 8 - 4);
  gst_bit_writer_clear (&bs, TRUE);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write PPS NAL unit");
    gst_bit_writer_clear (&bs, TRUE);
    return FALSE;
  }
}

static gboolean
add_packed_sei_header (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiH264SeiPayloadType payloadtype)
{
  GstVaapiEncPackedHeader *packed_sei;
  GstBitWriter bs, bs_buf_period, bs_pic_timing;
  VAEncPackedHeaderParameterBuffer packed_sei_param = { 0 };
  guint32 data_bit_size;
  guint8 buf_period_payload_size = 0, pic_timing_payload_size = 0;
  guint8 *data, *buf_period_payload = NULL, *pic_timing_payload = NULL;
  gboolean need_buf_period, need_pic_timing;

  gst_bit_writer_init (&bs_buf_period, 128 * 8);
  gst_bit_writer_init (&bs_pic_timing, 128 * 8);
  gst_bit_writer_init (&bs, 128 * 8);

  need_buf_period = GST_VAAPI_H264_SEI_BUF_PERIOD & payloadtype;
  need_pic_timing = GST_VAAPI_H264_SEI_PIC_TIMING & payloadtype;

  if (need_buf_period) {
    /* Write a Buffering Period SEI message */
    bs_write_sei_buf_period (&bs_buf_period, encoder, picture);
    /* Write byte alignment bits */
    if (GST_BIT_WRITER_BIT_SIZE (&bs_buf_period) % 8 != 0)
      bs_write_trailing_bits (&bs_buf_period);
    buf_period_payload_size = (GST_BIT_WRITER_BIT_SIZE (&bs_buf_period)) / 8;
    buf_period_payload = GST_BIT_WRITER_DATA (&bs_buf_period);
  }

  if (need_pic_timing) {
    /* Write a Picture Timing SEI message */
    if (GST_VAAPI_H264_SEI_PIC_TIMING & payloadtype)
      bs_write_sei_pic_timing (&bs_pic_timing, encoder, picture);
    /* Write byte alignment bits */
    if (GST_BIT_WRITER_BIT_SIZE (&bs_pic_timing) % 8 != 0)
      bs_write_trailing_bits (&bs_pic_timing);
    pic_timing_payload_size = (GST_BIT_WRITER_BIT_SIZE (&bs_pic_timing)) / 8;
    pic_timing_payload = GST_BIT_WRITER_DATA (&bs_pic_timing);
  }

  /* Write the SEI message */
  WRITE_UINT32 (&bs, 0x00000001, 32);   /* start code */
  bs_write_nal_header (&bs, GST_H264_NAL_REF_IDC_NONE, GST_H264_NAL_SEI);

  if (need_buf_period) {
    WRITE_UINT32 (&bs, GST_H264_SEI_BUF_PERIOD, 8);
    WRITE_UINT32 (&bs, buf_period_payload_size, 8);
    /* Add buffering period sei message */
    gst_bit_writer_put_bytes (&bs, buf_period_payload, buf_period_payload_size);
  }

  if (need_pic_timing) {
    WRITE_UINT32 (&bs, GST_H264_SEI_PIC_TIMING, 8);
    WRITE_UINT32 (&bs, pic_timing_payload_size, 8);
    /* Add picture timing sei message */
    gst_bit_writer_put_bytes (&bs, pic_timing_payload, pic_timing_payload_size);
  }

  /* rbsp_trailing_bits */
  bs_write_trailing_bits (&bs);

  g_assert (GST_BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
  data_bit_size = GST_BIT_WRITER_BIT_SIZE (&bs);
  data = GST_BIT_WRITER_DATA (&bs);

  packed_sei_param.type = VAEncPackedHeaderH264_SEI;
  packed_sei_param.bit_length = data_bit_size;
  packed_sei_param.has_emulation_bytes = 0;

  packed_sei = gst_vaapi_enc_packed_header_new (GST_VAAPI_ENCODER (encoder),
      &packed_sei_param, sizeof (packed_sei_param),
      data, (data_bit_size + 7) / 8);
  g_assert (packed_sei);

  gst_vaapi_enc_picture_add_packed_header (picture, packed_sei);
  gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & packed_sei, NULL);

  gst_bit_writer_clear (&bs_buf_period, TRUE);
  gst_bit_writer_clear (&bs_pic_timing, TRUE);
  gst_bit_writer_clear (&bs, TRUE);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write SEI NAL unit");
    gst_bit_writer_clear (&bs_buf_period, TRUE);
    gst_bit_writer_clear (&bs_pic_timing, TRUE);
    gst_bit_writer_clear (&bs, TRUE);
    return FALSE;
  }
}

static gboolean
get_nal_hdr_attributes (GstVaapiEncPicture * picture,
    guint8 * nal_ref_idc, guint8 * nal_unit_type)
{
  switch (picture->type) {
    case GST_VAAPI_PICTURE_TYPE_I:
      *nal_ref_idc = GST_H264_NAL_REF_IDC_HIGH;
      if (GST_VAAPI_ENC_PICTURE_IS_IDR (picture))
        *nal_unit_type = GST_H264_NAL_SLICE_IDR;
      else
        *nal_unit_type = GST_H264_NAL_SLICE;
      break;
    case GST_VAAPI_PICTURE_TYPE_P:
      *nal_ref_idc = GST_H264_NAL_REF_IDC_MEDIUM;
      *nal_unit_type = GST_H264_NAL_SLICE;
      break;
    case GST_VAAPI_PICTURE_TYPE_B:
      *nal_ref_idc = GST_H264_NAL_REF_IDC_NONE;
      *nal_unit_type = GST_H264_NAL_SLICE;
      break;
    default:
      return FALSE;
  }
  return TRUE;
}

/* Adds the supplied prefix nal header to the list of packed
   headers to pass down as-is to the encoder */
static gboolean
add_packed_prefix_nal_header (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiEncSlice * slice)
{
  GstVaapiEncPackedHeader *packed_prefix_nal;
  GstBitWriter bs;
  VAEncPackedHeaderParameterBuffer packed_prefix_nal_param = { 0 };
  guint32 data_bit_size;
  guint8 *data;
  guint8 nal_ref_idc, nal_unit_type;

  gst_bit_writer_init (&bs, 128 * 8);
  WRITE_UINT32 (&bs, 0x00000001, 32);   /* start code */

  if (!get_nal_hdr_attributes (picture, &nal_ref_idc, &nal_unit_type))
    goto bs_error;
  nal_unit_type = GST_H264_NAL_PREFIX_UNIT;

  bs_write_nal_header (&bs, nal_ref_idc, nal_unit_type);
  bs_write_nal_header_mvc_extension (&bs, picture, encoder->view_idx);
  g_assert (GST_BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
  data_bit_size = GST_BIT_WRITER_BIT_SIZE (&bs);
  data = GST_BIT_WRITER_DATA (&bs);

  packed_prefix_nal_param.type = VAEncPackedHeaderRawData;
  packed_prefix_nal_param.bit_length = data_bit_size;
  packed_prefix_nal_param.has_emulation_bytes = 0;

  packed_prefix_nal =
      gst_vaapi_enc_packed_header_new (GST_VAAPI_ENCODER (encoder),
      &packed_prefix_nal_param, sizeof (packed_prefix_nal_param), data,
      (data_bit_size + 7) / 8);
  g_assert (packed_prefix_nal);

  gst_vaapi_enc_slice_add_packed_header (slice, packed_prefix_nal);
  gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & packed_prefix_nal,
      NULL);

  gst_bit_writer_clear (&bs, TRUE);

  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write Prefix NAL unit header");
    gst_bit_writer_clear (&bs, TRUE);
    return FALSE;
  }
}

/* Adds the supplied slice header to the list of packed
   headers to pass down as-is to the encoder */
static gboolean
add_packed_slice_header (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiEncSlice * slice)
{
  GstVaapiEncPackedHeader *packed_slice;
  GstBitWriter bs;
  VAEncPackedHeaderParameterBuffer packed_slice_param = { 0 };
  const VAEncSliceParameterBufferH264 *const slice_param = slice->param;
  guint32 data_bit_size;
  guint8 *data;
  guint8 nal_ref_idc, nal_unit_type;

  gst_bit_writer_init (&bs, 128 * 8);
  WRITE_UINT32 (&bs, 0x00000001, 32);   /* start code */

  if (!get_nal_hdr_attributes (picture, &nal_ref_idc, &nal_unit_type))
    goto bs_error;
  /* pack nal_unit_header_mvc_extension() for the non base view */
  if (encoder->is_mvc && encoder->view_idx) {
    bs_write_nal_header (&bs, nal_ref_idc, GST_H264_NAL_SLICE_EXT);
    bs_write_nal_header_mvc_extension (&bs, picture,
        encoder->view_ids[encoder->view_idx]);
  } else
    bs_write_nal_header (&bs, nal_ref_idc, nal_unit_type);

  bs_write_slice (&bs, slice_param, encoder, picture);
  data_bit_size = GST_BIT_WRITER_BIT_SIZE (&bs);
  data = GST_BIT_WRITER_DATA (&bs);

  packed_slice_param.type = VAEncPackedHeaderSlice;
  packed_slice_param.bit_length = data_bit_size;
  packed_slice_param.has_emulation_bytes = 0;

  packed_slice = gst_vaapi_enc_packed_header_new (GST_VAAPI_ENCODER (encoder),
      &packed_slice_param, sizeof (packed_slice_param),
      data, (data_bit_size + 7) / 8);
  g_assert (packed_slice);

  gst_vaapi_enc_slice_add_packed_header (slice, packed_slice);
  gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & packed_slice,
      NULL);

  gst_bit_writer_clear (&bs, TRUE);
  return TRUE;

  /* ERRORS */
bs_error:
  {
    GST_WARNING ("failed to write Slice NAL unit header");
    gst_bit_writer_clear (&bs, TRUE);
    return FALSE;
  }
}

/* Reference picture management */
static void
reference_pic_free (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncoderH264FeiRef * ref)
{
  if (!ref)
    return;
  if (ref->pic)
    gst_vaapi_encoder_release_surface (GST_VAAPI_ENCODER (encoder), ref->pic);
  g_slice_free (GstVaapiEncoderH264FeiRef, ref);
}

static inline GstVaapiEncoderH264FeiRef *
reference_pic_create (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiSurfaceProxy * surface)
{
  GstVaapiEncoderH264FeiRef *const ref =
      g_slice_new0 (GstVaapiEncoderH264FeiRef);

  ref->pic = surface;
  ref->frame_num = picture->frame_num;
  ref->poc = picture->poc;
  return ref;
}

static gboolean
reference_list_update (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiSurfaceProxy * surface)
{
  GstVaapiEncoderH264FeiRef *ref;
  GstVaapiH264ViewRefPool *const ref_pool =
      &encoder->ref_pools[encoder->view_idx];

  if (GST_VAAPI_PICTURE_TYPE_B == picture->type) {
    gst_vaapi_encoder_release_surface (GST_VAAPI_ENCODER (encoder), surface);
    return TRUE;
  }
  if (GST_VAAPI_ENC_PICTURE_IS_IDR (picture)) {
    while (!g_queue_is_empty (&ref_pool->ref_list))
      reference_pic_free (encoder, g_queue_pop_head (&ref_pool->ref_list));
  } else if (g_queue_get_length (&ref_pool->ref_list) >=
      ref_pool->max_ref_frames) {
    reference_pic_free (encoder, g_queue_pop_head (&ref_pool->ref_list));
  }
  ref = reference_pic_create (encoder, picture, surface);
  g_queue_push_tail (&ref_pool->ref_list, ref);
  g_assert (g_queue_get_length (&ref_pool->ref_list) <=
      ref_pool->max_ref_frames);
  return TRUE;
}

static gboolean
reference_list_init (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture,
    GstVaapiEncoderH264FeiRef ** reflist_0,
    guint * reflist_0_count,
    GstVaapiEncoderH264FeiRef ** reflist_1, guint * reflist_1_count)
{
  GstVaapiEncoderH264FeiRef *tmp;
  GstVaapiH264ViewRefPool *const ref_pool =
      &encoder->ref_pools[encoder->view_idx];
  GList *iter, *list_0_start = NULL, *list_1_start = NULL;
  guint count;

  *reflist_0_count = 0;
  *reflist_1_count = 0;
  if (picture->type == GST_VAAPI_PICTURE_TYPE_I)
    return TRUE;

  iter = g_queue_peek_tail_link (&ref_pool->ref_list);
  for (; iter; iter = g_list_previous (iter)) {
    tmp = (GstVaapiEncoderH264FeiRef *) iter->data;
    g_assert (tmp && tmp->poc != picture->poc);
    if (_poc_greater_than (picture->poc, tmp->poc, encoder->max_pic_order_cnt)) {
      list_0_start = iter;
      list_1_start = g_list_next (iter);
      break;
    }
  }

  /* order reflist_0 */
  g_assert (list_0_start);
  iter = list_0_start;
  count = 0;
  for (; iter; iter = g_list_previous (iter)) {
    reflist_0[count] = (GstVaapiEncoderH264FeiRef *) iter->data;
    ++count;
  }
  *reflist_0_count = count;

  if (picture->type != GST_VAAPI_PICTURE_TYPE_B)
    return TRUE;

  /* order reflist_1 */
  count = 0;
  iter = list_1_start;
  for (; iter; iter = g_list_next (iter)) {
    reflist_1[count] = (GstVaapiEncoderH264FeiRef *) iter->data;
    ++count;
  }
  *reflist_1_count = count;
  return TRUE;
}

/* Fills in VA sequence parameter buffer */
static gboolean
fill_sequence (GstVaapiEncoderH264Fei * encoder, GstVaapiEncSequence * sequence)
{
  VAEncSequenceParameterBufferH264 *const seq_param = sequence->param;
  GstVaapiH264ViewRefPool *const ref_pool =
      &encoder->ref_pools[encoder->view_idx];

  memset (seq_param, 0, sizeof (VAEncSequenceParameterBufferH264));
  seq_param->seq_parameter_set_id = encoder->view_idx;
  seq_param->level_idc = encoder->level_idc;
  seq_param->intra_period = GST_VAAPI_ENCODER_KEYFRAME_PERIOD (encoder);
  seq_param->intra_idr_period = GST_VAAPI_ENCODER_KEYFRAME_PERIOD (encoder);
  seq_param->ip_period = 1 + encoder->num_bframes;
  seq_param->ip_period = seq_param->intra_period > 1 ?
      (1 + encoder->num_bframes) : 0;
  seq_param->bits_per_second = encoder->bitrate_bits;

  seq_param->max_num_ref_frames = ref_pool->max_ref_frames;
  seq_param->picture_width_in_mbs = encoder->mb_width;
  seq_param->picture_height_in_mbs = encoder->mb_height;

  /*sequence field values */
  seq_param->seq_fields.value = 0;
  seq_param->seq_fields.bits.chroma_format_idc = 1;
  seq_param->seq_fields.bits.frame_mbs_only_flag = 1;
  seq_param->seq_fields.bits.mb_adaptive_frame_field_flag = FALSE;
  seq_param->seq_fields.bits.seq_scaling_matrix_present_flag = FALSE;
  /* direct_8x8_inference_flag default false */
  seq_param->seq_fields.bits.direct_8x8_inference_flag = FALSE;
  g_assert (encoder->log2_max_frame_num >= 4);
  seq_param->seq_fields.bits.log2_max_frame_num_minus4 =
      encoder->log2_max_frame_num - 4;
  /* picture order count */
  encoder->pic_order_cnt_type = seq_param->seq_fields.bits.pic_order_cnt_type =
      0;
  g_assert (encoder->log2_max_pic_order_cnt >= 4);
  seq_param->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 =
      encoder->log2_max_pic_order_cnt - 4;

  seq_param->bit_depth_luma_minus8 = 0;
  seq_param->bit_depth_chroma_minus8 = 0;

  /* not used if pic_order_cnt_type == 0 */
  if (seq_param->seq_fields.bits.pic_order_cnt_type == 1) {
    encoder->delta_pic_order_always_zero_flag =
        seq_param->seq_fields.bits.delta_pic_order_always_zero_flag = TRUE;
    seq_param->num_ref_frames_in_pic_order_cnt_cycle = 0;
    seq_param->offset_for_non_ref_pic = 0;
    seq_param->offset_for_top_to_bottom_field = 0;
    memset (seq_param->offset_for_ref_frame, 0,
        sizeof (seq_param->offset_for_ref_frame));
  }

  /* frame_cropping_flag */
  if ((GST_VAAPI_ENCODER_WIDTH (encoder) & 15) ||
      (GST_VAAPI_ENCODER_HEIGHT (encoder) & 15)) {
    static const guint SubWidthC[] = { 1, 2, 2, 1 };
    static const guint SubHeightC[] = { 1, 2, 1, 1 };
    const guint CropUnitX =
        SubWidthC[seq_param->seq_fields.bits.chroma_format_idc];
    const guint CropUnitY =
        SubHeightC[seq_param->seq_fields.bits.chroma_format_idc] *
        (2 - seq_param->seq_fields.bits.frame_mbs_only_flag);

    seq_param->frame_cropping_flag = 1;
    seq_param->frame_crop_left_offset = 0;
    seq_param->frame_crop_right_offset =
        (16 * encoder->mb_width -
        GST_VAAPI_ENCODER_WIDTH (encoder)) / CropUnitX;
    seq_param->frame_crop_top_offset = 0;
    seq_param->frame_crop_bottom_offset =
        (16 * encoder->mb_height -
        GST_VAAPI_ENCODER_HEIGHT (encoder)) / CropUnitY;
  }

  /* VUI parameters are always set, at least for timing_info (framerate) */
  seq_param->vui_parameters_present_flag = TRUE;
  if (seq_param->vui_parameters_present_flag) {
    seq_param->vui_fields.bits.aspect_ratio_info_present_flag = TRUE;
    if (seq_param->vui_fields.bits.aspect_ratio_info_present_flag) {
      const GstVideoInfo *const vip = GST_VAAPI_ENCODER_VIDEO_INFO (encoder);
      seq_param->aspect_ratio_idc = 0xff;
      seq_param->sar_width = GST_VIDEO_INFO_PAR_N (vip);
      seq_param->sar_height = GST_VIDEO_INFO_PAR_D (vip);
    }
    seq_param->vui_fields.bits.bitstream_restriction_flag = FALSE;
    /* if vui_parameters_present_flag is TRUE and sps data belongs to
     * subset sps, timing_info_preset_flag should be zero (H.7.4.2.1.1) */
    seq_param->vui_fields.bits.timing_info_present_flag = !encoder->view_idx;
    if (seq_param->vui_fields.bits.timing_info_present_flag) {
      seq_param->num_units_in_tick = GST_VAAPI_ENCODER_FPS_D (encoder);
      seq_param->time_scale = GST_VAAPI_ENCODER_FPS_N (encoder) * 2;
    }
  }
  return TRUE;
}

/* Fills in VA picture parameter buffer */
static gboolean
fill_picture (GstVaapiEncoderH264Fei * encoder, GstVaapiEncPicture * picture,
    GstVaapiCodedBuffer * codedbuf, GstVaapiSurfaceProxy * surface)
{
  VAEncPictureParameterBufferH264 *const pic_param = picture->param;
  GstVaapiH264ViewRefPool *const ref_pool =
      &encoder->ref_pools[encoder->view_idx];
  GstVaapiEncoderH264FeiRef *ref_pic;
  GList *reflist;
  guint i;

  memset (pic_param, 0, sizeof (VAEncPictureParameterBufferH264));

  /* reference list,  */
  pic_param->CurrPic.picture_id = GST_VAAPI_SURFACE_PROXY_SURFACE_ID (surface);
  pic_param->CurrPic.TopFieldOrderCnt = picture->poc;
  i = 0;
  if (picture->type != GST_VAAPI_PICTURE_TYPE_I) {
    for (reflist = g_queue_peek_head_link (&ref_pool->ref_list);
        reflist; reflist = g_list_next (reflist)) {
      ref_pic = reflist->data;
      g_assert (ref_pic && ref_pic->pic &&
          GST_VAAPI_SURFACE_PROXY_SURFACE_ID (ref_pic->pic) != VA_INVALID_ID);

      pic_param->ReferenceFrames[i].picture_id =
          GST_VAAPI_SURFACE_PROXY_SURFACE_ID (ref_pic->pic);
      pic_param->ReferenceFrames[i].TopFieldOrderCnt = ref_pic->poc;
      pic_param->ReferenceFrames[i].flags |=
          VA_PICTURE_H264_SHORT_TERM_REFERENCE;
      pic_param->ReferenceFrames[i].frame_idx = ref_pic->frame_num;

      ++i;
    }
    g_assert (i <= 16 && i <= ref_pool->max_ref_frames);
  }
  for (; i < 16; ++i) {
    pic_param->ReferenceFrames[i].picture_id = VA_INVALID_ID;
  }
  pic_param->coded_buf = GST_VAAPI_OBJECT_ID (codedbuf);

  pic_param->pic_parameter_set_id = encoder->view_idx;
  pic_param->seq_parameter_set_id = encoder->view_idx ? 1 : 0;
  pic_param->last_picture = 0;  /* means last encoding picture */
  pic_param->frame_num = picture->frame_num;
  pic_param->pic_init_qp = encoder->init_qp;
  pic_param->num_ref_idx_l0_active_minus1 =
      (ref_pool->max_reflist0_count ? (ref_pool->max_reflist0_count - 1) : 0);
  pic_param->num_ref_idx_l1_active_minus1 =
      (ref_pool->max_reflist1_count ? (ref_pool->max_reflist1_count - 1) : 0);
  pic_param->chroma_qp_index_offset = 0;
  pic_param->second_chroma_qp_index_offset = 0;

  /* set picture fields */
  pic_param->pic_fields.value = 0;
  pic_param->pic_fields.bits.idr_pic_flag =
      GST_VAAPI_ENC_PICTURE_IS_IDR (picture);
  pic_param->pic_fields.bits.reference_pic_flag =
      (picture->type != GST_VAAPI_PICTURE_TYPE_B);
  pic_param->pic_fields.bits.entropy_coding_mode_flag = encoder->use_cabac;
  pic_param->pic_fields.bits.weighted_pred_flag = FALSE;
  pic_param->pic_fields.bits.weighted_bipred_idc = 0;
  pic_param->pic_fields.bits.constrained_intra_pred_flag = 0;
  pic_param->pic_fields.bits.transform_8x8_mode_flag = encoder->use_dct8x8;
  /* enable debloking */
  pic_param->pic_fields.bits.deblocking_filter_control_present_flag = TRUE;
  pic_param->pic_fields.bits.redundant_pic_cnt_present_flag = FALSE;
  /* bottom_field_pic_order_in_frame_present_flag */
  pic_param->pic_fields.bits.pic_order_present_flag = FALSE;
  pic_param->pic_fields.bits.pic_scaling_matrix_present_flag = FALSE;

  return TRUE;
}

/* Adds slice headers to picture */
static gboolean
add_slice_headers (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiEncoderH264FeiRef ** reflist_0,
    guint reflist_0_count, GstVaapiEncoderH264FeiRef ** reflist_1,
    guint reflist_1_count)
{
  VAEncSliceParameterBufferH264 *slice_param;
  GstVaapiEncSlice *slice;
  guint slice_of_mbs, slice_mod_mbs, cur_slice_mbs;
  guint mb_size;
  guint last_mb_index;
  guint i_slice, i_ref;

  g_assert (picture);

  mb_size = encoder->mb_width * encoder->mb_height;

  g_assert (encoder->num_slices && encoder->num_slices < mb_size);
  slice_of_mbs = mb_size / encoder->num_slices;
  slice_mod_mbs = mb_size % encoder->num_slices;
  last_mb_index = 0;
  for (i_slice = 0; i_slice < encoder->num_slices; ++i_slice) {
    cur_slice_mbs = slice_of_mbs;
    if (slice_mod_mbs) {
      ++cur_slice_mbs;
      --slice_mod_mbs;
    }
    slice = GST_VAAPI_ENC_SLICE_NEW (H264, encoder);
    g_assert (slice && slice->param_id != VA_INVALID_ID);
    slice_param = slice->param;

    memset (slice_param, 0, sizeof (VAEncSliceParameterBufferH264));
    slice_param->macroblock_address = last_mb_index;
    slice_param->num_macroblocks = cur_slice_mbs;
    slice_param->macroblock_info = VA_INVALID_ID;
    slice_param->slice_type = h264_get_slice_type (picture->type);
    g_assert ((gint8) slice_param->slice_type != -1);
    slice_param->pic_parameter_set_id = encoder->view_idx;
    slice_param->idr_pic_id = encoder->idr_num;
    slice_param->pic_order_cnt_lsb = picture->poc;

    /* not used if pic_order_cnt_type = 0 */
    slice_param->delta_pic_order_cnt_bottom = 0;
    memset (slice_param->delta_pic_order_cnt, 0,
        sizeof (slice_param->delta_pic_order_cnt));

    /* only works for B frames */
    if (slice_param->slice_type == GST_H264_B_SLICE)
      slice_param->direct_spatial_mv_pred_flag = TRUE;
    /* default equal to picture parameters */
    slice_param->num_ref_idx_active_override_flag = FALSE;
    if (picture->type != GST_VAAPI_PICTURE_TYPE_I && reflist_0_count > 0)
      slice_param->num_ref_idx_l0_active_minus1 = reflist_0_count - 1;
    else
      slice_param->num_ref_idx_l0_active_minus1 = 0;
    if (picture->type == GST_VAAPI_PICTURE_TYPE_B && reflist_1_count > 0)
      slice_param->num_ref_idx_l1_active_minus1 = reflist_1_count - 1;
    else
      slice_param->num_ref_idx_l1_active_minus1 = 0;
    g_assert (slice_param->num_ref_idx_l0_active_minus1 == 0);
    g_assert (slice_param->num_ref_idx_l1_active_minus1 == 0);

    i_ref = 0;
    if (picture->type != GST_VAAPI_PICTURE_TYPE_I) {
      for (; i_ref < reflist_0_count; ++i_ref) {
        slice_param->RefPicList0[i_ref].picture_id =
            GST_VAAPI_SURFACE_PROXY_SURFACE_ID (reflist_0[i_ref]->pic);
        slice_param->RefPicList0[i_ref].TopFieldOrderCnt =
            reflist_0[i_ref]->poc;
        slice_param->RefPicList0[i_ref].flags |=
            VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        slice_param->RefPicList0[i_ref].frame_idx = reflist_0[i_ref]->frame_num;
      }
      g_assert (i_ref == 1);
    }
    for (; i_ref < G_N_ELEMENTS (slice_param->RefPicList0); ++i_ref) {
      slice_param->RefPicList0[i_ref].picture_id = VA_INVALID_SURFACE;
    }

    i_ref = 0;
    if (picture->type == GST_VAAPI_PICTURE_TYPE_B) {
      for (; i_ref < reflist_1_count; ++i_ref) {
        slice_param->RefPicList1[i_ref].picture_id =
            GST_VAAPI_SURFACE_PROXY_SURFACE_ID (reflist_1[i_ref]->pic);
        slice_param->RefPicList1[i_ref].TopFieldOrderCnt =
            reflist_1[i_ref]->poc;
        slice_param->RefPicList1[i_ref].flags |=
            VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        slice_param->RefPicList1[i_ref].frame_idx = reflist_1[i_ref]->frame_num;
      }
      g_assert (i_ref == 1);
    }
    for (; i_ref < G_N_ELEMENTS (slice_param->RefPicList1); ++i_ref) {
      slice_param->RefPicList1[i_ref].picture_id = VA_INVALID_SURFACE;
    }

    /* not used if  pic_param.pic_fields.bits.weighted_pred_flag == FALSE */
    slice_param->luma_log2_weight_denom = 0;
    slice_param->chroma_log2_weight_denom = 0;
    slice_param->luma_weight_l0_flag = FALSE;
    memset (slice_param->luma_weight_l0, 0,
        sizeof (slice_param->luma_weight_l0));
    memset (slice_param->luma_offset_l0, 0,
        sizeof (slice_param->luma_offset_l0));
    slice_param->chroma_weight_l0_flag = FALSE;
    memset (slice_param->chroma_weight_l0, 0,
        sizeof (slice_param->chroma_weight_l0));
    memset (slice_param->chroma_offset_l0, 0,
        sizeof (slice_param->chroma_offset_l0));
    slice_param->luma_weight_l1_flag = FALSE;
    memset (slice_param->luma_weight_l1, 0,
        sizeof (slice_param->luma_weight_l1));
    memset (slice_param->luma_offset_l1, 0,
        sizeof (slice_param->luma_offset_l1));
    slice_param->chroma_weight_l1_flag = FALSE;
    memset (slice_param->chroma_weight_l1, 0,
        sizeof (slice_param->chroma_weight_l1));
    memset (slice_param->chroma_offset_l1, 0,
        sizeof (slice_param->chroma_offset_l1));

    slice_param->cabac_init_idc = 0;
    slice_param->slice_qp_delta = encoder->init_qp - encoder->min_qp;
    if (slice_param->slice_qp_delta > 4)
      slice_param->slice_qp_delta = 4;
    slice_param->disable_deblocking_filter_idc = 0;
    slice_param->slice_alpha_c0_offset_div2 = 2;
    slice_param->slice_beta_offset_div2 = 2;

    /* set calculation for next slice */
    last_mb_index += cur_slice_mbs;

    /* add packed Prefix NAL unit before each Coded slice NAL in base view */
    if (encoder->is_mvc && !encoder->view_idx &&
        (GST_VAAPI_ENCODER_PACKED_HEADERS (encoder) &
            VA_ENC_PACKED_HEADER_RAW_DATA)
        && !add_packed_prefix_nal_header (encoder, picture, slice))
      goto error_create_packed_prefix_nal_hdr;
    if ((GST_VAAPI_ENCODER_PACKED_HEADERS (encoder) &
            VA_ENC_PACKED_HEADER_SLICE)
        && !add_packed_slice_header (encoder, picture, slice))
      goto error_create_packed_slice_hdr;

    gst_vaapi_enc_picture_add_slice (picture, slice);
    gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & slice, NULL);
  }
  g_assert (last_mb_index == mb_size);
  return TRUE;

error_create_packed_slice_hdr:
  {
    GST_ERROR ("failed to create packed slice header buffer");
    gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & slice, NULL);
    return FALSE;
  }
error_create_packed_prefix_nal_hdr:
  {
    GST_ERROR ("failed to create packed prefix nal header buffer");
    gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & slice, NULL);
    return FALSE;
  }
}

/* Generates and submits SPS header accordingly into the bitstream */
static gboolean
ensure_sequence (GstVaapiEncoderH264Fei * encoder, GstVaapiEncPicture * picture)
{
  GstVaapiEncSequence *sequence = NULL;

  /* submit an SPS header before every new I-frame, if codec config changed */
  if (!encoder->config_changed || picture->type != GST_VAAPI_PICTURE_TYPE_I)
    return TRUE;

  sequence = GST_VAAPI_ENC_SEQUENCE_NEW (H264, encoder);
  if (!sequence || !fill_sequence (encoder, sequence))
    goto error_create_seq_param;

  /* add subset sps for non-base view and sps for base view */
  if (encoder->is_mvc && encoder->view_idx) {
    if ((GST_VAAPI_ENCODER_PACKED_HEADERS (encoder) &
            VA_ENC_PACKED_HEADER_SEQUENCE)
        && !add_packed_sequence_header_mvc (encoder, picture, sequence))
      goto error_create_packed_seq_hdr;
  } else {
    if ((GST_VAAPI_ENCODER_PACKED_HEADERS (encoder) &
            VA_ENC_PACKED_HEADER_SEQUENCE)
        && !add_packed_sequence_header (encoder, picture, sequence))
      goto error_create_packed_seq_hdr;
  }

  if (sequence) {
    gst_vaapi_enc_picture_set_sequence (picture, sequence);
    gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & sequence, NULL);
  }

  if (!encoder->is_mvc || encoder->view_idx > 0)
    encoder->config_changed = FALSE;
  return TRUE;

  /* ERRORS */
error_create_seq_param:
  {
    GST_ERROR ("failed to create sequence parameter buffer (SPS)");
    gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & sequence, NULL);
    return FALSE;
  }
error_create_packed_seq_hdr:
  {
    GST_ERROR ("failed to create packed sequence header buffer");
    gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) & sequence, NULL);
    return FALSE;
  }
}

/* Generates additional fei control parameters */
static gboolean
ensure_fei_misc_params (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture, GstVaapiCodedBufferProxy * codedbuf_proxy)
{
  GstVaapiEncMiscParam *misc = NULL;
  GstVaapiSurfaceProxy *surface_proxy = NULL;

  VAEncMiscParameterFEIFrameControlH264 *misc_fei_pic_control_param;
  guint mbcode_size = 0;
  guint mv_size = 0;
  guint dist_size = 0;
  gboolean enable_out = FALSE;

  /* fei pic control params */
  misc = GST_VAAPI_ENC_FEI_MISC_PARAM_NEW (H264, encoder);
  g_assert (misc);
  if (!misc)
    return FALSE;
  misc_fei_pic_control_param = misc->data;
  surface_proxy = picture->proxy;

  enable_out = ((encoder->is_stats_out_enabled &&
          (encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC_PAK)) ||
      (encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC)) ? TRUE : FALSE;

  misc_fei_pic_control_param->function = encoder->fei_mode;
  misc_fei_pic_control_param->search_path = encoder->search_path;
  misc_fei_pic_control_param->num_mv_predictors_l0 =
      encoder->num_mv_predictors_l0;
  misc_fei_pic_control_param->num_mv_predictors_l1 =
      encoder->num_mv_predictors_l1;
  misc_fei_pic_control_param->len_sp = encoder->len_sp;
  misc_fei_pic_control_param->sub_mb_part_mask = encoder->submb_part_mask;
  if (!encoder->use_dct8x8)
    misc_fei_pic_control_param->intra_part_mask = encoder->intra_part_mask | 2;
  misc_fei_pic_control_param->multi_pred_l0 = encoder->multi_predL0;
  misc_fei_pic_control_param->multi_pred_l1 = encoder->multi_predL1;
  misc_fei_pic_control_param->sub_pel_mode = encoder->subpel_mode;
  misc_fei_pic_control_param->inter_sad = encoder->inter_sad;
  misc_fei_pic_control_param->intra_sad = encoder->intra_sad;
  misc_fei_pic_control_param->distortion_type = 0;
  misc_fei_pic_control_param->repartition_check_enable = 0;
  misc_fei_pic_control_param->adaptive_search = encoder->adaptive_search;
  misc_fei_pic_control_param->mb_size_ctrl = 0;
  misc_fei_pic_control_param->ref_width = encoder->ref_width;
  misc_fei_pic_control_param->ref_height = encoder->ref_height;
  misc_fei_pic_control_param->search_window = encoder->search_window;

  if ((encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC_PAK) ||
      (encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC)) {

    /*****  ENC_PAK/ENC input: mv_predictor *****/
    if (surface_proxy->mvpred) {
      misc_fei_pic_control_param->mv_predictor =
          GST_VAAPI_FEI_CODEC_OBJECT (surface_proxy->mvpred)->param_id;
      misc_fei_pic_control_param->mv_predictor_enable = TRUE;
      gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) &
          picture->mvpred, surface_proxy->mvpred);
    } else {
      misc_fei_pic_control_param->mv_predictor = VA_INVALID_ID;
      misc_fei_pic_control_param->mv_predictor_enable = FALSE;
      picture->mvpred = NULL;
    }

    /*****  ENC_PAK/ENC input: qp ******/
    if (surface_proxy->qp) {
      misc_fei_pic_control_param->qp =
          GST_VAAPI_FEI_CODEC_OBJECT (surface_proxy->qp)->param_id;
      misc_fei_pic_control_param->mb_qp = TRUE;
      gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) &
          picture->qp, surface_proxy->qp);
    } else {
      misc_fei_pic_control_param->qp = VA_INVALID_ID;
      misc_fei_pic_control_param->mb_qp = FALSE;
      picture->qp = NULL;
    }

    /*****  ENC_PAK/ENC input: mb_control ******/
    if (surface_proxy->mbcntrl) {
      misc_fei_pic_control_param->mb_ctrl =
          GST_VAAPI_FEI_CODEC_OBJECT (surface_proxy->mbcntrl)->param_id;
      misc_fei_pic_control_param->mb_input = TRUE;
      gst_vaapi_codec_object_replace ((GstVaapiCodecObject **) &
          picture->mbcntrl, surface_proxy->mbcntrl);
    } else {
      misc_fei_pic_control_param->mb_ctrl = VA_INVALID_ID;
      misc_fei_pic_control_param->mb_input = FALSE;
      picture->mbcntrl = NULL;
    }
  }

  if (enable_out) {

    mbcode_size = sizeof (VAEncFEIMBCodeH264) *
        encoder->mb_width * encoder->mb_height;
    mv_size = sizeof (VAMotionVector) * 16 *
        encoder->mb_width * encoder->mb_height;
    dist_size = sizeof (VAEncFEIDistortionH264) *
        encoder->mb_width * encoder->mb_height;

    /***** ENC_PAK/ENC output: macroblock code buffer *****/
    codedbuf_proxy->mbcode =
        gst_vaapi_enc_fei_mb_code_new (GST_VAAPI_ENCODER_CAST (encoder),
        NULL, mbcode_size);
    misc_fei_pic_control_param->mb_code_data =
        GST_VAAPI_FEI_CODEC_OBJECT (codedbuf_proxy->mbcode)->param_id;
    picture->mbcode = gst_vaapi_codec_object_ref (codedbuf_proxy->mbcode);

    /***** ENC_PAK/ENC output: motion vector buffer *****/
    codedbuf_proxy->mv =
        gst_vaapi_enc_fei_mv_new (GST_VAAPI_ENCODER_CAST (encoder), NULL,
        mv_size);
    misc_fei_pic_control_param->mv_data =
        GST_VAAPI_FEI_CODEC_OBJECT (codedbuf_proxy->mv)->param_id;
    picture->mv = gst_vaapi_codec_object_ref (codedbuf_proxy->mv);

    /***** ENC_PAK/ENC output: distortion buffer *****/
    codedbuf_proxy->dist =
        gst_vaapi_enc_fei_distortion_new (GST_VAAPI_ENCODER_CAST (encoder),
        NULL, dist_size);
    misc_fei_pic_control_param->distortion =
        GST_VAAPI_FEI_CODEC_OBJECT (codedbuf_proxy->dist)->param_id;
    picture->dist = gst_vaapi_codec_object_ref (codedbuf_proxy->dist);

  } else if (encoder->fei_mode == GST_VAAPI_FEI_MODE_PAK) {

    g_assert (surface_proxy->mbcode != NULL);
    g_assert (surface_proxy->mv != NULL);

    /***** PAK input: macroblock code buffer *****/
    misc_fei_pic_control_param->mb_code_data =
        GST_VAAPI_FEI_CODEC_OBJECT (surface_proxy->mbcode)->param_id;
    picture->mbcode = gst_vaapi_codec_object_ref (surface_proxy->mbcode);

    /***** PAK input: motion vector buffer  *****/
    misc_fei_pic_control_param->mv_data =
        GST_VAAPI_FEI_CODEC_OBJECT (surface_proxy->mv)->param_id;
    picture->mv = gst_vaapi_codec_object_ref (surface_proxy->mv);
  } else {

    codedbuf_proxy->mbcode = picture->mbcode = NULL;
    codedbuf_proxy->mv = picture->mv = NULL;
    codedbuf_proxy->dist = picture->dist = NULL;
    misc_fei_pic_control_param->mb_code_data = VA_INVALID_ID;
    misc_fei_pic_control_param->mv_data = VA_INVALID_ID;
    misc_fei_pic_control_param->distortion = VA_INVALID_ID;
  }

  gst_vaapi_enc_picture_add_misc_param (picture, misc);
  gst_vaapi_codec_object_replace (&misc, NULL);
  return TRUE;
}

/* Generates additional control parameters */
static gboolean
ensure_misc_params (GstVaapiEncoderH264Fei * encoder,
    GstVaapiEncPicture * picture)
{
  GstVaapiEncMiscParam *misc = NULL;
  VAEncMiscParameterRateControl *rate_control;

  /* HRD params */
  misc = GST_VAAPI_ENC_MISC_PARAM_NEW (HRD, encoder);
  g_assert (misc);
  if (!misc)
    return FALSE;
  fill_hrd_params (encoder, misc->data);
  gst_vaapi_enc_picture_add_misc_param (picture, misc);
  gst_vaapi_codec_object_replace (&misc, NULL);

  /* RateControl params */
  if (GST_VAAPI_ENCODER_RATE_CONTROL (encoder) == GST_VAAPI_RATECONTROL_CBR ||
      GST_VAAPI_ENCODER_RATE_CONTROL (encoder) == GST_VAAPI_RATECONTROL_VBR) {
    misc = GST_VAAPI_ENC_MISC_PARAM_NEW (RateControl, encoder);
    g_assert (misc);
    if (!misc)
      return FALSE;
    rate_control = misc->data;
    memset (rate_control, 0, sizeof (VAEncMiscParameterRateControl));
    rate_control->bits_per_second = encoder->bitrate_bits;
    rate_control->target_percentage = 70;
    rate_control->window_size = encoder->cpb_length;
    rate_control->initial_qp = encoder->init_qp;
    rate_control->min_qp = encoder->min_qp;
    rate_control->basic_unit_size = 0;
    gst_vaapi_enc_picture_add_misc_param (picture, misc);
    gst_vaapi_codec_object_replace (&misc, NULL);

    if (!encoder->view_idx) {
      if ((GST_VAAPI_ENC_PICTURE_IS_IDR (picture)) &&
          (GST_VAAPI_ENCODER_PACKED_HEADERS (encoder) &
              VA_ENC_PACKED_HEADER_MISC) &&
          !add_packed_sei_header (encoder, picture,
              GST_VAAPI_H264_SEI_BUF_PERIOD | GST_VAAPI_H264_SEI_PIC_TIMING))
        goto error_create_packed_sei_hdr;

      else if (!GST_VAAPI_ENC_PICTURE_IS_IDR (picture) &&
          (GST_VAAPI_ENCODER_PACKED_HEADERS (encoder) &
              VA_ENC_PACKED_HEADER_MISC) &&
          !add_packed_sei_header (encoder, picture,
              GST_VAAPI_H264_SEI_PIC_TIMING))
        goto error_create_packed_sei_hdr;
    }

  }
  return TRUE;

error_create_packed_sei_hdr:
  {
    GST_ERROR ("failed to create packed SEI header");
    return FALSE;
  }
}

/* Generates and submits PPS header accordingly into the bitstream */
static gboolean
ensure_picture (GstVaapiEncoderH264Fei * encoder, GstVaapiEncPicture * picture,
    GstVaapiCodedBufferProxy * codedbuf_proxy, GstVaapiSurfaceProxy * surface)
{
  GstVaapiCodedBuffer *const codedbuf =
      GST_VAAPI_CODED_BUFFER_PROXY_BUFFER (codedbuf_proxy);
  gboolean res = FALSE;

  res = fill_picture (encoder, picture, codedbuf, surface);

  if (!res)
    return FALSE;

  if (picture->type == GST_VAAPI_PICTURE_TYPE_I &&
      (GST_VAAPI_ENCODER_PACKED_HEADERS (encoder) &
          VA_ENC_PACKED_HEADER_PICTURE)
      && !add_packed_picture_header (encoder, picture)) {
    GST_ERROR ("set picture packed header failed");
    return FALSE;
  }
  return TRUE;
}

/* Generates slice headers */
static gboolean
ensure_slices (GstVaapiEncoderH264Fei * encoder, GstVaapiEncPicture * picture)
{
  GstVaapiEncoderH264FeiRef *reflist_0[16];
  GstVaapiEncoderH264FeiRef *reflist_1[16];
  GstVaapiH264ViewRefPool *const ref_pool =
      &encoder->ref_pools[encoder->view_idx];
  guint reflist_0_count = 0, reflist_1_count = 0;

  g_assert (picture);

  if (picture->type != GST_VAAPI_PICTURE_TYPE_I &&
      !reference_list_init (encoder, picture,
          reflist_0, &reflist_0_count, reflist_1, &reflist_1_count)) {
    GST_ERROR ("reference list reorder failed");
    return FALSE;
  }

  g_assert (reflist_0_count + reflist_1_count <= ref_pool->max_ref_frames);
  if (reflist_0_count > ref_pool->max_reflist0_count)
    reflist_0_count = ref_pool->max_reflist0_count;
  if (reflist_1_count > ref_pool->max_reflist1_count)
    reflist_1_count = ref_pool->max_reflist1_count;

  if (!add_slice_headers (encoder, picture,
          reflist_0, reflist_0_count, reflist_1, reflist_1_count))
    return FALSE;

  return TRUE;
}

/* Normalizes bitrate (and CPB size) for HRD conformance */
static void
ensure_bitrate_hrd (GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiEncoder *const base_encoder = GST_VAAPI_ENCODER_CAST (encoder);
  guint bitrate, cpb_size;

  if (!base_encoder->bitrate) {
    encoder->bitrate_bits = 0;
    return;
  }

  /* Round down bitrate. This is a hard limit mandated by the user */
  g_assert (SX_BITRATE >= 6);
  bitrate = (base_encoder->bitrate * 1000) & ~((1U << SX_BITRATE) - 1);
  if (bitrate != encoder->bitrate_bits) {
    GST_DEBUG ("HRD bitrate: %u bits/sec", bitrate);
    encoder->bitrate_bits = bitrate;
    encoder->config_changed = TRUE;
  }

  /* Round up CPB size. This is an HRD compliance detail */
  g_assert (SX_CPB_SIZE >= 4);
  cpb_size = gst_util_uint64_scale (bitrate, encoder->cpb_length, 1000) &
      ~((1U << SX_CPB_SIZE) - 1);
  if (cpb_size != encoder->cpb_length_bits) {
    GST_DEBUG ("HRD CPB size: %u bits", cpb_size);
    encoder->cpb_length_bits = cpb_size;
    encoder->config_changed = TRUE;
  }
}

/* Estimates a good enough bitrate if none was supplied */
static void
ensure_bitrate (GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiEncoder *const base_encoder = GST_VAAPI_ENCODER_CAST (encoder);

  /* Default compression: 48 bits per macroblock in "high-compression" mode */
  switch (GST_VAAPI_ENCODER_RATE_CONTROL (encoder)) {
    case GST_VAAPI_RATECONTROL_CBR:
    case GST_VAAPI_RATECONTROL_VBR:
    case GST_VAAPI_RATECONTROL_VBR_CONSTRAINED:
      if (!base_encoder->bitrate) {
        /* According to the literature and testing, CABAC entropy coding
           mode could provide for +10% to +18% improvement in general,
           thus estimating +15% here ; and using adaptive 8x8 transforms
           in I-frames could bring up to +10% improvement. */
        guint bits_per_mb = 48;
        guint64 factor;

        if (!encoder->use_cabac)
          bits_per_mb += (bits_per_mb * 15) / 100;
        if (!encoder->use_dct8x8)
          bits_per_mb += (bits_per_mb * 10) / 100;

        factor = encoder->mb_width * encoder->mb_height * bits_per_mb;
        base_encoder->bitrate =
            gst_util_uint64_scale (factor, GST_VAAPI_ENCODER_FPS_N (encoder),
            GST_VAAPI_ENCODER_FPS_D (encoder)) / 1000;
        GST_INFO ("target bitrate computed to %u kbps", base_encoder->bitrate);
      }
      break;
    default:
      base_encoder->bitrate = 0;
      break;
  }
  ensure_bitrate_hrd (encoder);
}

/* Constructs profile and level information based on user-defined limits */
static GstVaapiEncoderStatus
ensure_profile_and_level (GstVaapiEncoderH264Fei * encoder)
{
  const GstVaapiProfile profile = encoder->profile;
  const GstVaapiLevelH264 level = encoder->level;

  if (!ensure_tuning (encoder))
    GST_WARNING ("Failed to set some of the tuning option as expected! ");

  if (!ensure_profile (encoder) || !ensure_profile_limits (encoder))
    return GST_VAAPI_ENCODER_STATUS_ERROR_UNSUPPORTED_PROFILE;

  /* Check HW constraints */
  if (!ensure_hw_profile_limits (encoder))
    return GST_VAAPI_ENCODER_STATUS_ERROR_UNSUPPORTED_PROFILE;
  if (encoder->profile_idc > encoder->hw_max_profile_idc)
    return GST_VAAPI_ENCODER_STATUS_ERROR_UNSUPPORTED_PROFILE;

  /* Ensure bitrate if not set already and derive the right level to use */
  ensure_bitrate (encoder);
  if (!ensure_level (encoder))
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;

  if (encoder->profile != profile || encoder->level != level) {
    GST_DEBUG ("selected %s profile at level %s",
        gst_vaapi_utils_h264_get_profile_string (encoder->profile),
        gst_vaapi_utils_h264_get_level_string (encoder->level));
    encoder->config_changed = TRUE;
  }
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;
}

static void
reset_properties (GstVaapiEncoderH264Fei * encoder)
{
  GstVaapiEncoder *const base_encoder = GST_VAAPI_ENCODER_CAST (encoder);
  guint mb_size, i;

  if (encoder->idr_period < base_encoder->keyframe_period)
    encoder->idr_period = base_encoder->keyframe_period;
  if (encoder->idr_period > MAX_IDR_PERIOD)
    encoder->idr_period = MAX_IDR_PERIOD;

  if (encoder->min_qp > encoder->init_qp ||
      (GST_VAAPI_ENCODER_RATE_CONTROL (encoder) == GST_VAAPI_RATECONTROL_CQP &&
          encoder->min_qp < encoder->init_qp))
    encoder->min_qp = encoder->init_qp;

  mb_size = encoder->mb_width * encoder->mb_height;
  if (encoder->num_slices > (mb_size + 1) / 2)
    encoder->num_slices = (mb_size + 1) / 2;
  g_assert (encoder->num_slices);

  if (encoder->num_bframes > (base_encoder->keyframe_period + 1) / 2)
    encoder->num_bframes = (base_encoder->keyframe_period + 1) / 2;

  /* Workaround : vaapi-intel-driver doesn't have support for
   * B-frame encode when utilizing low-power encode hardware block.
   * So Disabling b-frame encoding in low-pwer encode.
   *
   * Fixme :We should query the VAConfigAttribEncMaxRefFrames
   * instead of blindly disabling b-frame support and set b/p frame count,
   * buffer pool size etc based on that.*/
  if ((encoder->num_bframes > 0)
      && (encoder->entrypoint == GST_VAAPI_ENTRYPOINT_SLICE_ENCODE_LP)) {
    GST_WARNING
        ("Disabling b-frame since the driver doesn't supporting it in low-power encode");
    encoder->num_bframes = 0;
  }

  if (encoder->num_bframes > 0 && GST_VAAPI_ENCODER_FPS_N (encoder) > 0)
    encoder->cts_offset = gst_util_uint64_scale (GST_SECOND,
        GST_VAAPI_ENCODER_FPS_D (encoder), GST_VAAPI_ENCODER_FPS_N (encoder));
  else
    encoder->cts_offset = 0;

  /* init max_frame_num, max_poc */
  encoder->log2_max_frame_num =
      h264_get_log2_max_frame_num (encoder->idr_period);
  g_assert (encoder->log2_max_frame_num >= 4);
  encoder->max_frame_num = (1 << encoder->log2_max_frame_num);
  encoder->log2_max_pic_order_cnt = encoder->log2_max_frame_num + 1;
  encoder->max_pic_order_cnt = (1 << encoder->log2_max_pic_order_cnt);
  encoder->idr_num = 0;

  for (i = 0; i < encoder->num_views; i++) {
    GstVaapiH264ViewRefPool *const ref_pool = &encoder->ref_pools[i];
    GstVaapiH264ViewReorderPool *const reorder_pool =
        &encoder->reorder_pools[i];

    ref_pool->max_reflist0_count = 1;
    ref_pool->max_reflist1_count = encoder->num_bframes > 0;
    ref_pool->max_ref_frames = ref_pool->max_reflist0_count
        + ref_pool->max_reflist1_count;

    reorder_pool->frame_index = 0;
  }
}

static gboolean
copy_picture_attrib (GstVaapiEncPicture * dst, GstVaapiEncPicture * src)
{
  if (!dst || !src)
    return FALSE;

  dst->proxy = src->proxy;
  dst->surface = src->surface;
  dst->type = src->type;
  dst->surface_id = src->surface_id;
  dst->frame_num = src->frame_num;
  dst->poc = src->poc;

  return TRUE;
}

static GstVaapiEncoderStatus
gst_vaapi_encoder_h264_fei_encode (GstVaapiEncoder * base_encoder,
    GstVaapiEncPicture * picture, GstVaapiCodedBufferProxy * codedbuf)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  GstVaapiEncoder *enc_base_encoder = GST_VAAPI_ENCODER_CAST (encoder->feienc);
  GstVaapiEncoderStatus status = GST_VAAPI_ENCODER_STATUS_ERROR_UNKNOWN;
  GstVaapiSurfaceProxy *reconstruct = NULL;
  GstVaapiEncPicture *picture2 = NULL;
  GstVaapiFeiInfoToPakH264 info_to_pak;

  reconstruct = gst_vaapi_encoder_create_surface (base_encoder);

  g_assert (GST_VAAPI_SURFACE_PROXY_SURFACE (reconstruct));

  if ((encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC_PAK)
      || (encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC)
      || (encoder->fei_mode == GST_VAAPI_FEI_MODE_PAK)) {

    if (!ensure_sequence (encoder, picture))
      goto error;
    if (!ensure_misc_params (encoder, picture))
      goto error;
    if (!encoder->is_fei_disabled
        && !ensure_fei_misc_params (encoder, picture, codedbuf))
      goto error;
    if (!ensure_picture (encoder, picture, codedbuf, reconstruct))
      goto error;
    if (!ensure_slices (encoder, picture))
      goto error;
    if (!gst_vaapi_enc_picture_encode (picture))
      goto error;

    if (!reference_list_update (encoder, picture, reconstruct))
      goto error;

  } else if (encoder->fei_mode ==
      (GST_VAAPI_FEI_MODE_ENC | GST_VAAPI_FEI_MODE_PAK)) {
    /*
     * ref pool is managed by pak.
     * enc will copy from it.
     */
    if (picture->type != GST_VAAPI_PICTURE_TYPE_I
        && !gst_vaapi_feipak_h264_get_ref_pool (encoder->feipak,
            &encoder->ref_pool_ptr)) {
      GST_ERROR ("failed to get pak ref pool");
      status = GST_VAAPI_ENCODER_STATUS_ERROR_UNKNOWN;
      goto error;
    }

    if (picture->type != GST_VAAPI_PICTURE_TYPE_I
        && !gst_vaapi_feienc_h264_set_ref_pool (encoder->feienc,
            encoder->ref_pool_ptr)) {
      GST_ERROR ("failed to set enc ref pool");
      status = GST_VAAPI_ENCODER_STATUS_ERROR_UNKNOWN;
      goto error;
    }

    status =
        gst_vaapi_feienc_h264_encode (enc_base_encoder, picture, reconstruct,
        codedbuf, &info_to_pak);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
      GST_ERROR ("failed to process enc class encode");
      goto error;
    }

    /* duplicate a picture for pak */
    picture2 = GST_VAAPI_ENC_PICTURE_NEW (H264, base_encoder, picture->frame);
    if (!picture2) {
      GST_WARNING ("create H264 picture failed, frame timestamp:%"
          GST_TIME_FORMAT, GST_TIME_ARGS (picture->frame->pts));
      status = GST_VAAPI_ENCODER_STATUS_ERROR_ALLOCATION_FAILED;
      goto error;
    }
    if (!copy_picture_attrib (picture2, picture)) {
      status = GST_VAAPI_ENCODER_STATUS_ERROR_UNKNOWN;
      goto error;
    }
    /* need set picture IDR info for PAK */
    if (GST_VAAPI_ENC_PICTURE_IS_IDR (picture))
      GST_VAAPI_ENC_PICTURE_FLAG_SET (picture2, GST_VAAPI_ENC_PICTURE_FLAG_IDR);

    status =
        gst_vaapi_feipak_h264_encode (encoder->feipak, picture2, codedbuf,
        reconstruct, &info_to_pak);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
      GST_ERROR ("failed to process pak class encode");
      goto error;
    }

    /* Free the slice array */
    if (info_to_pak.h264_slice_headers)
      g_array_free (info_to_pak.h264_slice_headers, TRUE);

    gst_vaapi_enc_picture_unref (picture2);

  }

  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error:
  {
    if (reconstruct)
      gst_vaapi_encoder_release_surface (GST_VAAPI_ENCODER (encoder),
          reconstruct);
    if (picture2)
      gst_vaapi_enc_picture_unref (picture2);
    return status;
  }
}

static GstVaapiEncoderStatus
gst_vaapi_encoder_h264_fei_flush (GstVaapiEncoder * base_encoder)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  GstVaapiEncoder *enc_base_encoder = GST_VAAPI_ENCODER_CAST (encoder->feienc);
  GstVaapiH264ViewReorderPool *reorder_pool;
  GstVaapiEncPicture *pic;
  GstVaapiEncoderStatus status;
  guint i;

  if ((encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC_PAK)
      || (encoder->fei_mode == GST_VAAPI_FEI_MODE_PAK)) {
    for (i = 0; i < encoder->num_views; i++) {
      reorder_pool = &encoder->reorder_pools[i];
      reorder_pool->frame_index = 0;
      reorder_pool->cur_frame_num = 0;
      reorder_pool->cur_present_index = 0;

      while (!g_queue_is_empty (&reorder_pool->reorder_frame_list)) {
        pic = (GstVaapiEncPicture *)
            g_queue_pop_head (&reorder_pool->reorder_frame_list);
        gst_vaapi_enc_picture_unref (pic);
      }
      g_queue_clear (&reorder_pool->reorder_frame_list);
    }
  } else if (encoder->fei_mode ==
      (GST_VAAPI_FEI_MODE_ENC | GST_VAAPI_FEI_MODE_PAK)) {

    status = gst_vaapi_feienc_h264_flush (enc_base_encoder);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
      GST_ERROR ("failed to process enc class flush");
      return status;
    }

    status = gst_vaapi_feipak_h264_flush (encoder->feipak);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
      GST_ERROR ("failed to process pak class flush");
      return status;
    }
  } else {
    g_assert (encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC);
  }

  return GST_VAAPI_ENCODER_STATUS_SUCCESS;
}

/* Generate "codec-data" buffer */
static GstVaapiEncoderStatus
gst_vaapi_encoder_h264_fei_get_codec_data (GstVaapiEncoder * base_encoder,
    GstBuffer ** out_buffer_ptr)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  const guint32 configuration_version = 0x01;
  const guint32 nal_length_size = 4;
  guint8 profile_idc, profile_comp, level_idc;
  GstMapInfo sps_info, pps_info;
  GstBitWriter bs;
  GstBuffer *buffer;
  GstVaapiEncoderStatus status;

  if ((encoder->fei_mode != GST_VAAPI_FEI_MODE_ENC_PAK)
      && (encoder->fei_mode != GST_VAAPI_FEI_MODE_PAK)) {
    status =
        gst_vaapi_feipak_h264_get_codec_data (encoder->feipak, out_buffer_ptr);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
      GST_ERROR ("failed to get pak codec data");
      return status;
    }
    return status;
  }

  if (!encoder->sps_data || !encoder->pps_data)
    return GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_HEADER;
  if (gst_buffer_get_size (encoder->sps_data) < 4)
    return GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_HEADER;

  if (!gst_buffer_map (encoder->sps_data, &sps_info, GST_MAP_READ))
    goto error_map_sps_buffer;

  if (!gst_buffer_map (encoder->pps_data, &pps_info, GST_MAP_READ))
    goto error_map_pps_buffer;

  /* skip sps_data[0], which is the nal_unit_type */
  profile_idc = sps_info.data[1];
  profile_comp = sps_info.data[2];
  level_idc = sps_info.data[3];

  /* Header */
  gst_bit_writer_init (&bs, (sps_info.size + pps_info.size + 64) * 8);
  WRITE_UINT32 (&bs, configuration_version, 8);
  WRITE_UINT32 (&bs, profile_idc, 8);
  WRITE_UINT32 (&bs, profile_comp, 8);
  WRITE_UINT32 (&bs, level_idc, 8);
  WRITE_UINT32 (&bs, 0x3f, 6);  /* 111111 */
  WRITE_UINT32 (&bs, nal_length_size - 1, 2);
  WRITE_UINT32 (&bs, 0x07, 3);  /* 111 */

  /* Write SPS */
  WRITE_UINT32 (&bs, 1, 5);     /* SPS count = 1 */
  g_assert (GST_BIT_WRITER_BIT_SIZE (&bs) % 8 == 0);
  WRITE_UINT32 (&bs, sps_info.size, 16);
  gst_bit_writer_put_bytes (&bs, sps_info.data, sps_info.size);

  /* Write PPS */
  WRITE_UINT32 (&bs, 1, 8);     /* PPS count = 1 */
  WRITE_UINT32 (&bs, pps_info.size, 16);
  gst_bit_writer_put_bytes (&bs, pps_info.data, pps_info.size);

  gst_buffer_unmap (encoder->pps_data, &pps_info);
  gst_buffer_unmap (encoder->sps_data, &sps_info);

  buffer = gst_buffer_new_wrapped (GST_BIT_WRITER_DATA (&bs),
      GST_BIT_WRITER_BIT_SIZE (&bs) / 8);
  if (!buffer)
    goto error_alloc_buffer;
  *out_buffer_ptr = buffer;

  gst_bit_writer_clear (&bs, FALSE);
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
bs_error:
  {
    GST_ERROR ("failed to write codec-data");
    gst_buffer_unmap (encoder->sps_data, &sps_info);
    gst_buffer_unmap (encoder->pps_data, &pps_info);
    gst_bit_writer_clear (&bs, TRUE);
    return FALSE;
  }
error_map_sps_buffer:
  {
    GST_ERROR ("failed to map SPS packed header");
    return GST_VAAPI_ENCODER_STATUS_ERROR_ALLOCATION_FAILED;
  }
error_map_pps_buffer:
  {
    GST_ERROR ("failed to map PPS packed header");
    gst_buffer_unmap (encoder->sps_data, &sps_info);
    return GST_VAAPI_ENCODER_STATUS_ERROR_ALLOCATION_FAILED;
  }
error_alloc_buffer:
  {
    GST_ERROR ("failed to allocate codec-data buffer");
    gst_bit_writer_clear (&bs, TRUE);
    return GST_VAAPI_ENCODER_STATUS_ERROR_ALLOCATION_FAILED;
  }
}

static GstVaapiEncoderStatus
gst_vaapi_encoder_h264_fei_reordering (GstVaapiEncoder * base_encoder,
    GstVideoCodecFrame * frame, GstVaapiEncPicture ** output)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  GstVaapiH264ViewReorderPool *reorder_pool = NULL;
  GstVaapiEncPicture *picture;
  gboolean is_idr = FALSE;

  *output = NULL;

  if ((encoder->fei_mode != GST_VAAPI_FEI_MODE_ENC_PAK)
      && (encoder->fei_mode != GST_VAAPI_FEI_MODE_PAK)) {
    GstVaapiEncoder *enc_base_encoder =
        GST_VAAPI_ENCODER_CAST (encoder->feienc);
    GstVaapiEncoderStatus status;

    status = gst_vaapi_feienc_h264_reordering (enc_base_encoder, frame, output);
    if ((status != GST_VAAPI_ENCODER_STATUS_SUCCESS) &&
        (status != GST_VAAPI_ENCODER_STATUS_NO_SURFACE))
      GST_ERROR ("failed to process enc reordering");

    return status;
  }

  /* encoding views alternatively for MVC */
  if (encoder->is_mvc) {
    /* FIXME: Use first-in-bundle flag on buffers to reset view idx? */
    if (frame)
      encoder->view_idx = frame->system_frame_number % encoder->num_views;
    else
      encoder->view_idx = (encoder->view_idx + 1) % encoder->num_views;
  }
  reorder_pool = &encoder->reorder_pools[encoder->view_idx];

  if (!frame) {
    if (reorder_pool->reorder_state != GST_VAAPI_ENC_H264_REORD_DUMP_FRAMES)
      return GST_VAAPI_ENCODER_STATUS_NO_SURFACE;

    /* reorder_state = GST_VAAPI_ENC_H264_REORD_DUMP_FRAMES
       dump B frames from queue, sometime, there may also have P frame or I frame */
    g_assert (encoder->num_bframes > 0);
    g_return_val_if_fail (!g_queue_is_empty (&reorder_pool->reorder_frame_list),
        GST_VAAPI_ENCODER_STATUS_ERROR_UNKNOWN);
    picture = g_queue_pop_head (&reorder_pool->reorder_frame_list);
    g_assert (picture);
    if (g_queue_is_empty (&reorder_pool->reorder_frame_list)) {
      reorder_pool->reorder_state = GST_VAAPI_ENC_H264_REORD_WAIT_FRAMES;
    }
    goto end;
  }

  /* new frame coming */
  picture = GST_VAAPI_ENC_PICTURE_NEW (H264, encoder, frame);
  if (!picture) {
    GST_WARNING ("create H264 picture failed, frame timestamp:%"
        GST_TIME_FORMAT, GST_TIME_ARGS (frame->pts));
    return GST_VAAPI_ENCODER_STATUS_ERROR_ALLOCATION_FAILED;
  }
  ++reorder_pool->cur_present_index;
  picture->poc = ((reorder_pool->cur_present_index * 2) %
      encoder->max_pic_order_cnt);

  is_idr = (reorder_pool->frame_index == 0 ||
      reorder_pool->frame_index >= encoder->idr_period);

  /* check key frames */
  if (is_idr || GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (frame) ||
      (reorder_pool->frame_index %
          GST_VAAPI_ENCODER_KEYFRAME_PERIOD (encoder)) == 0) {
    ++reorder_pool->cur_frame_num;
    ++reorder_pool->frame_index;

    /* b frame enabled,  check queue of reorder_frame_list */
    if (encoder->num_bframes
        && !g_queue_is_empty (&reorder_pool->reorder_frame_list)) {
      GstVaapiEncPicture *p_pic;

      p_pic = g_queue_pop_tail (&reorder_pool->reorder_frame_list);
      set_p_frame (p_pic, encoder);
      g_queue_foreach (&reorder_pool->reorder_frame_list,
          (GFunc) set_b_frame, encoder);
      ++reorder_pool->cur_frame_num;
      set_key_frame (picture, encoder, is_idr);
      g_queue_push_tail (&reorder_pool->reorder_frame_list, picture);
      picture = p_pic;
      reorder_pool->reorder_state = GST_VAAPI_ENC_H264_REORD_DUMP_FRAMES;
    } else {                    /* no b frames in queue */
      set_key_frame (picture, encoder, is_idr);
      g_assert (g_queue_is_empty (&reorder_pool->reorder_frame_list));
      if (encoder->num_bframes)
        reorder_pool->reorder_state = GST_VAAPI_ENC_H264_REORD_WAIT_FRAMES;
    }
    goto end;
  }

  /* new p/b frames coming */
  ++reorder_pool->frame_index;
  if (reorder_pool->reorder_state == GST_VAAPI_ENC_H264_REORD_WAIT_FRAMES &&
      g_queue_get_length (&reorder_pool->reorder_frame_list) <
      encoder->num_bframes) {
    g_queue_push_tail (&reorder_pool->reorder_frame_list, picture);
    return GST_VAAPI_ENCODER_STATUS_NO_SURFACE;
  }

  ++reorder_pool->cur_frame_num;
  set_p_frame (picture, encoder);

  if (reorder_pool->reorder_state == GST_VAAPI_ENC_H264_REORD_WAIT_FRAMES) {
    g_queue_foreach (&reorder_pool->reorder_frame_list, (GFunc) set_b_frame,
        encoder);
    reorder_pool->reorder_state = GST_VAAPI_ENC_H264_REORD_DUMP_FRAMES;
    g_assert (!g_queue_is_empty (&reorder_pool->reorder_frame_list));
  }

end:
  g_assert (picture);
  frame = picture->frame;
  if (GST_CLOCK_TIME_IS_VALID (frame->pts))
    frame->pts += encoder->cts_offset;
  *output = picture;

  return GST_VAAPI_ENCODER_STATUS_SUCCESS;
}

static GstVaapiEncoderStatus
set_context_info (GstVaapiEncoder * base_encoder)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  GstVideoInfo *const vip = GST_VAAPI_ENCODER_VIDEO_INFO (encoder);
  const guint DEFAULT_SURFACES_COUNT = 3;

  /* Maximum sizes for common headers (in bits) */
  enum
  {
    MAX_SPS_HDR_SIZE = 16473,
    MAX_VUI_PARAMS_SIZE = 210,
    MAX_HRD_PARAMS_SIZE = 4103,
    MAX_PPS_HDR_SIZE = 101,
    MAX_SLICE_HDR_SIZE = 397 + 2572 + 6670 + 2402,
  };

  if (!ensure_hw_profile (encoder))
    return GST_VAAPI_ENCODER_STATUS_ERROR_UNSUPPORTED_PROFILE;

  base_encoder->num_ref_frames =
      ((encoder->num_bframes ? 2 : 1) + DEFAULT_SURFACES_COUNT)
      * encoder->num_views;

  /* Only YUV 4:2:0 formats are supported for now. This means that we
     have a limit of 3200 bits per macroblock. */
  /* XXX: check profile and compute RawMbBits */
  base_encoder->codedbuf_size = (GST_ROUND_UP_16 (vip->width) *
      GST_ROUND_UP_16 (vip->height) / 256) * 400;

  /* Account for SPS header */
  /* XXX: exclude scaling lists, MVC/SVC extensions */
  base_encoder->codedbuf_size += 4 + GST_ROUND_UP_8 (MAX_SPS_HDR_SIZE +
      MAX_VUI_PARAMS_SIZE + 2 * MAX_HRD_PARAMS_SIZE) / 8;

  /* Account for PPS header */
  /* XXX: exclude slice groups, scaling lists, MVC/SVC extensions */
  base_encoder->codedbuf_size += 4 + GST_ROUND_UP_8 (MAX_PPS_HDR_SIZE) / 8;

  /* Account for slice header */
  base_encoder->codedbuf_size += encoder->num_slices * (4 +
      GST_ROUND_UP_8 (MAX_SLICE_HDR_SIZE) / 8);

  base_encoder->context_info.entrypoint = encoder->entrypoint;

  /* Fixme:  Add a method to get VA_FEI_FUNCTION_* from GstVaapiFeiMode */
  base_encoder->context_info.config.encoder.fei_function = encoder->fei_mode;

  return GST_VAAPI_ENCODER_STATUS_SUCCESS;
}

static gboolean
copy_encoder_common_property (GstVaapiEncoder * dst, GstVaapiEncoder * src)
{
  if (!dst || !src)
    return FALSE;

  dst->tune = src->tune;
  dst->rate_control = src->rate_control;
  dst->rate_control_mask = src->rate_control_mask;
  dst->bitrate = src->bitrate;
  dst->keyframe_period = src->keyframe_period;

  return TRUE;
}

static GstVaapiEncoderStatus
gst_vaapi_encoder_h264_fei_reconfigure (GstVaapiEncoder * base_encoder)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  GstVideoInfo *const vip = GST_VAAPI_ENCODER_VIDEO_INFO (encoder);
  GstVaapiEncoder *enc_base_encoder = GST_VAAPI_ENCODER_CAST (encoder->feienc);
  GstVideoInfo *vip_enc = GST_VAAPI_ENCODER_VIDEO_INFO (encoder->feienc);
  GstVaapiEncoderStatus status;
  guint mb_width, mb_height;
  const guint DEFAULT_SURFACES_COUNT = 3;

  if ((encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC_PAK)
      || (encoder->fei_mode == GST_VAAPI_FEI_MODE_PAK)) {
    /* ENC_PAK mode doesn't need to care about ENC and PAK
     * abstrct objects */
    mb_width = (GST_VAAPI_ENCODER_WIDTH (encoder) + 15) / 16;
    mb_height = (GST_VAAPI_ENCODER_HEIGHT (encoder) + 15) / 16;
    if (mb_width != encoder->mb_width || mb_height != encoder->mb_height) {
      GST_DEBUG ("resolution: %dx%d", GST_VAAPI_ENCODER_WIDTH (encoder),
          GST_VAAPI_ENCODER_HEIGHT (encoder));
      encoder->mb_width = mb_width;
      encoder->mb_height = mb_height;
      encoder->config_changed = TRUE;
    }

    /* Take number of MVC views from input caps if provided */
    if (GST_VIDEO_INFO_MULTIVIEW_MODE (vip) ==
        GST_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME
        || GST_VIDEO_INFO_MULTIVIEW_MODE (vip) ==
        GST_VIDEO_MULTIVIEW_MODE_MULTIVIEW_FRAME_BY_FRAME)
      encoder->num_views = GST_VIDEO_INFO_VIEWS (vip);

    encoder->is_mvc = encoder->num_views > 1;

    status = ensure_profile_and_level (encoder);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
      return status;

    reset_properties (encoder);
    status = set_context_info (base_encoder);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
      return status;

  } else {
    /* ENC, PAK and ENC+PAK mode requires two separate objects
     * for ENC and PAK */

    /* Maximum sizes for common headers (in bits) */
    enum
    {
      MAX_SPS_HDR_SIZE = 16473,
      MAX_VUI_PARAMS_SIZE = 210,
      MAX_HRD_PARAMS_SIZE = 4103,
      MAX_PPS_HDR_SIZE = 101,
      MAX_SLICE_HDR_SIZE = 397 + 2572 + 6670 + 2402,
    };

    /* copy encoder-fei common property to feienc */
    if (!copy_encoder_common_property (enc_base_encoder, base_encoder))
      return GST_VAAPI_ENCODER_STATUS_ERROR_UNKNOWN;

    /* copy video info to feienc */
    *vip_enc = *vip;

    status = gst_vaapi_feienc_h264_reconfigure (enc_base_encoder);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
      GST_ERROR ("failed to process enc reconfigure");
      return status;
    }

    if (!gst_vaapi_feienc_h264_get_profile_and_idc (encoder->feienc,
            &encoder->profile, &encoder->profile_idc))
      return GST_VAAPI_ENCODER_STATUS_ERROR_UNKNOWN;

    base_encoder->profile = enc_base_encoder->profile;

    mb_width = (GST_VAAPI_ENCODER_WIDTH (encoder) + 15) / 16;
    mb_height = (GST_VAAPI_ENCODER_HEIGHT (encoder) + 15) / 16;
    if (mb_width != encoder->mb_width || mb_height != encoder->mb_height) {
      GST_DEBUG ("resolution: %dx%d", GST_VAAPI_ENCODER_WIDTH (encoder),
          GST_VAAPI_ENCODER_HEIGHT (encoder));
      encoder->mb_width = mb_width;
      encoder->mb_height = mb_height;
      encoder->config_changed = TRUE;
    }

    status =
        gst_vaapi_feipak_h264_reconfigure (encoder->feipak,
        base_encoder->va_context, encoder->profile, encoder->profile_idc,
        encoder->mb_width, encoder->mb_height, encoder->num_views,
        encoder->num_slices, encoder->num_ref_frames);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
      GST_ERROR ("failed to process pak reconfigure");
      return status;
    }

    base_encoder->num_ref_frames =
        (encoder->num_ref_frames + DEFAULT_SURFACES_COUNT) * encoder->num_views;

    /* Only YUV 4:2:0 formats are supported for now. This means that we
       have a limit of 3200 bits per macroblock. */
    /* XXX: check profile and compute RawMbBits */
    base_encoder->codedbuf_size = (GST_ROUND_UP_16 (vip->width) *
        GST_ROUND_UP_16 (vip->height) / 256) * 400;

    /* Account for SPS header */
    /* XXX: exclude scaling lists, MVC/SVC extensions */
    base_encoder->codedbuf_size += 4 + GST_ROUND_UP_8 (MAX_SPS_HDR_SIZE +
        MAX_VUI_PARAMS_SIZE + 2 * MAX_HRD_PARAMS_SIZE) / 8;

    /* Account for PPS header */
    /* XXX: exclude slice groups, scaling lists, MVC/SVC extensions */
    base_encoder->codedbuf_size += 4 + GST_ROUND_UP_8 (MAX_PPS_HDR_SIZE) / 8;

    /* Account for slice header */
    base_encoder->codedbuf_size += encoder->num_slices * (4 +
        GST_ROUND_UP_8 (MAX_SLICE_HDR_SIZE) / 8);

    base_encoder->context_info.entrypoint = encoder->entrypoint;

    /* ENC+PAK mode use the base encoder context for PAK
     * ENC handled separately */
    if (encoder->fei_mode == (GST_VAAPI_FEI_MODE_ENC | GST_VAAPI_FEI_MODE_PAK))
      base_encoder->context_info.config.encoder.fei_function =
          GST_VAAPI_FEI_MODE_PAK;
  }

  return status;
}

static gboolean
gst_vaapi_encoder_h264_fei_init (GstVaapiEncoder * base_encoder)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  guint32 i;

  /* Default encoding entrypoint */
  encoder->entrypoint = GST_VAAPI_ENTRYPOINT_SLICE_ENCODE;
  encoder->is_fei_disabled = FALSE;
  encoder->is_stats_out_enabled = FALSE;
  encoder->fei_mode = GST_VAAPI_FEI_MODE_ENC_PAK;
  encoder->search_path = GST_VAAPI_FEI_H264_SEARCH_PATH_DEFAULT;
  encoder->len_sp = GST_VAAPI_FEI_H264_SEARCH_PATH_LENGTH_DEFAULT;
  encoder->ref_width = GST_VAAPI_FEI_H264_REF_WIDTH_DEFAULT;
  encoder->ref_height = GST_VAAPI_FEI_H264_REF_HEIGHT_DEFAULT;
  encoder->intra_part_mask = GST_VAAPI_FEI_H264_INTRA_PART_MASK_DEFAULT;
  /* default num ref frames */
  encoder->num_ref_frames = 1;
  /* Multi-view coding information */
  encoder->is_mvc = FALSE;
  encoder->num_views = 1;
  encoder->view_idx = 0;
  memset (encoder->view_ids, 0, sizeof (encoder->view_ids));

  /* re-ordering  list initialize */
  for (i = 0; i < MAX_NUM_VIEWS; i++) {
    GstVaapiH264ViewReorderPool *const reorder_pool =
        &encoder->reorder_pools[i];
    g_queue_init (&reorder_pool->reorder_frame_list);
    reorder_pool->reorder_state = GST_VAAPI_ENC_H264_REORD_NONE;
    reorder_pool->frame_index = 0;
    reorder_pool->cur_frame_num = 0;
    reorder_pool->cur_present_index = 0;
  }

  /* reference list info initialize */
  for (i = 0; i < MAX_NUM_VIEWS; i++) {
    GstVaapiH264ViewRefPool *const ref_pool = &encoder->ref_pools[i];
    g_queue_init (&ref_pool->ref_list);
    ref_pool->max_ref_frames = 0;
    ref_pool->max_reflist0_count = 1;
    ref_pool->max_reflist1_count = 1;
  }

  return TRUE;
}

static void
gst_vaapi_encoder_h264_fei_finalize (GstVaapiEncoder * base_encoder)
{
  /*free private buffers */
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  GstVaapiEncoder *enc_base_encoder = GST_VAAPI_ENCODER_CAST (encoder->feienc);
  GstVaapiMiniObject *object = GST_VAAPI_MINI_OBJECT (encoder->feipak);
  GstVaapiEncPicture *pic;
  GstVaapiEncoderH264FeiRef *ref;
  guint32 i;

  if ((encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC_PAK)
      || (encoder->fei_mode == GST_VAAPI_FEI_MODE_PAK)) {

    gst_buffer_replace (&encoder->sps_data, NULL);
    gst_buffer_replace (&encoder->subset_sps_data, NULL);
    gst_buffer_replace (&encoder->pps_data, NULL);

    /* reference list info de-init */
    for (i = 0; i < MAX_NUM_VIEWS; i++) {
      GstVaapiH264ViewRefPool *const ref_pool = &encoder->ref_pools[i];
      while (!g_queue_is_empty (&ref_pool->ref_list)) {
        ref = (GstVaapiEncoderH264FeiRef *)
            g_queue_pop_head (&ref_pool->ref_list);
        reference_pic_free (encoder, ref);
      }
      g_queue_clear (&ref_pool->ref_list);
    }

    /* re-ordering  list initialize */
    for (i = 0; i < MAX_NUM_VIEWS; i++) {
      GstVaapiH264ViewReorderPool *const reorder_pool =
          &encoder->reorder_pools[i];
      while (!g_queue_is_empty (&reorder_pool->reorder_frame_list)) {
        pic = (GstVaapiEncPicture *)
            g_queue_pop_head (&reorder_pool->reorder_frame_list);
        gst_vaapi_enc_picture_unref (pic);
      }
      g_queue_clear (&reorder_pool->reorder_frame_list);
    }

  } else {
    if (encoder->coded_buf != VA_INVALID_ID) {
      GST_VAAPI_DISPLAY_LOCK (base_encoder->display);
      vaapi_destroy_buffer (base_encoder->va_display, &encoder->coded_buf);
      GST_VAAPI_DISPLAY_UNLOCK (base_encoder->display);
      encoder->coded_buf = VA_INVALID_ID;
    }

    if (enc_base_encoder->va_context != VA_INVALID_ID) {
      GST_VAAPI_DISPLAY_LOCK (base_encoder->display);
      vaDestroyContext (base_encoder->va_display, enc_base_encoder->va_context);
      GST_VAAPI_DISPLAY_UNLOCK (base_encoder->display);
      enc_base_encoder->va_context = VA_INVALID_ID;
    }

    if (encoder->va_config != VA_INVALID_ID) {
      GST_VAAPI_DISPLAY_LOCK (base_encoder->display);
      vaDestroyConfig (base_encoder->va_display, encoder->va_config);
      GST_VAAPI_DISPLAY_UNLOCK (base_encoder->display);
      encoder->va_config = VA_INVALID_ID;
    }

    gst_vaapi_encoder_replace (&enc_base_encoder, NULL);
    gst_vaapi_mini_object_replace (&object, NULL);

    encoder->ref_pool_ptr = NULL;
    encoder->feienc = NULL;
  }
}

static GstVaapiEncoderStatus
gst_vaapi_encoder_h264_fei_set_property (GstVaapiEncoder * base_encoder,
    gint prop_id, const GValue * value)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  GstVaapiEncoder *enc_base_encoder = GST_VAAPI_ENCODER_CAST (encoder->feienc);
  GstVaapiEncoderStatus status;

  switch (prop_id) {
    case GST_VAAPI_ENCODER_H264_FEI_PROP_MAX_BFRAMES:
      encoder->num_bframes = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_FEI_PROP_INIT_QP:
      encoder->init_qp = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_FEI_PROP_MIN_QP:
      encoder->min_qp = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_FEI_PROP_NUM_SLICES:
      encoder->num_slices = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_FEI_PROP_CABAC:
      encoder->use_cabac = g_value_get_boolean (value);
      break;
    case GST_VAAPI_ENCODER_H264_FEI_PROP_DCT8X8:
      encoder->use_dct8x8 = g_value_get_boolean (value);
      break;
    case GST_VAAPI_ENCODER_H264_FEI_PROP_CPB_LENGTH:
      encoder->cpb_length = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_FEI_PROP_NUM_VIEWS:
      encoder->num_views = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_FEI_PROP_VIEW_IDS:{
      guint i;
      GValueArray *view_ids = g_value_get_boxed (value);

      if (view_ids == NULL) {
        for (i = 0; i < encoder->num_views; i++)
          encoder->view_ids[i] = i;
      } else {
        g_assert (view_ids->n_values <= encoder->num_views);

        for (i = 0; i < encoder->num_views; i++) {
          GValue *val = g_value_array_get_nth (view_ids, i);
          encoder->view_ids[i] = g_value_get_uint (val);
        }
      }
      break;
    }
    case GST_VAAPI_ENCODER_H264_PROP_FEI_DISABLE:
      encoder->is_fei_disabled = g_value_get_boolean (value);
      if (!encoder->is_fei_disabled)
        encoder->entrypoint = GST_VAAPI_ENTRYPOINT_SLICE_ENCODE_FEI;
      break;
    case GST_VAAPI_ENCODER_H264_PROP_NUM_MV_PREDICT_L0:
      encoder->num_mv_predictors_l0 = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_NUM_MV_PREDICT_L1:
      encoder->num_mv_predictors_l1 = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_SEARCH_WINDOW:
      encoder->search_window = g_value_get_enum (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_LEN_SP:
      encoder->len_sp = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_SEARCH_PATH:
      encoder->search_path = g_value_get_enum (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_REF_WIDTH:
      encoder->ref_width = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_REF_HEIGHT:
      encoder->ref_height = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_SUBMB_MASK:
      encoder->submb_part_mask = g_value_get_uint (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_SUBPEL_MODE:
      encoder->subpel_mode = g_value_get_enum (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_INTRA_PART_MASK:
      encoder->intra_part_mask = g_value_get_flags (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_INTRA_SAD:
      encoder->intra_sad = g_value_get_enum (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_INTER_SAD:
      encoder->inter_sad = g_value_get_enum (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_ADAPT_SEARCH:
      encoder->adaptive_search = g_value_get_boolean (value) ? 1 : 0;
      break;
    case GST_VAAPI_ENCODER_H264_PROP_MULTI_PRED_L0:
      encoder->multi_predL0 = g_value_get_boolean (value) ? 1 : 0;
      break;
    case GST_VAAPI_ENCODER_H264_PROP_MULTI_PRED_L1:
      encoder->multi_predL1 = g_value_get_boolean (value) ? 1 : 0;
      break;
    case GST_VAAPI_ENCODER_H264_PROP_ENABLE_STATS_OUT:
      encoder->is_stats_out_enabled = g_value_get_boolean (value);
      break;
    case GST_VAAPI_ENCODER_H264_PROP_FEI_MODE:
      encoder->fei_mode = g_value_get_flags (value);
      if (encoder->fei_mode == GST_VAAPI_FEI_MODE_ENC) {
        g_warning ("============= ENC only mode selected ============ \n"
            "We internally run the PAK stage because, the ENC operation requires the reconstructed output of PAK mode. Right now we have no infrastructure to provide reconstructed surfaces to ENC with out running the PAK \n");
        /*Fixme: Support ENC only mode with out running PAK */
        encoder->fei_mode = GST_VAAPI_FEI_MODE_ENC | GST_VAAPI_FEI_MODE_PAK;
      } else if (encoder->fei_mode == GST_VAAPI_FEI_MODE_PAK) {
        g_warning ("============ PAK only mode selected ============ \n"
            "This mode can work as expected, only if there is a custom user specific upstream element which provides mb_code and mv_vectors. If you are running the pipeline only for verification, We recommand to use the fei-mod ENC|PAK which will run the ENC operation and  generate what ever input needed for PAK \n");
      }

      break;

    default:
      return GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
  }

  if ((prop_id != GST_VAAPI_ENCODER_H264_PROP_FEI_MODE) &&
      (prop_id != GST_VAAPI_ENCODER_H264_PROP_FEI_DISABLE) &&
      (prop_id != GST_VAAPI_ENCODER_H264_PROP_ENABLE_STATS_OUT)) {
  /**
   * when new feiencoder, enc_base_encoder is NULL.
   * Only need enc class when set input property.
   */
    if (enc_base_encoder) {
      status =
          gst_vaapi_feienc_h264_set_property (enc_base_encoder, prop_id, value);
      if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
        GST_ERROR ("failed to set enc property");
        return status;
      }
    }

    if (encoder->feipak) {
      status =
          gst_vaapi_feipak_h264_set_property (encoder->feipak, prop_id, value);
      if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS) {
        GST_ERROR ("failed to set pak property");
        return status;
      }
    }
  }
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;
}


static inline gboolean
context_get_attribute (GstVaapiContext * context, VAConfigAttribType type,
    guint * out_value_ptr)
{
  return gst_vaapi_get_config_attribute (GST_VAAPI_OBJECT_DISPLAY (context),
      context->va_profile, context->va_entrypoint, type, out_value_ptr);
}

static gboolean
create_context_for_enc (GstVaapiEncoder * fei_encoder,
    GstVaapiEncoder * enc_encoder)
{
  GstVaapiEncoderH264Fei *const feiencoder =
      GST_VAAPI_ENCODER_H264_FEI (fei_encoder);
  GstVaapiContext *context = fei_encoder->context;
  const GstVaapiContextInfo *const cip = &context->info;
  GstVaapiDisplay *const display = fei_encoder->display;
  const GstVaapiConfigInfoEncoder *const config = &cip->config.encoder;
  guint va_rate_control;
  VAConfigAttrib attribs[5], *attrib = attribs;
  VASurfaceID surface_id;
  VAStatus status;
  GArray *surfaces = NULL;
  gboolean success = FALSE;
  guint i, value, va_chroma_format;

  if (!context->surfaces)
    goto cleanup;

  /* Create VA surfaces list for vaCreateContext() */
  surfaces = g_array_sized_new (FALSE,
      FALSE, sizeof (VASurfaceID), context->surfaces->len);
  if (!surfaces)
    goto cleanup;

  for (i = 0; i < context->surfaces->len; i++) {
    GstVaapiSurface *const surface = g_ptr_array_index (context->surfaces, i);
    if (!surface)
      goto cleanup;
    surface_id = GST_VAAPI_OBJECT_ID (surface);
    g_array_append_val (surfaces, surface_id);
  }
  g_assert (surfaces->len == context->surfaces->len);
  if (!cip->profile || !cip->entrypoint)
    goto cleanup;

  /* Validate VA surface format */
  va_chroma_format = from_GstVaapiChromaType (cip->chroma_type);
  if (!va_chroma_format)
    goto cleanup;
  attrib->type = VAConfigAttribRTFormat;
  if (!context_get_attribute (context, attrib->type, &value))
    goto cleanup;
  if (!(value & va_chroma_format)) {
    GST_ERROR ("unsupported chroma format (%s)",
        string_of_va_chroma_format (va_chroma_format));
    goto cleanup;
  }
  attrib->value = va_chroma_format;
  attrib++;

  /* Rate control */
  va_rate_control = from_GstVaapiRateControl (config->rc_mode);
  if (va_rate_control != VA_RC_NONE) {
    attrib->type = VAConfigAttribRateControl;
    if (!context_get_attribute (context, attrib->type, &value))
      goto cleanup;

    if ((value & va_rate_control) != va_rate_control) {
      GST_ERROR ("unsupported %s rate control",
          string_of_VARateControl (va_rate_control));
      goto cleanup;
    }
    attrib->value = va_rate_control;
    attrib++;
  }

  /* Packed headers */
  if (config->packed_headers) {
    attrib->type = VAConfigAttribEncPackedHeaders;
    attrib->value = VA_ENC_PACKED_HEADER_NONE;
    attrib++;
  }

  if (cip->entrypoint == GST_VAAPI_ENTRYPOINT_SLICE_ENCODE_FEI) {
    attrib->type = (VAConfigAttribType) VAConfigAttribFEIFunctionType;
    attrib->value = VA_FEI_FUNCTION_ENC;
    attrib++;
    attrib->type = (VAConfigAttribType) VAConfigAttribFEIMVPredictors;
    attrib->value = 1;
    attrib++;
  }

  GST_VAAPI_DISPLAY_LOCK (display);
  status = vaCreateConfig (GST_VAAPI_DISPLAY_VADISPLAY (display),
      context->va_profile, context->va_entrypoint, attribs, attrib - attribs,
      &feiencoder->va_config);
  GST_VAAPI_DISPLAY_UNLOCK (display);
  if (!vaapi_check_status (status, "vaCreateConfig()"))
    goto cleanup;

  GST_VAAPI_DISPLAY_LOCK (display);
  status = vaCreateContext (GST_VAAPI_DISPLAY_VADISPLAY (display),
      feiencoder->va_config, GST_ROUND_UP_16 (cip->width),
      GST_ROUND_UP_16 (cip->height), VA_PROGRESSIVE,
      (VASurfaceID *) surfaces->data, surfaces->len, &enc_encoder->va_context);
  GST_VAAPI_DISPLAY_UNLOCK (display);
  if (!vaapi_check_status (status, "vaCreateContext()"))
    goto cleanup;

  success = TRUE;

cleanup:
  if (surfaces)
    g_array_free (surfaces, TRUE);
  return success;
}

/**
 * gst_vaapi_encoder_h264_get_fei_properties:
 *
 * Determines the set of common and H.264 Fei specific encoder properties.
 * The caller owns an extra reference to the resulting array of
 * #GstVaapiEncoderPropInfo elements, so it shall be released with
 * g_ptr_array_unref() after usage.
 *
 * Return value: the set of encoder properties for #GstVaapiEncoderH264,
 *   or %NULL if an error occurred.
 */
static GPtrArray *
gst_vaapi_encoder_h264_get_fei_properties (GPtrArray * props)
{
  /**
    * GstVaapiEncoderH264: disable-fei:
    *
    * Disable FEI mode Encode: disabling fei will results
    * the encoder to use VAEntrypointEncSlice, which means
    * vaapi-intel-driver will be using a different media kerenl.
    * And most of the properties associated with this element
    * will be non functional.
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_FEI_DISABLE,
      g_param_spec_boolean ("disable-fei",
          "Disable FEI Mode Encode",
          "Disable Flexible Encoding Infrasturcture", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


  /**
    * GstVaapiEncoderH264: stats-out:
    *
    * Enable outputting fei buffers MV, MBCode and Distortion.
    * If enabled, encoder will allocate memory for these buffers
    * and submit to the driver even for ENC_PAK mode so that
    * the output data can be extraced for analysis after the
    * complettion of each frame ncode
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_ENABLE_STATS_OUT,
      g_param_spec_boolean ("stats-out",
          "stats out",
          "Enable stats out for fei",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:num_mv_predictors_l0:
    * Indicate how many mv predictors should be used for l0 frames.
    * Only valid if MVPredictor input has been enabled.
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_NUM_MV_PREDICT_L0,
      g_param_spec_uint ("num-mvpredict-l0",
          "Num mv predict l0",
          "Indicate how many predictors should be used for l0,"
          "only valid if MVPredictor input enabled",
          0, 3, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
    * GstVaapiEncoderH264:num_mv_predictors_l1:
    * Indicate how many mv predictors should be used for l1 frames.
    * Only valid if MVPredictor input has been enabled.
    *
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_NUM_MV_PREDICT_L1,
      g_param_spec_uint ("num-mvpredict-l1",
          "Num mv predict l1",
          "Indicate how many predictors should be used for l1,"
          "only valid if MVPredictor input enabled",
          0, 3, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
 /**
    * GstVaapiEncoderH264:search-window:
    * Use predefined Search Window
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_SEARCH_WINDOW,
      g_param_spec_enum ("search-window",
          "search window",
          "Specify one of the predefined search path",
          GST_VAAPI_TYPE_FEI_H264_SEARCH_WINDOW,
          GST_VAAPI_FEI_H264_SEARCH_WINDOW_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:len-sp:
    * Defines the maximum number of Search Units per reference.
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_LEN_SP,
      g_param_spec_uint ("len-sp",
          "length of search path",
          "This value defines number of search units in search path",
          1, 63, 32, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:search-path:
    * SearchPath defines the motion search method.
    * Zero means full search, 1 indicate diamond search.
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_SEARCH_PATH,
      g_param_spec_enum ("search-path",
          "search path",
          "Specify search path",
          GST_VAAPI_TYPE_FEI_H264_SEARCH_PATH,
          GST_VAAPI_FEI_H264_SEARCH_PATH_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:ref-width:
    * Specifies the search region width in pixels.
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_REF_WIDTH,
      g_param_spec_uint ("ref-width",
          "ref width",
          "Width of search region in pixel, must be multiple of 4",
          4, 64, 32, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:ref-height:
    * Specifies the search region height in pixels.
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_REF_HEIGHT,
      g_param_spec_uint ("ref-height",
          "ref height",
          "Height of search region in pixel, must be multiple of 4",
          4, 32, 32, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
    * GstVaapiEncoderH264:submb-mask:
    * Defines the bit-mask for disabling sub-partition
    *
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_SUBMB_MASK,
      g_param_spec_uint ("submb-mask",
          "submb mask",
          "What block and sub-block partitions should be excluded",
          0, 127, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:subpel-mode:
    * defines the half/quarter pel modes
    * 00: integer mode searching
    * 01: half-pel mode searching
    * 11: quarter-pel mode searching
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_SUBPEL_MODE,
      g_param_spec_enum ("subpel-mode",
          "subpel mode",
          "Sub pixel precision for motion estimation",
          GST_VAAPI_TYPE_FEI_H264_SUB_PEL_MODE,
          GST_VAAPI_FEI_H264_SUB_PEL_MODE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
    * GstVaapiEncoderH264:intrapart-mask:
    * Specifies which Luma Intra partition is enabled/disabled
    * for intra mode decision
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_INTRA_PART_MASK,
      g_param_spec_flags ("intrapart-mask",
          "intra part mask",
          "Specifies which Luma Intra partition is enabled/disabled for"
          "intra mode decision",
          GST_VAAPI_TYPE_FEI_H264_INTRA_PART_MASK,
          GST_VAAPI_FEI_H264_INTRA_PART_MASK_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:intra-sad:
    * Specifies distortion measure adjustments used for
    * the motion search SAD comparison.
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_INTRA_SAD,
      g_param_spec_enum ("intra-sad",
          "intra sad",
          "Specifies distortion measure adjustments used"
          "in the motion search SAD comparison for intra MB",
          GST_VAAPI_TYPE_FEI_H264_SAD_MODE, GST_VAAPI_FEI_H264_SAD_MODE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:inter-sad:
    * Specifies distortion measure adjustments used
    * in the motion search SAD comparison for inter MB
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_INTER_SAD,
      g_param_spec_enum ("inter-sad",
          "inter sad",
          "Specifies distortion measure adjustments used"
          "in the motion search SAD comparison for inter MB",
          GST_VAAPI_TYPE_FEI_H264_SAD_MODE, GST_VAAPI_FEI_H264_SAD_MODE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:adaptive-search:
    * Defines whether adaptive searching is enabled for IME
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_ADAPT_SEARCH,
      g_param_spec_boolean ("adaptive-search",
          "adaptive-search",
          "Enable adaptive search",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
    * GstVaapiEncoderH264:multi-predL0:
    * When set to 1, neighbor MV will be used as predictor for list L0,
    * otherwise no neighbor MV will be used as predictor
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_MULTI_PRED_L0,
      g_param_spec_boolean ("multi-predL0",
          "multi predL0",
          "Enable multi prediction for ref L0 list, when set neighbor MV will be used"
          "as predictor, no neighbor MV will be used otherwise",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
    * GstVaapiEncoderH264:multi-predL1:
    * When set to 1, neighbor MV will be used as predictor
    * when set to 0, no neighbor MV will be used as predictor.
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_MULTI_PRED_L1,
      g_param_spec_boolean ("multi-predL1",
          "multi predL1",
          "Enable multi prediction for ref L1 list, when set neighbor MV will be used"
          "as predictor, no neighbor MV will be used otherwise",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

   /**
    * GstVaapiEncoderH264Fei: fei-mode:
    *
    * Cose ENC, PAK, ENC_PAK, or ENC+PAK
    * ENC: Only the Motion Estimation, no transformation or entropy coding
    * PAK: transformation, quantization and entropy coding
    * ENC_PAK: default mode, enc an pak are invoked by driver, middleware has
               control over ENC input only
    * ENC+PAK: enc and pak invoked separately, middleware has control over the ENC input,
               ENC output, and PAK input
    * Encoding mode which can be used for FEI
    */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_PROP_FEI_MODE,
      g_param_spec_flags ("fei-mode",
          "FEI Encoding Mode",
          "Functional mode of FEI Encoding",
          GST_VAAPI_TYPE_FEI_MODE, GST_VAAPI_FEI_MODE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  return props;

}

GST_VAAPI_ENCODER_DEFINE_CLASS_DATA (H264);

static const GstVaapiEncoderClassData fei_encoder_class_data = {
  .codec = GST_VAAPI_CODEC_H264,
  .packed_headers = SUPPORTED_PACKED_HEADERS,
  .rate_control_get_type = gst_vaapi_rate_control_get_type,
  .default_rate_control = DEFAULT_RATECONTROL,
  .rate_control_mask = SUPPORTED_RATECONTROLS,
  .encoder_tune_get_type = gst_vaapi_encoder_tune_get_type,
  .default_encoder_tune = GST_VAAPI_ENCODER_TUNE_NONE,
  .encoder_tune_mask = SUPPORTED_TUNE_OPTIONS,
};

static inline const GstVaapiEncoderClass *
gst_vaapi_encoder_h264_fei_class (void)
{
  static const GstVaapiEncoderClass GstVaapiEncoderH264FeiClass = {
    .parent_class = {
          .size = sizeof (GstVaapiEncoderH264Fei),
          .finalize = (GDestroyNotify) gst_vaapi_encoder_finalize,
        }
    ,
    .class_data = &fei_encoder_class_data,
    .init = gst_vaapi_encoder_h264_fei_init,
    .finalize = gst_vaapi_encoder_h264_fei_finalize,
    .reconfigure = gst_vaapi_encoder_h264_fei_reconfigure,
    .get_default_properties = gst_vaapi_encoder_h264_fei_get_default_properties,
    .reordering = gst_vaapi_encoder_h264_fei_reordering,
    .encode = gst_vaapi_encoder_h264_fei_encode,
    .flush = gst_vaapi_encoder_h264_fei_flush,
    .set_property = gst_vaapi_encoder_h264_fei_set_property,
    .get_codec_data = gst_vaapi_encoder_h264_fei_get_codec_data,
    .ensure_secondary_context =
        gst_vaapi_encoder_h264_fei_ensure_secondary_context,
  };
  return &GstVaapiEncoderH264FeiClass;
}

/**
 * gst_vaapi_encoder_h264_fei_get_default_properties:
 *
 * Determines the set of common and H.264 specific encoder properties.
 * The caller owns an extra reference to the resulting array of
 * #GstVaapiEncoderPropInfo elements, so it shall be released with
 * g_ptr_array_unref() after usage.
 *
 * Return value: the set of encoder properties for #GstVaapiEncoderH264Fei,
 *   or %NULL if an error occurred.
 */
GPtrArray *
gst_vaapi_encoder_h264_fei_get_default_properties (void)
{
  const GstVaapiEncoderClass *const klass = gst_vaapi_encoder_h264_fei_class ();
  GPtrArray *props;

  props = gst_vaapi_encoder_properties_get_default (klass);
  if (!props)
    return NULL;

  /**
   * GstVaapiEncoderH264Fei:max-bframes:
   *
   * The number of B-frames between I and P.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_MAX_BFRAMES,
      g_param_spec_uint ("max-bframes",
          "Max B-Frames", "Number of B-frames between I and P", 0, 10, 1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoderH264Fei:init-qp:
   *
   * The initial quantizer value.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_INIT_QP,
      g_param_spec_uint ("init-qp",
          "Initial QP", "Initial quantizer value", 1, 51, 26,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoderH264Fei:min-qp:
   *
   * The minimum quantizer value.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_MIN_QP,
      g_param_spec_uint ("min-qp",
          "Minimum QP", "Minimum quantizer value", 1, 51, 1,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoderH264Fei:num-slices:
   *
   * The number of slices per frame.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_NUM_SLICES,
      g_param_spec_uint ("num-slices",
          "Number of Slices",
          "Number of slices per frame",
          1, 200, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoderH264Fei:cabac:
   *
   * Enable CABAC entropy coding mode for improved compression ratio,
   * at the expense that the minimum target profile is Main. Default
   * is CAVLC entropy coding mode.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_CABAC,
      g_param_spec_boolean ("cabac",
          "Enable CABAC",
          "Enable CABAC entropy coding mode",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoderH264Fei:dct8x8:
   *
   * Enable adaptive use of 8x8 transforms in I-frames. This improves
   * the compression ratio by the minimum target profile is High.
   * Default is to use 4x4 DCT only.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_DCT8X8,
      g_param_spec_boolean ("dct8x8",
          "Enable 8x8 DCT",
          "Enable adaptive use of 8x8 transforms in I-frames",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoderH264Fei:cpb-length:
   *
   * The size of the CPB buffer in milliseconds.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_CPB_LENGTH,
      g_param_spec_uint ("cpb-length",
          "CPB Length", "Length of the CPB buffer in milliseconds",
          1, 10000, DEFAULT_CPB_LENGTH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoderH264Fei:num-views:
   *
   * The number of views for MVC encoding .
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_NUM_VIEWS,
      g_param_spec_uint ("num-views",
          "Number of Views",
          "Number of Views for MVC encoding",
          1, MAX_NUM_VIEWS, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  /**
   * GstVaapiEncoderH264Fei:view-ids:
   *
   * The view ids for MVC encoding .
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_H264_FEI_PROP_VIEW_IDS,
      g_param_spec_value_array ("view-ids",
          "View IDs", "Set of View Ids used for MVC encoding",
          g_param_spec_uint ("view-id-value", "View id value",
              "view id values used for mvc encoding", 0, MAX_VIEW_ID, 0,
              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS),
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  props = gst_vaapi_encoder_h264_get_fei_properties (props);

  return props;
}

/**
 * gst_vaapi_encoder_h264_fei_set_max_profile:
 * @encoder: a #GstVaapiEncoderH264Fei
 * @profile: an H.264 #GstVaapiProfile
 *
 * Notifies the @encoder to use coding tools from the supplied
 * @profile at most.
 *
 * This means that if the minimal profile derived to
 * support the specified coding tools is greater than this @profile,
 * then an error is returned when the @encoder is configured.
 *
 * Return value: %TRUE on success
 */
gboolean
gst_vaapi_encoder_h264_fei_set_max_profile (GstVaapiEncoderH264Fei *
    encoder, GstVaapiProfile profile)
{
  guint8 profile_idc;

  g_return_val_if_fail (encoder != NULL, FALSE);
  g_return_val_if_fail (profile != GST_VAAPI_PROFILE_UNKNOWN, FALSE);

  if (encoder->fei_mode == (GST_VAAPI_FEI_MODE_ENC | GST_VAAPI_FEI_MODE_PAK)) {
    if (!gst_vaapi_feienc_h264_set_max_profile (encoder->feienc, profile))
      return FALSE;
    return TRUE;
  }

  if (gst_vaapi_profile_get_codec (profile) != GST_VAAPI_CODEC_H264)
    return FALSE;

  profile_idc = gst_vaapi_utils_h264_get_profile_idc (profile);
  if (!profile_idc)
    return FALSE;

  encoder->max_profile_idc = profile_idc;
  return TRUE;
}

/**
 * gst_vaapi_encoder_h264_fei_get_profile_and_level:
 * @encoder: a #GstVaapiEncoderH264Fei
 * @out_profile_ptr: return location for the #GstVaapiProfile
 * @out_level_ptr: return location for the #GstVaapiLevelH264
 *
 * Queries the H.264 @encoder for the active profile and level. That
 * information is only constructed and valid after the encoder is
 * configured, i.e. after the gst_vaapi_encoder_set_codec_state()
 * function is called.
 *
 * Return value: %TRUE on success
 */
gboolean
gst_vaapi_encoder_h264_fei_get_profile_and_level (GstVaapiEncoderH264Fei *
    encoder, GstVaapiProfile * out_profile_ptr,
    GstVaapiLevelH264 * out_level_ptr)
{
  g_return_val_if_fail (encoder != NULL, FALSE);

  if (!encoder->profile || !encoder->level)
    return FALSE;

  if (out_profile_ptr)
    *out_profile_ptr = encoder->profile;
  if (out_level_ptr)
    *out_level_ptr = encoder->level;
  return TRUE;
}

/**
 * gst_vaapi_encoder_h264_is_fei_stats_out_enabled
 * @encoder: a #GstVaapiEncoderH264
 *
 * check if fei output statis is needed
 *
 * Return value: %TRUE if output statistic is needed
 */
gboolean
gst_vaapi_encoder_h264_is_fei_stats_out_enabled (GstVaapiEncoderH264Fei *
    encoder)
{
  return !encoder->is_fei_disabled && encoder->is_stats_out_enabled;
}

/**
 * gst_vaapi_encoder_h264_fei_get_function_mode
 * @encoder: a #GstVaapiEncoderH264Fei
 *
 * return the configured FEI Encoding mode
 *
 * Return value: a #GstVaapiFeiMode
 */
GstVaapiFeiMode
gst_vaapi_encoder_h264_fei_get_function_mode (GstVaapiEncoderH264Fei * encoder)
{
  return encoder->fei_mode;
}

/**
 * gst_vaapi_encoder_h264_fei_set_function_mode
 * @encoder: a #GstVaapiEncoderH264Fei
 *
 * set the configured FEI Encoding mode
 *
 */
void
gst_vaapi_encoder_h264_fei_set_function_mode (GstVaapiEncoderH264Fei * encoder,
    guint fei_mode)
{
  encoder->fei_mode = fei_mode;
}

static gboolean
gst_vaapi_encoder_h264_fei_ensure_secondary_context (GstVaapiEncoder *
    base_encoder)
{
  GstVaapiEncoderH264Fei *const feiencoder =
      GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);
  GstVaapiEncoder *enc_base_encoder =
      GST_VAAPI_ENCODER_CAST (feiencoder->feienc);
  gboolean success;

  if (feiencoder->fei_mode != (GST_VAAPI_FEI_MODE_ENC | GST_VAAPI_FEI_MODE_PAK))
    return TRUE;

  /* Create separate context for ENC */
  if (!create_context_for_enc (base_encoder, enc_base_encoder)) {
    GST_ERROR ("create vacontext for enc failed.\n");
    return FALSE;
  }

  /*
   * create coded-buf for ENC.
   * PAK coded-buf is created by parent encoder.
   */
  success =
      vaapi_create_buffer (enc_base_encoder->va_display,
      enc_base_encoder->va_context, VAEncCodedBufferType,
      base_encoder->codedbuf_size, NULL, &feiencoder->coded_buf, NULL);
  if (!success) {
    g_error ("failed to create coded buf for feienc.\n");
    return FALSE;
  }

  return TRUE;
}

/**
 * gst_vaapi_encoder_h264_fei_new:
 * @display: a #GstVaapiDisplay
 *
 * Creates a new #GstVaapiEncoder for H.264 encoding. Note that the
 * only supported output stream format is "byte-stream" format.
 *
 * Return value: the newly allocated #GstVaapiEncoder object
 */
GstVaapiEncoder *
gst_vaapi_encoder_h264_fei_new (GstVaapiDisplay * display)
{
  GstVaapiEncoder *base_encoder;
  GstVaapiEncoderH264Fei *feiencoder;
  GstVaapiFeiEncH264 *feienc;
  GstVaapiFEIPakH264 *feipak;

  /* create FEIEncoderObject: Default mode of operation in ENC_PAK */
  base_encoder =
      gst_vaapi_encoder_new (gst_vaapi_encoder_h264_fei_class (), display);
  if (!base_encoder)
    return NULL;
  feiencoder = GST_VAAPI_ENCODER_H264_FEI_CAST (base_encoder);

  /* create an enc object */
  feienc = GST_VAAPI_FEI_H264_ENC (gst_vaapi_feienc_h264_new (display));
  if (!feienc)
    return NULL;

  /* create a pak object */
  feipak =
      gst_vaapi_feipak_h264_new (base_encoder, display,
      base_encoder->va_context);
  if (!feipak)
    return NULL;

  feiencoder->feienc = feienc;
  feiencoder->feipak = feipak;

  return base_encoder;
}
