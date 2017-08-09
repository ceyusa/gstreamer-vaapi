/*
 *  gstvaapiencode_h264_fei.c - VA-API H.264 FEI ncoder
 *
 *  Copyright (C) 2016-2019 Intel Corporation
 *    Author: Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 *    Author: Yi A Wang <yi.a.wang@intel.com>
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

/**
 * SECTION:element-vaapih264feienc
 * @short_description: A VA-API FEI based H.264 video encoder
 *
 * Encodes raw video streams into H.264 bitstreams.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 *  gst-launch-1.0 -ev videotestsrc num-buffers=60 !  vaapih264feienc fei-mode=ENC_PAK ! filesink location=test.264
 * ]|
 * </refsect2>
 */

#include "gstcompat.h"
#include <gst/vaapi/gstvaapidisplay.h>
#include <gst/vaapi/gstvaapiencoder_h264_fei.h>
#include <gst/vaapi/gstvaapiutils_h264.h>
#include <gst/vaapi/gstvaapifei_objects.h>
#include <gst/vaapi/gstvaapifeiutils_h264.h>
#include "gstvaapiencode_h264_fei.h"
#include "gstvaapipluginutil.h"
#include "gstvaapivideomemory.h"
#include "gstvaapifeivideometa.h"
#include <gst/vaapi/gstvaapicodedbufferproxy.h>

#define GST_VAAPI_ENCODE_FLOW_MEM_ERROR         GST_FLOW_CUSTOM_ERROR
#define GST_PLUGIN_NAME "vaapih264feienc"
#define GST_PLUGIN_DESC "A VA-API FEI based advanced H264 video encoder"

GST_DEBUG_CATEGORY_STATIC (gst_vaapi_h264_fei_encode_debug);
#define GST_CAT_DEFAULT gst_vaapi_h264_fei_encode_debug

#define GST_CODEC_CAPS                              \
  "video/x-h264, "                                  \
  "stream-format = (string) { avc, byte-stream }, " \
  "alignment = (string) au"

/* *INDENT-OFF* */
static const char gst_vaapiencode_h264_fei_sink_caps_str[] =
  GST_VAAPI_MAKE_SURFACE_CAPS ", "
  GST_CAPS_INTERLACED_FALSE "; "
  GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL) ", "
  GST_CAPS_INTERLACED_FALSE;
/* *INDENT-ON* */

/* *INDENT-OFF* */
static const char gst_vaapiencode_h264_fei_src_caps_str[] =
  GST_CODEC_CAPS ", "
  "profile = (string) { constrained-baseline, baseline, main, high, multiview-high, stereo-high }";
/* *INDENT-ON* */

/* *INDENT-OFF* */
static GstStaticPadTemplate gst_vaapiencode_h264_fei_sink_factory =
  GST_STATIC_PAD_TEMPLATE ("sink",
      GST_PAD_SINK,
      GST_PAD_ALWAYS,
      GST_STATIC_CAPS (gst_vaapiencode_h264_fei_sink_caps_str));
/* *INDENT-ON* */

/* *INDENT-OFF* */
static GstStaticPadTemplate gst_vaapiencode_h264_fei_src_factory =
  GST_STATIC_PAD_TEMPLATE ("src",
      GST_PAD_SRC,
      GST_PAD_ALWAYS,
      GST_STATIC_CAPS (gst_vaapiencode_h264_fei_src_caps_str));
/* *INDENT-ON* */

/* h264 encode */
G_DEFINE_TYPE (GstVaapiEncodeH264Fei, gst_vaapiencode_h264_fei,
    GST_TYPE_VAAPIENCODE);

static void
gst_vaapiencode_h264_fei_init (GstVaapiEncodeH264Fei * encode)
{
  gst_vaapiencode_init_properties (GST_VAAPIENCODE_CAST (encode));
}

