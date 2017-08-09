/*
 *  gstvaapiencode.c - VA-API video encoder
 *
 *  Copyright (C) 2013-2014 Intel Corporation
 *    Author: Wind Yuan <feng.yuan@intel.com>
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
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

#include "gstcompat.h"
#include <gst/vaapi/gstvaapivalue.h>
#include <gst/vaapi/gstvaapidisplay.h>
#include "gstvaapiencode.h"
#include "gstvaapipluginutil.h"
#include "gstvaapivideometa.h"
#include "gstvaapivideomemory.h"
#include "gstvaapivideobufferpool.h"

#define GST_PLUGIN_NAME "vaapiencode"
#define GST_PLUGIN_DESC "A VA-API based video encoder"

#define GST_VAAPI_ENCODE_FLOW_TIMEOUT           GST_FLOW_CUSTOM_SUCCESS
#define GST_VAAPI_ENCODE_FLOW_MEM_ERROR         GST_FLOW_CUSTOM_ERROR
#define GST_VAAPI_ENCODE_FLOW_CONVERT_ERROR     GST_FLOW_CUSTOM_ERROR_1

GST_DEBUG_CATEGORY_STATIC (gst_vaapiencode_debug);
#ifndef GST_DISABLE_GST_DEBUG
#define GST_CAT_DEFAULT gst_vaapiencode_debug
#else
#define GST_CAT_DEFAULT NULL
#endif

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstVaapiEncode,
    gst_vaapiencode, GST_TYPE_VIDEO_ENCODER,
    GST_VAAPI_PLUGIN_BASE_INIT_INTERFACES);

GST_VAAPI_PLUGIN_BASE_DEFINE_SET_CONTEXT (gst_vaapiencode_parent_class);

enum
{
  PROP_0,

  PROP_BASE,
};

static inline gboolean
ensure_display (GstVaapiEncode * encode)
{
  return gst_vaapi_plugin_base_ensure_display (GST_VAAPI_PLUGIN_BASE (encode));
}

static gboolean
gst_vaapiencode_sink_query (GstVideoEncoder * encoder, GstQuery * query)
{
  gboolean ret = TRUE;
  GstElement *const element = GST_ELEMENT (encoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      ret = gst_vaapi_handle_context_query (element, query);
      break;
    default:
      ret = GST_VIDEO_ENCODER_CLASS (gst_vaapiencode_parent_class)->sink_query
          (encoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_vaapiencode_src_query (GstVideoEncoder * encoder, GstQuery * query)
{
  gboolean ret = TRUE;
  GstElement *const element = GST_ELEMENT (encoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      ret = gst_vaapi_handle_context_query (element, query);
      break;
    default:
      ret = GST_VIDEO_ENCODER_CLASS (gst_vaapiencode_parent_class)->src_query
          (encoder, query);
      break;
  }

  return ret;
}

typedef struct
{
  GstVaapiEncoderProp id;
  GParamSpec *pspec;
  GValue value;
} PropValue;

static PropValue *
prop_value_new (GstVaapiEncoderPropInfo * prop)
{
  static const GValue default_value = G_VALUE_INIT;
  PropValue *prop_value;

  if (!prop || !prop->pspec)
    return NULL;

  prop_value = g_slice_new (PropValue);
  if (!prop_value)
    return NULL;

  prop_value->id = prop->prop;
  prop_value->pspec = g_param_spec_ref (prop->pspec);

  memcpy (&prop_value->value, &default_value, sizeof (prop_value->value));
  g_value_init (&prop_value->value, prop->pspec->value_type);
  g_param_value_set_default (prop->pspec, &prop_value->value);
  return prop_value;
}

static void
prop_value_free (PropValue * prop_value)
{
  if (!prop_value)
    return;

  if (G_VALUE_TYPE (&prop_value->value))
    g_value_unset (&prop_value->value);

  if (prop_value->pspec) {
    g_param_spec_unref (prop_value->pspec);
    prop_value->pspec = NULL;
  }
  g_slice_free (PropValue, prop_value);
}

static inline PropValue *
prop_value_lookup (GstVaapiEncode * encode, guint prop_id)
{
  GPtrArray *const prop_values = encode->prop_values;

  if (prop_values &&
      (prop_id >= PROP_BASE && prop_id < PROP_BASE + prop_values->len))
    return g_ptr_array_index (prop_values, prop_id - PROP_BASE);
  return NULL;
}

static gboolean
gst_vaapiencode_default_get_property (GstVaapiEncode * encode, guint prop_id,
    GValue * value)
{
  PropValue *const prop_value = prop_value_lookup (encode, prop_id);

  if (prop_value) {
    g_value_copy (&prop_value->value, value);
    return TRUE;
  }
  return FALSE;
}

static gboolean
gst_vaapiencode_default_set_property (GstVaapiEncode * encode, guint prop_id,
    const GValue * value)
{
  PropValue *const prop_value = prop_value_lookup (encode, prop_id);

  if (prop_value) {
    g_value_copy (value, &prop_value->value);
    return TRUE;
  }
  return FALSE;
}

static GstFlowReturn
gst_vaapiencode_default_alloc_buffer (GstVaapiEncode * encode,
    GstVaapiCodedBuffer * coded_buf, GstBuffer ** outbuf_ptr)
{
  GstBuffer *buf;
  gint32 buf_size;

  g_return_val_if_fail (coded_buf != NULL, GST_FLOW_ERROR);
  g_return_val_if_fail (outbuf_ptr != NULL, GST_FLOW_ERROR);

  buf_size = gst_vaapi_coded_buffer_get_size (coded_buf);
  if (buf_size <= 0)
    goto error_invalid_buffer;

  buf =
      gst_video_encoder_allocate_output_buffer (GST_VIDEO_ENCODER_CAST (encode),
      buf_size);
  if (!buf)
    goto error_create_buffer;
  if (!gst_vaapi_coded_buffer_copy_into (buf, coded_buf))
    goto error_copy_buffer;

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

static gboolean
ensure_output_state (GstVaapiEncode * encode)
{
  GstVideoEncoder *const venc = GST_VIDEO_ENCODER_CAST (encode);
  GstVaapiEncodeClass *const klass = GST_VAAPIENCODE_GET_CLASS (encode);
  GstVaapiEncoderStatus status;
  GstCaps *out_caps;

  if (!encode->input_state_changed)
    return TRUE;

  out_caps = klass->get_caps (encode);
  if (!out_caps)
    return FALSE;

  if (encode->output_state)
    gst_video_codec_state_unref (encode->output_state);
  encode->output_state = gst_video_encoder_set_output_state (venc, out_caps,
      encode->input_state);

  if (encode->need_codec_data) {
    status = gst_vaapi_encoder_get_codec_data (encode->encoder,
        &encode->output_state->codec_data);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
      return FALSE;
  }

  if (!gst_video_encoder_negotiate (venc))
    return FALSE;

  encode->input_state_changed = FALSE;
  return TRUE;
}

static GstFlowReturn
gst_vaapiencode_push_frame (GstVaapiEncode * encode, gint64 timeout)
{
  GstVideoEncoder *const venc = GST_VIDEO_ENCODER_CAST (encode);
  GstVaapiEncodeClass *const klass = GST_VAAPIENCODE_GET_CLASS (encode);
  GstVideoCodecFrame *out_frame;
  GstVaapiCodedBufferProxy *codedbuf_proxy = NULL;
  GstVaapiEncoderStatus status;
  GstBuffer *out_buffer;
  GstFlowReturn ret;
#if USE_H264_FEI_ENCODER
  GstVaapiFeiVideoMeta *feimeta = NULL;
#endif

  status = gst_vaapi_encoder_get_buffer_with_timeout (encode->encoder,
      &codedbuf_proxy, timeout);
  if (status == GST_VAAPI_ENCODER_STATUS_NO_BUFFER)
    return GST_VAAPI_ENCODE_FLOW_TIMEOUT;
  if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
    goto error_get_buffer;

  out_frame = gst_vaapi_coded_buffer_proxy_get_user_data (codedbuf_proxy);
  if (!out_frame)
    goto error_get_buffer;
  gst_video_codec_frame_ref (out_frame);
  gst_video_codec_frame_set_user_data (out_frame, NULL, NULL);

  /* Update output state */
  GST_VIDEO_ENCODER_STREAM_LOCK (encode);
  if (!ensure_output_state (encode))
    goto error_output_state;
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encode);

  /* Allocate and copy buffer into system memory */
  out_buffer = NULL;
  ret = klass->alloc_buffer (encode,
      GST_VAAPI_CODED_BUFFER_PROXY_BUFFER (codedbuf_proxy), &out_buffer);