static void
gst_vaapiencode_h264_fei_finalize (GObject * object)
{
  G_OBJECT_CLASS (gst_vaapiencode_h264_fei_parent_class)->finalize (object);
}

static void
gst_vaapiencode_h264_fei_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstVaapiEncodeClass *const encode_class = GST_VAAPIENCODE_GET_CLASS (object);
  GstVaapiEncode *const base_encode = GST_VAAPIENCODE_CAST (object);

  switch (prop_id) {
    default:
      if (!encode_class->set_property (base_encode, prop_id, value))
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vaapiencode_h264_fei_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstVaapiEncodeClass *const encode_class = GST_VAAPIENCODE_GET_CLASS (object);
  GstVaapiEncode *const base_encode = GST_VAAPIENCODE_CAST (object);

  switch (prop_id) {
    default:
      if (!encode_class->get_property (base_encode, prop_id, value))
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

typedef struct
{
  GstVaapiProfile best_profile;
  guint best_score;
} FindBestProfileData;

static void
find_best_profile_value (FindBestProfileData * data, const GValue * value)
{
  const gchar *str;
  GstVaapiProfile profile;
  guint score;

  if (!value || !G_VALUE_HOLDS_STRING (value))
    return;

  str = g_value_get_string (value);
  if (!str)
    return;
  profile = gst_vaapi_utils_h264_get_profile_from_string (str);
  if (!profile)
    return;
  score = gst_vaapi_utils_h264_get_profile_score (profile);
  if (score < data->best_score)
    return;
  data->best_profile = profile;
  data->best_score = score;
}

static GstVaapiProfile
find_best_profile (GstCaps * caps)
{
  FindBestProfileData data;
  guint i, j, num_structures, num_values;

  data.best_profile = GST_VAAPI_PROFILE_UNKNOWN;
  data.best_score = 0;

  num_structures = gst_caps_get_size (caps);
  for (i = 0; i < num_structures; i++) {
    GstStructure *const structure = gst_caps_get_structure (caps, i);
    const GValue *const value = gst_structure_get_value (structure, "profile");

    if (!value)
      continue;
    if (G_VALUE_HOLDS_STRING (value))
      find_best_profile_value (&data, value);
    else if (GST_VALUE_HOLDS_LIST (value)) {
      num_values = gst_value_list_get_size (value);
      for (j = 0; j < num_values; j++)
        find_best_profile_value (&data, gst_value_list_get_value (value, j));
    }
  }
  return data.best_profile;
}

static gboolean
gst_vaapiencode_h264_fei_set_config (GstVaapiEncode * base_encode)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI (base_encode->encoder);
  GstCaps *allowed_caps;
  GstVaapiProfile profile;

  /* Check for the largest profile that is supported */
  allowed_caps =
      gst_pad_get_allowed_caps (GST_VAAPI_PLUGIN_BASE_SRC_PAD (base_encode));
  if (!allowed_caps)
    return TRUE;

  profile = find_best_profile (allowed_caps);
  gst_caps_unref (allowed_caps);
  if (profile) {
    GST_INFO ("using %s profile as target decoder constraints",
        gst_vaapi_utils_h264_get_profile_string (profile));
    if (!gst_vaapi_encoder_h264_fei_set_max_profile (encoder, profile))
      return FALSE;
  }
  return TRUE;
}

static GstCaps *
gst_vaapiencode_h264_fei_get_caps (GstVaapiEncode * base_encode)
{
  GstVaapiEncodeH264Fei *const encode =
      GST_VAAPIENCODE_H264_FEI_CAST (base_encode);
  GstCaps *caps, *allowed_caps;

  caps = gst_caps_from_string (GST_CODEC_CAPS);

  /* Check whether "stream-format" is avcC mode */
  allowed_caps =
      gst_pad_get_allowed_caps (GST_VAAPI_PLUGIN_BASE_SRC_PAD (encode));
  if (allowed_caps) {
    const char *stream_format = NULL;
    GstStructure *structure;
    guint i, num_structures;

    num_structures = gst_caps_get_size (allowed_caps);
    for (i = 0; !stream_format && i < num_structures; i++) {
      structure = gst_caps_get_structure (allowed_caps, i);
      if (!gst_structure_has_field_typed (structure, "stream-format",
              G_TYPE_STRING))
        continue;
      stream_format = gst_structure_get_string (structure, "stream-format");
    }
    encode->is_avc = stream_format && strcmp (stream_format, "avc") == 0;
    gst_caps_unref (allowed_caps);
  }
  gst_caps_set_simple (caps, "stream-format", G_TYPE_STRING,
      encode->is_avc ? "avc" : "byte-stream", NULL);

  base_encode->need_codec_data = encode->is_avc;

  /* XXX: update profile and level information */
  return caps;
}

static GstVaapiEncoder *
gst_vaapiencode_h264_fei_alloc_encoder (GstVaapiEncode * base,
    GstVaapiDisplay * display)
{
  return gst_vaapi_encoder_h264_fei_new (display);
}

/* h264 NAL byte stream operations */
static guint8 *
_h264_byte_stream_next_nal (guint8 * buffer, guint32 len, guint32 * nal_size)
{
  const guint8 *cur = buffer;
  const guint8 *const end = buffer + len;
  guint8 *nal_start = NULL;
  guint32 flag = 0xFFFFFFFF;
  guint32 nal_start_len = 0;

  g_assert (len >= 0 && buffer && nal_size);
  if (len < 3) {
    *nal_size = len;
    nal_start = (len ? buffer : NULL);
    return nal_start;
  }

  /*locate head postion */
  if (!buffer[0] && !buffer[1]) {
    if (buffer[2] == 1) {       /* 0x000001 */
      nal_start_len = 3;
    } else if (!buffer[2] && len >= 4 && buffer[3] == 1) {      /* 0x00000001 */
      nal_start_len = 4;
    }
  }
  nal_start = buffer + nal_start_len;
  cur = nal_start;

  /*find next nal start position */
  while (cur < end) {
    flag = ((flag << 8) | ((*cur++) & 0xFF));
    if ((flag & 0x00FFFFFF) == 0x00000001) {
      if (flag == 0x00000001)
        *nal_size = cur - 4 - nal_start;
      else
        *nal_size = cur - 3 - nal_start;
      break;
    }
  }
  if (cur >= end) {
    *nal_size = end - nal_start;
    if (nal_start >= end) {
      nal_start = NULL;
    }
  }
  return nal_start;
}

static inline void
_start_code_to_size (guint8 nal_start_code[4], guint32 nal_size)
{
  nal_start_code[0] = ((nal_size >> 24) & 0xFF);
  nal_start_code[1] = ((nal_size >> 16) & 0xFF);
  nal_start_code[2] = ((nal_size >> 8) & 0xFF);
  nal_start_code[3] = (nal_size & 0xFF);
}

static gboolean
_h264_convert_byte_stream_to_avc (GstBuffer * buf)
{
  GstMapInfo info;
  guint32 nal_size;
  guint8 *nal_start_code, *nal_body;
  guint8 *frame_end;

  g_assert (buf);

  if (!gst_buffer_map (buf, &info, GST_MAP_READ | GST_MAP_WRITE))
    return FALSE;

  nal_start_code = info.data;
  frame_end = info.data + info.size;
  nal_size = 0;

  while ((frame_end > nal_start_code) &&
      (nal_body = _h264_byte_stream_next_nal (nal_start_code,
              frame_end - nal_start_code, &nal_size)) != NULL) {
    if (!nal_size)
      goto error;

    g_assert (nal_body - nal_start_code == 4);
    _start_code_to_size (nal_start_code, nal_size);
    nal_start_code = nal_body + nal_size;
  }
  gst_buffer_unmap (buf, &info);
  return TRUE;

  /* ERRORS */
error:
  {
    gst_buffer_unmap (buf, &info);
    return FALSE;
  }
}

static GstFlowReturn
alloc_buffer (GstVaapiEncode * encode,
    GstVaapiCodedBuffer * coded_buf, GstBuffer ** outbuf_ptr)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI (encode->encoder);
  GstBuffer *buf;
  gint32 buf_size;
  GstVaapiFeiMode fei_mode;

  g_return_val_if_fail (coded_buf != NULL, GST_FLOW_ERROR);
  g_return_val_if_fail (outbuf_ptr != NULL, GST_FLOW_ERROR);

  fei_mode = gst_vaapi_encoder_h264_fei_get_function_mode (encoder);

  if (fei_mode == GST_VAAPI_FEI_MODE_ENC)
    buf_size = 4;               /* just avoid zero size buffer allocation */
  else
    buf_size = gst_vaapi_coded_buffer_get_size (coded_buf);

  if (buf_size <= 0)
    goto error_invalid_buffer;

  buf =
      gst_video_encoder_allocate_output_buffer (GST_VIDEO_ENCODER_CAST (encode),
      buf_size);
  if (!buf)
    goto error_create_buffer;

  /* There is no encoded output content in ENC only mode */
  if (fei_mode != GST_VAAPI_FEI_MODE_ENC) {
    if (!gst_vaapi_coded_buffer_copy_into (buf, coded_buf))
      goto error_copy_buffer;
  }

  *outbuf_ptr = buf;
  return GST_FLOW_OK;

  /* ERRORS */
error_invalid_buffer:
  {
    GST_ERROR ("invalid GstVaapiCodedBuffer size (%d bytes)", buf_size);
    return GST_VAAPI_ENCODE_FLOW_MEM_ERROR;
  }
error_create_buffer:
  {
    GST_ERROR ("failed to create output buffer of size %d", buf_size);
    return GST_VAAPI_ENCODE_FLOW_MEM_ERROR;
  }
error_copy_buffer:
  {
    GST_ERROR ("failed to copy GstVaapiCodedBuffer data");
    gst_buffer_unref (buf);
    return GST_VAAPI_ENCODE_FLOW_MEM_ERROR;
  }
}