#if USE_H264_FEI_ENCODER
  if (klass->save_stats_to_meta) {
    feimeta = klass->save_stats_to_meta (encode, codedbuf_proxy);
    if (feimeta != NULL)
      gst_buffer_set_vaapi_fei_video_meta (out_buffer, feimeta);
  }
#endif

  gst_vaapi_coded_buffer_proxy_replace (&codedbuf_proxy, NULL);
  if (ret != GST_FLOW_OK)
    goto error_allocate_buffer;

  gst_buffer_replace (&out_frame->output_buffer, out_buffer);
  gst_buffer_unref (out_buffer);

  GST_TRACE_OBJECT (encode, "output:%" GST_TIME_FORMAT ", size:%zu",
      GST_TIME_ARGS (out_frame->pts), gst_buffer_get_size (out_buffer));

  return gst_video_encoder_finish_frame (venc, out_frame);

  /* ERRORS */
error_get_buffer:
  {
    GST_ERROR ("failed to get encoded buffer (status %d)", status);
    if (codedbuf_proxy)
      gst_vaapi_coded_buffer_proxy_unref (codedbuf_proxy);
    return GST_FLOW_ERROR;
  }
error_allocate_buffer:
  {
    GST_ERROR ("failed to allocate encoded buffer in system memory");
    if (out_buffer)
      gst_buffer_unref (out_buffer);
    gst_video_codec_frame_unref (out_frame);
    return ret;
  }
error_output_state:
  {
    GST_ERROR ("failed to negotiate output state (status %d)", status);
    GST_VIDEO_ENCODER_STREAM_UNLOCK (encode);
    gst_video_codec_frame_unref (out_frame);
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

static void
gst_vaapiencode_buffer_loop (GstVaapiEncode * encode)
{
  GstFlowReturn ret;
  const gint64 timeout = 50000; /* microseconds */

  ret = gst_vaapiencode_push_frame (encode, timeout);
  if (ret == GST_FLOW_OK || ret == GST_VAAPI_ENCODE_FLOW_TIMEOUT)
    return;

  GST_LOG_OBJECT (encode, "pausing task, reason %s", gst_flow_get_name (ret));
  gst_pad_pause_task (GST_VAAPI_PLUGIN_BASE_SRC_PAD (encode));
}

static gboolean
ensure_allowed_sinkpad_caps (GstVaapiEncode * encode)
{
  GstVaapiEncodeClass *klass = GST_VAAPIENCODE_GET_CLASS (encode);
  GstCaps *out_caps, *raw_caps = NULL;
  GArray *formats = NULL;
  gboolean ret = FALSE;
  GstVaapiProfile profile = GST_VAAPI_PROFILE_UNKNOWN;

  if (encode->allowed_sinkpad_caps)
    return TRUE;
  if (!encode->encoder)
    return TRUE;

  out_caps = gst_caps_from_string (GST_VAAPI_MAKE_SURFACE_CAPS);
  if (!out_caps)
    goto failed_create_va_caps;

  if (klass->get_profile) {
    GstCaps *allowed =
        gst_pad_get_allowed_caps (GST_VAAPI_PLUGIN_BASE_SRC_PAD (encode));
    if (allowed) {
      if (!gst_caps_is_empty (allowed) && !gst_caps_is_any (allowed))
        profile = klass->get_profile (allowed);
      gst_caps_unref (allowed);
    }
  }

  formats = gst_vaapi_encoder_get_surface_formats (encode->encoder, profile);
  if (!formats)
    goto failed_get_formats;

  raw_caps = gst_vaapi_video_format_new_template_caps_from_list (formats);
  if (!raw_caps)
    goto failed_create_raw_caps;

  out_caps = gst_caps_make_writable (out_caps);
  gst_caps_append (out_caps, gst_caps_copy (raw_caps));
  gst_caps_replace (&encode->allowed_sinkpad_caps, out_caps);
  GST_INFO_OBJECT (encode, "Allowed sink caps %" GST_PTR_FORMAT,
      encode->allowed_sinkpad_caps);

  ret = TRUE;

bail:
  if (out_caps)
    gst_caps_unref (out_caps);
  if (raw_caps)
    gst_caps_unref (raw_caps);
  if (formats)
    g_array_unref (formats);
  return ret;

failed_create_va_caps:
  {
    GST_WARNING_OBJECT (encode, "failed to create VA/GL sink caps");
    return FALSE;
  }
failed_get_formats:
  {
    GST_WARNING_OBJECT (encode, "failed to get allowed surface formats");
    goto bail;
  }
failed_create_raw_caps:
  {
    GST_WARNING_OBJECT (encode, "failed to create raw sink caps");
    goto bail;
  }
}

static GstCaps *
gst_vaapiencode_get_caps (GstVideoEncoder * venc, GstCaps * filter)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (venc);
  GstCaps *result;

  ensure_allowed_sinkpad_caps (encode);
  result = gst_video_encoder_proxy_getcaps (venc, encode->allowed_sinkpad_caps,
      filter);

  GST_DEBUG_OBJECT (venc, "Negotiated sink caps %" GST_PTR_FORMAT, result);
  return result;
}