static GstFlowReturn
gst_vaapiencode_h264_fei_alloc_buffer (GstVaapiEncode * base_encode,
    GstVaapiCodedBuffer * coded_buf, GstBuffer ** out_buffer_ptr)
{
  GstVaapiEncodeH264Fei *const encode =
      GST_VAAPIENCODE_H264_FEI_CAST (base_encode);
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI (base_encode->encoder);
  GstFlowReturn ret;

  g_return_val_if_fail (encoder != NULL, GST_FLOW_ERROR);

  ret = alloc_buffer (base_encode, coded_buf, out_buffer_ptr);
  if (ret != GST_FLOW_OK)
    return ret;

  if (!encode->is_avc)
    return GST_FLOW_OK;

  /* Convert to avcC format */
  if (!_h264_convert_byte_stream_to_avc (*out_buffer_ptr))
    goto error_convert_buffer;
  return GST_FLOW_OK;

  /* ERRORS */
error_convert_buffer:
  {
    GST_ERROR ("failed to convert from bytestream format to avcC format");
    gst_buffer_replace (out_buffer_ptr, NULL);
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_vaapiencode_h264_load_control_data (GstVaapiEncode * base_encode,
    GstVaapiFeiVideoMeta * feimeta, GstVaapiSurfaceProxy * proxy)
{
  if (feimeta != NULL) {
    gst_vaapi_surface_proxy_set_fei_mb_code (proxy, feimeta->mbcode);
    gst_vaapi_surface_proxy_set_fei_mv (proxy, feimeta->mv);
    gst_vaapi_surface_proxy_set_fei_mv_predictor (proxy, feimeta->mvpred);
    gst_vaapi_surface_proxy_set_fei_mb_control (proxy, feimeta->mbcntrl);
    gst_vaapi_surface_proxy_set_fei_qp (proxy, feimeta->qp);
    gst_vaapi_surface_proxy_set_fei_distortion (proxy, feimeta->dist);
  }
  return TRUE;

}

static GstVaapiFeiVideoMeta *
gst_vaapiencode_h264_save_stats_to_meta (GstVaapiEncode * base_encode,
    GstVaapiCodedBufferProxy * proxy)
{
  GstVaapiEncoderH264Fei *const encoder =
      GST_VAAPI_ENCODER_H264_FEI (base_encode->encoder);
  GstVaapiFeiVideoMeta *feimeta = NULL;
  GstVaapiEncFeiMbCode *mbcode = NULL;
  GstVaapiEncFeiMv *mv = NULL;
  GstVaapiEncFeiDistortion *dist = NULL;

  if (!gst_vaapi_encoder_h264_is_fei_stats_out_enabled (encoder))
    return NULL;

  feimeta = gst_vaapi_fei_video_meta_new ();
  if (feimeta == NULL)
    return NULL;

  mbcode = gst_vaapi_coded_buffer_proxy_get_fei_mbcode (proxy);
  if (mbcode)
    feimeta->mbcode = (GstVaapiEncFeiMbCode *)
        gst_vaapi_fei_codec_object_ref ((GstVaapiFeiCodecObject *) mbcode);

  mv = gst_vaapi_coded_buffer_proxy_get_fei_mv (proxy);
  if (mv)
    feimeta->mv = (GstVaapiEncFeiMv *)
        gst_vaapi_fei_codec_object_ref ((GstVaapiFeiCodecObject *) mv);

  dist = gst_vaapi_coded_buffer_proxy_get_fei_distortion (proxy);
  if (dist)
    feimeta->dist = (GstVaapiEncFeiDistortion *)
        gst_vaapi_fei_codec_object_ref ((GstVaapiFeiCodecObject *) dist);

  return feimeta;
}

static void
gst_vaapiencode_h264_fei_class_init (GstVaapiEncodeH264FeiClass * klass)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  GstElementClass *const element_class = GST_ELEMENT_CLASS (klass);
  GstVaapiEncodeClass *const encode_class = GST_VAAPIENCODE_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_vaapi_h264_fei_encode_debug,
      GST_PLUGIN_NAME, 0, GST_PLUGIN_DESC);

  object_class->finalize = gst_vaapiencode_h264_fei_finalize;
  object_class->set_property = gst_vaapiencode_h264_fei_set_property;
  object_class->get_property = gst_vaapiencode_h264_fei_get_property;

  encode_class->get_properties =
      gst_vaapi_encoder_h264_fei_get_default_properties;
  encode_class->set_config = gst_vaapiencode_h264_fei_set_config;
  encode_class->get_caps = gst_vaapiencode_h264_fei_get_caps;
  encode_class->alloc_encoder = gst_vaapiencode_h264_fei_alloc_encoder;
  encode_class->alloc_buffer = gst_vaapiencode_h264_fei_alloc_buffer;

  encode_class->load_control_data = gst_vaapiencode_h264_load_control_data;
  encode_class->save_stats_to_meta = gst_vaapiencode_h264_save_stats_to_meta;

  gst_element_class_set_static_metadata (element_class,
      "VA-API H264 FEI Advanced encoder",
      "Codec/Encoder/Video",
      GST_PLUGIN_DESC,
      "Sreerenj Balachandran <sreerenj.balachandran@intel.com> ,"
      "Yi A Wang <yi.a.wang@intel.com>");

  /* sink pad */
  gst_element_class_add_static_pad_template (element_class,
      &gst_vaapiencode_h264_fei_sink_factory);

  /* src pad */
  gst_element_class_add_static_pad_template (element_class,
      &gst_vaapiencode_h264_fei_src_factory);

  gst_vaapiencode_class_init_properties (encode_class);
}