static gboolean
gst_vaapiencode_destroy (GstVaapiEncode * encode)
{
  if (encode->input_state) {
    gst_video_codec_state_unref (encode->input_state);
    encode->input_state = NULL;
  }

  if (encode->output_state) {
    gst_video_codec_state_unref (encode->output_state);
    encode->output_state = NULL;
  }

  gst_caps_replace (&encode->allowed_sinkpad_caps, NULL);
  gst_vaapi_encoder_replace (&encode->encoder, NULL);
  return TRUE;
}

static void
gst_vaapiencode_purge (GstVaapiEncode * encode)
{
  GstVaapiCodedBufferProxy *codedbuf_proxy = NULL;
  GstVaapiEncoderStatus status;
  GstVideoCodecFrame *out_frame;

  do {
    status = gst_vaapi_encoder_get_buffer_with_timeout (encode->encoder,
        &codedbuf_proxy, 0);
    if (status == GST_VAAPI_ENCODER_STATUS_SUCCESS) {
      out_frame = gst_vaapi_coded_buffer_proxy_get_user_data (codedbuf_proxy);
      if (out_frame)
        gst_video_codec_frame_set_user_data (out_frame, NULL, NULL);

      gst_vaapi_coded_buffer_proxy_unref (codedbuf_proxy);
    }
  } while (status == GST_VAAPI_ENCODER_STATUS_SUCCESS);
}

static gboolean
ensure_encoder (GstVaapiEncode * encode)
{
  GstVaapiEncodeClass *klass = GST_VAAPIENCODE_GET_CLASS (encode);
  GstVaapiEncoderStatus status;
  GPtrArray *const prop_values = encode->prop_values;
  guint i;

  g_return_val_if_fail (klass->alloc_encoder, FALSE);

  if (encode->encoder)
    return FALSE;

  encode->encoder = klass->alloc_encoder (encode,
      GST_VAAPI_PLUGIN_BASE_DISPLAY (encode));
  if (!encode->encoder)
    return FALSE;

  if (prop_values) {
    for (i = 0; i < prop_values->len; i++) {
      PropValue *const prop_value = g_ptr_array_index (prop_values, i);
      status = gst_vaapi_encoder_set_property (encode->encoder, prop_value->id,
          &prop_value->value);
      if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
        return FALSE;
    }
  }
  return TRUE;
}

static gboolean
gst_vaapiencode_open (GstVideoEncoder * venc)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (venc);
  GstVaapiDisplay *const old_display = GST_VAAPI_PLUGIN_BASE_DISPLAY (encode);
  gboolean success;

  if (!gst_vaapi_plugin_base_open (GST_VAAPI_PLUGIN_BASE (encode)))
    return FALSE;

  GST_VAAPI_PLUGIN_BASE_DISPLAY (encode) = NULL;
  success = ensure_display (encode);
  if (old_display)
    gst_vaapi_display_unref (old_display);
  return success;
}

static gboolean
gst_vaapiencode_start (GstVideoEncoder * venc)
{
  return ensure_encoder (GST_VAAPIENCODE_CAST (venc));
}

static gboolean
gst_vaapiencode_stop (GstVideoEncoder * venc)
{
  return gst_vaapiencode_destroy (GST_VAAPIENCODE_CAST (venc));
}

static gboolean
gst_vaapiencode_close (GstVideoEncoder * venc)
{
  gst_vaapi_plugin_base_close (GST_VAAPI_PLUGIN_BASE (venc));
  return TRUE;
}

static gboolean
set_codec_state (GstVaapiEncode * encode, GstVideoCodecState * state)
{
  GstVaapiEncodeClass *const klass = GST_VAAPIENCODE_GET_CLASS (encode);
  GstVaapiEncoderStatus status;

  g_return_val_if_fail (encode->encoder, FALSE);

  /* Initialize codec specific parameters */
  if (klass->set_config && !klass->set_config (encode))
    return FALSE;

  status = gst_vaapi_encoder_set_codec_state (encode->encoder, state);
  if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
    return FALSE;
  return TRUE;
}

static gboolean
gst_vaapiencode_set_format (GstVideoEncoder * venc, GstVideoCodecState * state)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (venc);
  gboolean ret;

  g_return_val_if_fail (state->caps != NULL, FALSE);

  if (!set_codec_state (encode, state))
    return FALSE;

  if (!gst_vaapi_plugin_base_set_caps (GST_VAAPI_PLUGIN_BASE (encode),
          state->caps, NULL))
    return FALSE;

  if (encode->input_state)
    gst_video_codec_state_unref (encode->input_state);
  encode->input_state = gst_video_codec_state_ref (state);
  encode->input_state_changed = TRUE;

  ret = gst_pad_start_task (GST_VAAPI_PLUGIN_BASE_SRC_PAD (encode),
      (GstTaskFunction) gst_vaapiencode_buffer_loop, encode, NULL);

  if (!ret)
    return FALSE;

  /* Store some tags */
  {
    GstTagList *tags = gst_tag_list_new_empty ();
    const gchar *encoder, *codec;
    guint bitrate = 0;

    g_object_get (encode, "bitrate", &bitrate, NULL);
    gst_tag_list_add (tags, GST_TAG_MERGE_REPLACE, GST_TAG_NOMINAL_BITRATE,
        bitrate, NULL);

    if ((encoder =
            gst_element_class_get_metadata (GST_ELEMENT_GET_CLASS (encode),
                GST_ELEMENT_METADATA_LONGNAME)))
      gst_tag_list_add (tags, GST_TAG_MERGE_REPLACE, GST_TAG_ENCODER, encoder,
          NULL);

    if ((codec =
            gst_vaapi_codec_get_name (gst_vaapi_profile_get_codec
                (gst_vaapi_profile_from_caps (state->caps)))))
      gst_tag_list_add (tags, GST_TAG_MERGE_REPLACE, GST_TAG_CODEC, codec,
          NULL);

    gst_video_encoder_merge_tags (venc, tags, GST_TAG_MERGE_REPLACE);
    gst_tag_list_unref (tags);
  }

  return TRUE;
}

static GstFlowReturn
gst_vaapiencode_handle_frame (GstVideoEncoder * venc,
    GstVideoCodecFrame * frame)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (venc);
  GstVaapiEncoderStatus status;
  GstVaapiVideoMeta *meta;
  GstVaapiSurfaceProxy *proxy;
  GstFlowReturn ret;
  GstBuffer *buf;
#if USE_H264_FEI_ENCODER
  GstVaapiFeiVideoMeta *feimeta = NULL;
  GstVaapiEncodeClass *const klass = GST_VAAPIENCODE_GET_CLASS (venc);
#endif

  buf = NULL;
  ret = gst_vaapi_plugin_base_get_input_buffer (GST_VAAPI_PLUGIN_BASE (encode),
      frame->input_buffer, &buf);
  if (ret != GST_FLOW_OK)
    goto error_buffer_invalid;

  gst_buffer_replace (&frame->input_buffer, buf);
  gst_buffer_unref (buf);

  meta = gst_buffer_get_vaapi_video_meta (buf);
  if (!meta)
    goto error_buffer_no_meta;

  proxy = gst_vaapi_video_meta_get_surface_proxy (meta);
  if (!proxy)
    goto error_buffer_no_surface_proxy;

#ifdef USE_H264_FEI_ENCODER
  feimeta = gst_buffer_get_vaapi_fei_video_meta (buf);
  if (feimeta && klass->load_control_data)
    klass->load_control_data (encode, feimeta, proxy);
#endif

  gst_video_codec_frame_set_user_data (frame,
      gst_vaapi_surface_proxy_ref (proxy),
      (GDestroyNotify) gst_vaapi_surface_proxy_unref);

  GST_VIDEO_ENCODER_STREAM_UNLOCK (encode);
  status = gst_vaapi_encoder_put_frame (encode->encoder, frame);
  GST_VIDEO_ENCODER_STREAM_LOCK (encode);
  if (status < GST_VAAPI_ENCODER_STATUS_SUCCESS)
    goto error_encode_frame;

  gst_video_codec_frame_unref (frame);
  return GST_FLOW_OK;

  /* ERRORS */
error_buffer_invalid:
  {
    if (buf)
      gst_buffer_unref (buf);
    gst_video_codec_frame_unref (frame);
    return ret;
  }
error_buffer_no_meta:
  {
    GST_ERROR ("failed to get GstVaapiVideoMeta information");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
error_buffer_no_surface_proxy:
  {
    GST_ERROR ("failed to get VA surface proxy");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
error_encode_frame:
  {
    GST_ERROR ("failed to encode frame %d (status %d)",
        frame->system_frame_number, status);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_vaapiencode_finish (GstVideoEncoder * venc)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (venc);
  GstVaapiEncoderStatus status;
  GstFlowReturn ret = GST_FLOW_OK;

  /* Don't try to destroy encoder if none was created in the first place.
     Return "not-negotiated" error since this means we did not even reach
     GstVideoEncoder::set_format() state, where the encoder could have
     been created */
  if (!encode->encoder)
    return GST_FLOW_NOT_NEGOTIATED;

  status = gst_vaapi_encoder_flush (encode->encoder);

  GST_VIDEO_ENCODER_STREAM_UNLOCK (encode);
  gst_pad_stop_task (GST_VAAPI_PLUGIN_BASE_SRC_PAD (encode));
  GST_VIDEO_ENCODER_STREAM_LOCK (encode);

  while (status == GST_VAAPI_ENCODER_STATUS_SUCCESS && ret == GST_FLOW_OK)
    ret = gst_vaapiencode_push_frame (encode, 0);

  if (ret == GST_VAAPI_ENCODE_FLOW_TIMEOUT)
    ret = GST_FLOW_OK;
  return ret;
}

static GstStateChangeReturn
gst_vaapiencode_change_state (GstElement * element, GstStateChange transition)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_pad_stop_task (GST_VAAPI_PLUGIN_BASE_SRC_PAD (encode));
      break;
    default:
      break;
  }
  return
      GST_ELEMENT_CLASS (gst_vaapiencode_parent_class)->change_state (element,
      transition);
}

static gboolean
gst_vaapiencode_propose_allocation (GstVideoEncoder * venc, GstQuery * query)
{
  GstVaapiPluginBase *const plugin = GST_VAAPI_PLUGIN_BASE (venc);

  if (!gst_vaapi_plugin_base_propose_allocation (plugin, query))
    return FALSE;
  return TRUE;
}

static gboolean
gst_vaapiencode_sink_event (GstVideoEncoder * venc, GstEvent * event)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (venc);
  GstPad *const srcpad = GST_VAAPI_PLUGIN_BASE_SRC_PAD (encode);
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_DOWNSTREAM:{
      const GstStructure *s = gst_event_get_structure (event);
      if (gst_structure_has_name (s, "GstVaapiEncoderRegionOfInterest")) {
        GstVaapiROI roi;

        if (!encode->encoder)
          return TRUE;

        if (!gst_structure_get_uint (s, "roi-x", &roi.rect.x) ||
            !gst_structure_get_uint (s, "roi-y", &roi.rect.y) ||
            !gst_structure_get_uint (s, "roi-width", &roi.rect.width) ||
            !gst_structure_get_uint (s, "roi-height", &roi.rect.height) ||
            !gst_structure_get_int (s, "roi-value", &roi.roi_value)) {
          return TRUE;
        }

        if (roi.roi_value == 0) {
          ret = gst_vaapi_encoder_del_roi (encode->encoder, &roi);
          if (ret) {
            GST_INFO_OBJECT (venc, "ROI: region with %d/%d/%d/%d is removed",
                roi.rect.x, roi.rect.y, roi.rect.width, roi.rect.height);
          }
        } else {
          ret = gst_vaapi_encoder_add_roi (encode->encoder, &roi);
          if (ret) {
            GST_INFO_OBJECT (venc, "ROI: region with %d/%d/%d/%d is added",
                roi.rect.x, roi.rect.y, roi.rect.width, roi.rect.height);
          }
        }
        gst_event_unref (event);
        return ret;
      }
      break;
    }
    default:
      break;
  }

  ret = GST_VIDEO_ENCODER_CLASS (gst_vaapiencode_parent_class)->sink_event
      (venc, event);
  if (!ret)
    return FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      gst_pad_pause_task (srcpad);
      break;
    case GST_EVENT_FLUSH_STOP:
      ret = gst_pad_start_task (srcpad,
          (GstTaskFunction) gst_vaapiencode_buffer_loop, encode, NULL);
      break;
    default:
      break;
  }

  return ret;
}

static gboolean
gst_vaapiencode_flush (GstVideoEncoder * venc)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (venc);
  GstVaapiEncoderStatus status;

  if (!encode->encoder)
    return FALSE;

  GST_LOG_OBJECT (encode, "flushing");

  status = gst_vaapi_encoder_flush (encode->encoder);
  if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
    return FALSE;
  gst_vaapiencode_purge (encode);

  gst_vaapi_encoder_replace (&encode->encoder, NULL);
  if (!ensure_encoder (encode))
    return FALSE;
  if (!set_codec_state (encode, encode->input_state))
    return FALSE;

  return TRUE;
}

static void
gst_vaapiencode_finalize (GObject * object)
{
  GstVaapiEncode *const encode = GST_VAAPIENCODE_CAST (object);

  gst_vaapiencode_destroy (encode);

  if (encode->prop_values) {
    g_ptr_array_unref (encode->prop_values);
    encode->prop_values = NULL;
  }

  gst_vaapi_plugin_base_finalize (GST_VAAPI_PLUGIN_BASE (object));
  G_OBJECT_CLASS (gst_vaapiencode_parent_class)->finalize (object);
}

static void
gst_vaapiencode_init (GstVaapiEncode * encode)
{
  GstVaapiPluginBase *const plugin = GST_VAAPI_PLUGIN_BASE (encode);

  gst_vaapi_plugin_base_init (GST_VAAPI_PLUGIN_BASE (encode), GST_CAT_DEFAULT);
  gst_pad_use_fixed_caps (plugin->srcpad);
}

static void
gst_vaapiencode_class_init (GstVaapiEncodeClass * klass)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  GstElementClass *const element_class = GST_ELEMENT_CLASS (klass);
  GstVideoEncoderClass *const venc_class = GST_VIDEO_ENCODER_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_vaapiencode_debug,
      GST_PLUGIN_NAME, 0, GST_PLUGIN_DESC);

  gst_vaapi_plugin_base_class_init (GST_VAAPI_PLUGIN_BASE_CLASS (klass));

  object_class->finalize = gst_vaapiencode_finalize;

  element_class->set_context = gst_vaapi_base_set_context;
  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_vaapiencode_change_state);

  venc_class->open = GST_DEBUG_FUNCPTR (gst_vaapiencode_open);
  venc_class->close = GST_DEBUG_FUNCPTR (gst_vaapiencode_close);
  venc_class->start = GST_DEBUG_FUNCPTR (gst_vaapiencode_start);
  venc_class->stop = GST_DEBUG_FUNCPTR (gst_vaapiencode_stop);
  venc_class->set_format = GST_DEBUG_FUNCPTR (gst_vaapiencode_set_format);
  venc_class->handle_frame = GST_DEBUG_FUNCPTR (gst_vaapiencode_handle_frame);
  venc_class->finish = GST_DEBUG_FUNCPTR (gst_vaapiencode_finish);
  venc_class->getcaps = GST_DEBUG_FUNCPTR (gst_vaapiencode_get_caps);
  venc_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_vaapiencode_propose_allocation);
  venc_class->flush = GST_DEBUG_FUNCPTR (gst_vaapiencode_flush);
  venc_class->sink_event = GST_DEBUG_FUNCPTR (gst_vaapiencode_sink_event);

  klass->get_property = gst_vaapiencode_default_get_property;
  klass->set_property = gst_vaapiencode_default_set_property;
  klass->alloc_buffer = gst_vaapiencode_default_alloc_buffer;

  venc_class->src_query = GST_DEBUG_FUNCPTR (gst_vaapiencode_src_query);
  venc_class->sink_query = GST_DEBUG_FUNCPTR (gst_vaapiencode_sink_query);
}

static inline GPtrArray *
get_properties (GstVaapiEncodeClass * klass)
{
  return klass->get_properties ? klass->get_properties () : NULL;
}

gboolean
gst_vaapiencode_init_properties (GstVaapiEncode * encode)
{
  GPtrArray *const props = get_properties (GST_VAAPIENCODE_GET_CLASS (encode));
  guint i;

  /* XXX: use base_init()/base_finalize() to avoid multiple initializations */
  if (!props)
    return FALSE;

  encode->prop_values =
      g_ptr_array_new_full (props->len, (GDestroyNotify) prop_value_free);
  if (!encode->prop_values) {
    g_ptr_array_unref (props);
    return FALSE;
  }

  for (i = 0; i < props->len; i++) {
    PropValue *const prop_value = prop_value_new ((GstVaapiEncoderPropInfo *)
        g_ptr_array_index (props, i));
    if (!prop_value)
      return FALSE;
    g_ptr_array_add (encode->prop_values, prop_value);
  }
  return TRUE;
}

gboolean
gst_vaapiencode_class_init_properties (GstVaapiEncodeClass * klass)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  GPtrArray *const props = get_properties (klass);
  guint i;

  if (!props)
    return FALSE;

  for (i = 0; i < props->len; i++) {
    GstVaapiEncoderPropInfo *const prop = g_ptr_array_index (props, i);
    g_object_class_install_property (object_class, PROP_BASE + i, prop->pspec);
  }
  g_ptr_array_unref (props);
  return TRUE;
}
