/*
 *  gstvaapiencoder.c - VA encoder abstraction
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

#include "sysdeps.h"
#include "gstvaapicompat.h"
#include "gstvaapiencoder.h"
#include "gstvaapiencoder_priv.h"
#include "gstvaapicontext.h"
#include "gstvaapidisplay_priv.h"
#include "gstvaapiutils.h"
#include "gstvaapiutils_core.h"
#include "gstvaapivalue.h"

#define DEBUG 1
#include "gstvaapidebug.h"

/* Helper function to create a new encoder property object */
static GstVaapiEncoderPropData *
prop_new (gint id, GParamSpec * pspec)
{
  GstVaapiEncoderPropData *prop;

  if (!id || !pspec)
    return NULL;

  prop = g_slice_new (GstVaapiEncoderPropData);
  if (!prop)
    return NULL;

  prop->prop = id;
  prop->pspec = g_param_spec_ref_sink (pspec);
  return prop;
}

/* Helper function to release a property object and any memory held herein */
static void
prop_free (GstVaapiEncoderPropData * prop)
{
  if (!prop)
    return;

  if (prop->pspec) {
    g_param_spec_unref (prop->pspec);
    prop->pspec = NULL;
  }
  g_slice_free (GstVaapiEncoderPropData, prop);
}

/* Helper function to lookup the supplied property specification */
static GParamSpec *
prop_find_pspec (GstVaapiEncoder * encoder, gint prop_id)
{
  GPtrArray *const props = encoder->properties;
  guint i;

  if (props) {
    for (i = 0; i < props->len; i++) {
      GstVaapiEncoderPropInfo *const prop = g_ptr_array_index (props, i);
      if (prop->prop == prop_id)
        return prop->pspec;
    }
  }
  return NULL;
}

/* Create a new array of properties, or NULL on error */
GPtrArray *
gst_vaapi_encoder_properties_append (GPtrArray * props, gint prop_id,
    GParamSpec * pspec)
{
  GstVaapiEncoderPropData *prop;

  if (!props) {
    props = g_ptr_array_new_with_free_func ((GDestroyNotify) prop_free);
    if (!props)
      return NULL;
  }

  prop = prop_new (prop_id, pspec);
  if (!prop)
    goto error_allocation_failed;
  g_ptr_array_add (props, prop);
  return props;

  /* ERRORS */
error_allocation_failed:
  {
    GST_ERROR ("failed to allocate encoder property info structure");
    g_ptr_array_unref (props);
    return NULL;
  }
}

/* Generate the common set of encoder properties */
GPtrArray *
gst_vaapi_encoder_properties_get_default (const GstVaapiEncoderClass * klass)
{
  const GstVaapiEncoderClassData *const cdata = klass->class_data;
  GPtrArray *props = NULL;

  g_assert (cdata->rate_control_get_type != NULL);

  /**
   * GstVaapiEncoder:rate-control:
   *
   * The desired rate control mode, expressed as a #GstVaapiRateControl.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_PROP_RATECONTROL,
      g_param_spec_enum ("rate-control",
          "Rate Control", "Rate control mode",
          cdata->rate_control_get_type (), cdata->default_rate_control,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoder:bitrate:
   *
   * The desired bitrate, expressed in kbps.
   * This is available when rate-control is CBR or VBR.
   *
   * CBR: This applies equally to minimum, maximum and target bitrate in the driver.
   * VBR: This applies to maximum bitrate in the driver.
   *      Minimum bitrate will be calculated like the following in the driver.
   *          if (target percentage < 50) minimum bitrate = 0
   *          else minimum bitrate = maximum bitrate * (2 * target percentage -100) / 100
   *      Target bitrate will be calculated like the following in the driver.
   *          target bitrate = maximum bitrate * target percentage / 100
   *
   * Note that target percentage is set as 70 currently in GStreamer VA-API.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_PROP_BITRATE,
      g_param_spec_uint ("bitrate",
          "Bitrate (kbps)",
          "The desired bitrate expressed in kbps (0: auto-calculate)",
          0, 100 * 1024, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoder:keyframe-period:
   *
   * The maximal distance between two keyframes.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_PROP_KEYFRAME_PERIOD,
      g_param_spec_uint ("keyframe-period",
          "Keyframe Period",
          "Maximal distance between two keyframes (0: auto-calculate)", 1, 300,
          30, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoder:tune:
   *
   * The desired encoder tuning option.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_PROP_TUNE,
      g_param_spec_enum ("tune",
          "Encoder Tuning",
          "Encoder tuning option",
          cdata->encoder_tune_get_type (), cdata->default_encoder_tune,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiEncoder:quality-level:
   *
   * The Encoding quality level.
   */
  GST_VAAPI_ENCODER_PROPERTIES_APPEND (props,
      GST_VAAPI_ENCODER_PROP_QUALITY_LEVEL,
      g_param_spec_uint ("quality-level",
          "Quality Level", "Encoding Quality Level "
          "(lower value means higher-quality/slow-encode, "
          " higher value means lower-quality/fast-encode)",
          1, 7, 4, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  return props;
}

gboolean
gst_vaapi_encoder_ensure_param_quality_level (GstVaapiEncoder * encoder,
    GstVaapiEncPicture * picture)
{
#if VA_CHECK_VERSION(0,36,0)
  GstVaapiEncMiscParam *misc;

  /* quality level param is not supported */
  if (GST_VAAPI_ENCODER_QUALITY_LEVEL (encoder) == 0)
    return TRUE;

  misc = GST_VAAPI_ENC_QUALITY_LEVEL_MISC_PARAM_NEW (encoder);
  if (!misc)
    return FALSE;
  memcpy (misc->data, &encoder->va_quality_level,
      sizeof (encoder->va_quality_level));
  gst_vaapi_enc_picture_add_misc_param (picture, misc);
  gst_vaapi_codec_object_replace (&misc, NULL);
#endif
  return TRUE;
}

gboolean
gst_vaapi_encoder_ensure_param_control_rate (GstVaapiEncoder * encoder,
    GstVaapiEncPicture * picture)
{
  GstVaapiEncMiscParam *misc;

  if (GST_VAAPI_ENCODER_RATE_CONTROL (encoder) == GST_VAAPI_RATECONTROL_CQP)
    return TRUE;

  /* RateControl params */
  misc = GST_VAAPI_ENC_MISC_PARAM_NEW (RateControl, encoder);
  if (!misc)
    return FALSE;
  memcpy (misc->data, &GST_VAAPI_ENCODER_VA_RATE_CONTROL (encoder),
      sizeof (VAEncMiscParameterRateControl));
  gst_vaapi_enc_picture_add_misc_param (picture, misc);
  gst_vaapi_codec_object_replace (&misc, NULL);

  /* HRD params */
  misc = GST_VAAPI_ENC_MISC_PARAM_NEW (HRD, encoder);
  if (!misc)
    return FALSE;
  memcpy (misc->data, &GST_VAAPI_ENCODER_VA_HRD (encoder),
      sizeof (VAEncMiscParameterHRD));
  gst_vaapi_enc_picture_add_misc_param (picture, misc);
  gst_vaapi_codec_object_replace (&misc, NULL);

  /* FrameRate params */
  if (GST_VAAPI_ENCODER_VA_FRAME_RATE (encoder).framerate == 0)
    return TRUE;

  misc = GST_VAAPI_ENC_MISC_PARAM_NEW (FrameRate, encoder);
  if (!misc)
    return FALSE;
  memcpy (misc->data, &GST_VAAPI_ENCODER_VA_FRAME_RATE (encoder),
      sizeof (VAEncMiscParameterFrameRate));
  gst_vaapi_enc_picture_add_misc_param (picture, misc);
  gst_vaapi_codec_object_replace (&misc, NULL);

  return TRUE;
}

/**
 * gst_vaapi_encoder_ref:
 * @encoder: a #GstVaapiEncoder
 *
 * Atomically increases the reference count of the given @encoder by one.
 *
 * Returns: The same @encoder argument
 */
GstVaapiEncoder *
gst_vaapi_encoder_ref (GstVaapiEncoder * encoder)
{
  return gst_vaapi_object_ref (encoder);
}

/**
 * gst_vaapi_encoder_unref:
 * @encoder: a #GstVaapiEncoder
 *
 * Atomically decreases the reference count of the @encoder by one. If
 * the reference count reaches zero, the encoder will be free'd.
 */
void
gst_vaapi_encoder_unref (GstVaapiEncoder * encoder)
{
  gst_vaapi_object_unref (encoder);
}

/**
 * gst_vaapi_encoder_replace:
 * @old_encoder_ptr: a pointer to a #GstVaapiEncoder
 * @new_encoder: a #GstVaapiEncoder
 *
 * Atomically replaces the encoder encoder held in @old_encoder_ptr
 * with @new_encoder. This means that @old_encoder_ptr shall reference
 * a valid encoder. However, @new_encoder can be NULL.
 */
void
gst_vaapi_encoder_replace (GstVaapiEncoder ** old_encoder_ptr,
    GstVaapiEncoder * new_encoder)
{
  gst_vaapi_object_replace (old_encoder_ptr, new_encoder);
}

/* Notifies gst_vaapi_encoder_create_coded_buffer() that a new buffer is free */
static void
_coded_buffer_proxy_released_notify (GstVaapiEncoder * encoder)
{
  g_mutex_lock (&encoder->mutex);
  g_cond_signal (&encoder->codedbuf_free);
  g_mutex_unlock (&encoder->mutex);
}

/* Creates a new VA coded buffer object proxy, backed from a pool */
static GstVaapiCodedBufferProxy *
gst_vaapi_encoder_create_coded_buffer (GstVaapiEncoder * encoder)
{
  GstVaapiCodedBufferPool *const pool =
      GST_VAAPI_CODED_BUFFER_POOL (encoder->codedbuf_pool);
  GstVaapiCodedBufferProxy *codedbuf_proxy;

  g_mutex_lock (&encoder->mutex);
  do {
    codedbuf_proxy = gst_vaapi_coded_buffer_proxy_new_from_pool (pool);
    if (codedbuf_proxy)
      break;

    /* Wait for a free coded buffer to become available */
    g_cond_wait (&encoder->codedbuf_free, &encoder->mutex);
    codedbuf_proxy = gst_vaapi_coded_buffer_proxy_new_from_pool (pool);
  } while (0);
  g_mutex_unlock (&encoder->mutex);
  if (!codedbuf_proxy)
    return NULL;

  gst_vaapi_coded_buffer_proxy_set_destroy_notify (codedbuf_proxy,
      (GDestroyNotify) _coded_buffer_proxy_released_notify, encoder);
  return codedbuf_proxy;
}

/* Notifies gst_vaapi_encoder_create_surface() that a new surface is free */
static void
_surface_proxy_released_notify (GstVaapiEncoder * encoder)
{
  g_mutex_lock (&encoder->mutex);
  g_cond_signal (&encoder->surface_free);
  g_mutex_unlock (&encoder->mutex);
}

/* Creates a new VA surface object proxy, backed from a pool and
   useful to allocate reconstructed surfaces */
GstVaapiSurfaceProxy *
gst_vaapi_encoder_create_surface (GstVaapiEncoder * encoder)
{
  GstVaapiSurfaceProxy *proxy;

  g_return_val_if_fail (encoder->context != NULL, NULL);

  g_mutex_lock (&encoder->mutex);
  for (;;) {
    proxy = gst_vaapi_context_get_surface_proxy (encoder->context);
    if (proxy)
      break;

    /* Wait for a free surface proxy to become available */
    g_cond_wait (&encoder->surface_free, &encoder->mutex);
  }
  g_mutex_unlock (&encoder->mutex);

  gst_vaapi_surface_proxy_set_destroy_notify (proxy,
      (GDestroyNotify) _surface_proxy_released_notify, encoder);
  return proxy;
}

/**
 * gst_vaapi_encoder_put_frame:
 * @encoder: a #GstVaapiEncoder
 * @frame: a #GstVideoCodecFrame
 *
 * Queues a #GstVideoCodedFrame to the HW encoder. The encoder holds
 * an extra reference to the @frame.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_put_frame (GstVaapiEncoder * encoder,
    GstVideoCodecFrame * frame)
{
  GstVaapiEncoderClass *const klass = GST_VAAPI_ENCODER_GET_CLASS (encoder);
  GstVaapiEncoderStatus status;
  GstVaapiEncPicture *picture;
  GstVaapiCodedBufferProxy *codedbuf_proxy;

  for (;;) {
    picture = NULL;
    status = klass->reordering (encoder, frame, &picture);
    if (status == GST_VAAPI_ENCODER_STATUS_NO_SURFACE)
      break;
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
      goto error_reorder_frame;

    codedbuf_proxy = gst_vaapi_encoder_create_coded_buffer (encoder);
    if (!codedbuf_proxy)
      goto error_create_coded_buffer;

    status = klass->encode (encoder, picture, codedbuf_proxy);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
      goto error_encode;

    gst_vaapi_coded_buffer_proxy_set_user_data (codedbuf_proxy,
        picture, (GDestroyNotify) gst_vaapi_mini_object_unref);
    g_async_queue_push (encoder->codedbuf_queue, codedbuf_proxy);
    encoder->num_codedbuf_queued++;

    /* Try again with any pending reordered frame now available for encoding */
    frame = NULL;
  }
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_reorder_frame:
  {
    GST_ERROR ("failed to process reordered frames");
    return status;
  }
error_create_coded_buffer:
  {
    GST_ERROR ("failed to allocate coded buffer");
    gst_vaapi_enc_picture_unref (picture);
    return GST_VAAPI_ENCODER_STATUS_ERROR_ALLOCATION_FAILED;
  }
error_encode:
  {
    GST_ERROR ("failed to encode frame (status = %d)", status);
    gst_vaapi_enc_picture_unref (picture);
    gst_vaapi_coded_buffer_proxy_unref (codedbuf_proxy);
    return status;
  }
}

/**
 * gst_vaapi_encoder_get_buffer_with_timeout:
 * @encoder: a #GstVaapiEncoder
 * @out_codedbuf_proxy_ptr: the next coded buffer as a #GstVaapiCodedBufferProxy
 * @timeout: the number of microseconds to wait for the coded buffer, at most
 *
 * Upon successful return, *@out_codedbuf_proxy_ptr contains the next
 * coded buffer as a #GstVaapiCodedBufferProxy. The caller owns this
 * object, so gst_vaapi_coded_buffer_proxy_unref() shall be called
 * after usage. Otherwise, @GST_VAAPI_DECODER_STATUS_ERROR_NO_BUFFER
 * is returned if no coded buffer is available so far (timeout).
 *
 * The parent frame is available as a #GstVideoCodecFrame attached to
 * the user-data anchor of the output coded buffer. Ownership of the
 * frame is transferred to the coded buffer.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_get_buffer_with_timeout (GstVaapiEncoder * encoder,
    GstVaapiCodedBufferProxy ** out_codedbuf_proxy_ptr, guint64 timeout)
{
  GstVaapiEncPicture *picture;
  GstVaapiCodedBufferProxy *codedbuf_proxy;

  codedbuf_proxy = g_async_queue_timeout_pop (encoder->codedbuf_queue, timeout);
  if (!codedbuf_proxy)
    return GST_VAAPI_ENCODER_STATUS_NO_BUFFER;

  /* Wait for completion of all operations and report any error that occurred */
  picture = gst_vaapi_coded_buffer_proxy_get_user_data (codedbuf_proxy);
  if (!gst_vaapi_surface_sync (picture->surface))
    goto error_invalid_buffer;

  gst_vaapi_coded_buffer_proxy_set_user_data (codedbuf_proxy,
      gst_video_codec_frame_ref (picture->frame),
      (GDestroyNotify) gst_video_codec_frame_unref);

  if (out_codedbuf_proxy_ptr)
    *out_codedbuf_proxy_ptr = gst_vaapi_coded_buffer_proxy_ref (codedbuf_proxy);
  gst_vaapi_coded_buffer_proxy_unref (codedbuf_proxy);
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_invalid_buffer:
  {
    GST_ERROR ("failed to encode the frame");
    gst_vaapi_coded_buffer_proxy_unref (codedbuf_proxy);
    return GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_SURFACE;
  }
}

/**
 * gst_vaapi_encoder_flush:
 * @encoder: a #GstVaapiEncoder
 *
 * Submits any pending (reordered) frame for encoding.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_flush (GstVaapiEncoder * encoder)
{
  GstVaapiEncoderClass *const klass = GST_VAAPI_ENCODER_GET_CLASS (encoder);

  return klass->flush (encoder);
}

/**
 * gst_vaapi_encoder_get_codec_data:
 * @encoder: a #GstVaapiEncoder
 * @out_codec_data_ptr: the pointer to the resulting codec-data (#GstBuffer)
 *
 * Returns a codec-data buffer that best represents the encoded
 * bitstream. Upon successful return, and if the @out_codec_data_ptr
 * contents is not NULL, then the caller function shall deallocates
 * that buffer with gst_buffer_unref().
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_get_codec_data (GstVaapiEncoder * encoder,
    GstBuffer ** out_codec_data_ptr)
{
  GstVaapiEncoderStatus ret = GST_VAAPI_ENCODER_STATUS_SUCCESS;
  GstVaapiEncoderClass *const klass = GST_VAAPI_ENCODER_GET_CLASS (encoder);

  *out_codec_data_ptr = NULL;
  if (!klass->get_codec_data)
    return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  ret = klass->get_codec_data (encoder, out_codec_data_ptr);
  return ret;
}

/* Checks video info */
static GstVaapiEncoderStatus
check_video_info (GstVaapiEncoder * encoder, const GstVideoInfo * vip)
{
  if (!vip->width || !vip->height)
    goto error_invalid_resolution;
  if (vip->fps_n < 0 || vip->fps_d <= 0)
    goto error_invalid_framerate;
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_invalid_resolution:
  {
    GST_ERROR ("invalid resolution (%dx%d)", vip->width, vip->height);
    return GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
  }
error_invalid_framerate:
  {
    GST_ERROR ("invalid framerate (%d/%d)", vip->fps_n, vip->fps_d);
    return GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
  }
}

/* Gets a compatible profile for the active codec */
static GstVaapiProfile
get_compatible_profile (GstVaapiEncoder * encoder)
{
  const GstVaapiEncoderClassData *const cdata =
      GST_VAAPI_ENCODER_GET_CLASS (encoder)->class_data;
  GstVaapiProfile profile;
  GArray *profiles;
  guint i;

  profiles = gst_vaapi_display_get_encode_profiles (encoder->display);
  if (!profiles)
    return GST_VAAPI_PROFILE_UNKNOWN;

  // Pick a profile matching the class codec
  for (i = 0; i < profiles->len; i++) {
    profile = g_array_index (profiles, GstVaapiProfile, i);
    if (gst_vaapi_profile_get_codec (profile) == cdata->codec)
      break;
  }
  if (i == profiles->len)
    profile = GST_VAAPI_PROFILE_UNKNOWN;

  g_array_unref (profiles);
  return profile;
}

/* Gets a supported profile for the active codec */
static GstVaapiProfile
get_profile (GstVaapiEncoder * encoder)
{
  if (!encoder->profile)
    encoder->profile = get_compatible_profile (encoder);
  return encoder->profile;
}

/* Gets config attribute for the supplied profile */
static gboolean
get_config_attribute (GstVaapiEncoder * encoder, VAConfigAttribType type,
    guint * out_value_ptr)
{
  GstVaapiProfile profile;
  VAProfile va_profile;
  VAEntrypoint va_entrypoint;

  profile = get_profile (encoder);
  if (!profile)
    return FALSE;
  va_profile = gst_vaapi_profile_get_va_profile (profile);

  va_entrypoint =
      gst_vaapi_entrypoint_get_va_entrypoint (encoder->context_info.entrypoint);

  return gst_vaapi_get_config_attribute (encoder->display, va_profile,
      va_entrypoint, type, out_value_ptr);
}

/* Determines the set of supported packed headers */
static guint
get_packed_headers (GstVaapiEncoder * encoder)
{
  const GstVaapiEncoderClassData *const cdata =
      GST_VAAPI_ENCODER_GET_CLASS (encoder)->class_data;
  guint value;

  if (encoder->got_packed_headers)
    return encoder->packed_headers;

  if (!get_config_attribute (encoder, VAConfigAttribEncPackedHeaders, &value))
    value = 0;
  GST_INFO ("supported packed headers: 0x%08x", value);

  encoder->got_packed_headers = TRUE;
  encoder->packed_headers = cdata->packed_headers & value;

  if (cdata->codec == GST_VAAPI_CODEC_JPEG) {
#if !VA_CHECK_VERSION(0,37,1)
    encoder->packed_headers = VA_ENC_PACKED_HEADER_RAW_DATA;
    GST_DEBUG ("Hard coding the packed header flag value to "
        "VA_ENC_PACKED_HEADER_RAW_DATA. This is a work around for the driver "
        "bug");
#endif
  }

  return encoder->packed_headers;
}

static gboolean
get_roi_capability (GstVaapiEncoder * encoder, guint * num_roi_supported)
{
#if VA_CHECK_VERSION(0,39,1)
  VAConfigAttribValEncROI *roi_config;
  guint value;

  if (!get_config_attribute (encoder, VAConfigAttribEncROI, &value))
    return FALSE;

  roi_config = (VAConfigAttribValEncROI *) & value;

  if (roi_config->bits.num_roi_regions == 0 ||
      roi_config->bits.roi_rc_qp_delat_support == 0)
    return FALSE;

  GST_INFO ("Support for ROI - number of regions supported: %d",
      roi_config->bits.num_roi_regions);

  *num_roi_supported = roi_config->bits.num_roi_regions;
  return TRUE;
#else
  return FALSE;
#endif
}

static inline gboolean
is_chroma_type_supported (GstVaapiEncoder * encoder)
{
  GstVaapiContextInfo *const cip = &encoder->context_info;
  const GstVideoFormat fmt =
      GST_VIDEO_INFO_FORMAT (GST_VAAPI_ENCODER_VIDEO_INFO (encoder));
  guint format = 0;

  if (fmt == GST_VIDEO_FORMAT_ENCODED)
    return TRUE;

  if (cip->chroma_type != GST_VAAPI_CHROMA_TYPE_YUV420 &&
      cip->chroma_type != GST_VAAPI_CHROMA_TYPE_YUV422 &&
      cip->chroma_type != GST_VAAPI_CHROMA_TYPE_YUV420_10BPP)
    goto unsupported;

  if (!get_config_attribute (encoder, VAConfigAttribRTFormat, &format))
    return FALSE;

  if (!(format & from_GstVaapiChromaType (cip->chroma_type)))
    goto unsupported;

  return TRUE;

  /* ERRORS */
unsupported:
  {
    GST_ERROR ("We only support YUV 4:2:0 and YUV 4:2:2 for encoding. "
        "Please try to use vaapipostproc to convert the input format.");
    return FALSE;
  }
}

static guint
get_default_chroma_type (GstVaapiEncoder * encoder,
    const GstVaapiContextInfo * cip)
{
  guint value;

  if (!gst_vaapi_get_config_attribute (encoder->display,
          gst_vaapi_profile_get_va_profile (cip->profile),
          gst_vaapi_entrypoint_get_va_entrypoint (cip->entrypoint),
          VAConfigAttribRTFormat, &value))
    return 0;

  return to_GstVaapiChromaType (value);
}

static void
init_context_info (GstVaapiEncoder * encoder, GstVaapiContextInfo * cip,
    GstVaapiProfile profile)
{
  const GstVaapiEncoderClassData *const cdata =
      GST_VAAPI_ENCODER_GET_CLASS (encoder)->class_data;

  cip->usage = GST_VAAPI_CONTEXT_USAGE_ENCODE;
  cip->profile = profile;
  if (cdata->codec == GST_VAAPI_CODEC_JPEG) {
    cip->entrypoint = GST_VAAPI_ENTRYPOINT_PICTURE_ENCODE;
  } else {
    if (cip->entrypoint != GST_VAAPI_ENTRYPOINT_SLICE_ENCODE_LP &&
        cip->entrypoint != GST_VAAPI_ENTRYPOINT_SLICE_ENCODE_FEI)
      cip->entrypoint = GST_VAAPI_ENTRYPOINT_SLICE_ENCODE;
  }
  cip->chroma_type = get_default_chroma_type (encoder, cip);
  cip->width = 0;
  cip->height = 0;
  cip->ref_frames = encoder->num_ref_frames;
}

/* Updates video context */
static gboolean
set_context_info (GstVaapiEncoder * encoder)
{
  GstVaapiContextInfo *const cip = &encoder->context_info;
  GstVaapiConfigInfoEncoder *const config = &cip->config.encoder;
  const GstVideoFormat format =
      GST_VIDEO_INFO_FORMAT (GST_VAAPI_ENCODER_VIDEO_INFO (encoder));
  guint fei_function = config->fei_function;

  init_context_info (encoder, cip, get_profile (encoder));

  cip->chroma_type = gst_vaapi_video_format_get_chroma_type (format);
  cip->width = GST_VAAPI_ENCODER_WIDTH (encoder);
  cip->height = GST_VAAPI_ENCODER_HEIGHT (encoder);

  if (!is_chroma_type_supported (encoder))
    goto error_unsupported_format;

  memset (config, 0, sizeof (*config));
  config->rc_mode = GST_VAAPI_ENCODER_RATE_CONTROL (encoder);
  config->packed_headers = get_packed_headers (encoder);
  config->roi_capability =
      get_roi_capability (encoder, &config->roi_num_supported);
  config->fei_function = fei_function;

  return TRUE;

  /* ERRORS */
error_unsupported_format:
  {
    GST_ERROR ("failed to determine chroma type for format %s",
        gst_vaapi_video_format_to_string (format));
    return FALSE;
  }
}

/* Ensures the underlying VA context for encoding is created */
static gboolean
gst_vaapi_encoder_ensure_context (GstVaapiEncoder * encoder)
{
  GstVaapiContextInfo *const cip = &encoder->context_info;

  if (!set_context_info (encoder))
    return FALSE;

  if (encoder->context) {
    if (!gst_vaapi_context_reset (encoder->context, cip))
      return FALSE;
  } else {
    encoder->context = gst_vaapi_context_new (encoder->display, cip);
    if (!encoder->context)
      return FALSE;
  }
  encoder->va_context = gst_vaapi_context_get_id (encoder->context);
  return TRUE;
}

/* Reconfigures the encoder with the new properties */
static GstVaapiEncoderStatus
gst_vaapi_encoder_reconfigure_internal (GstVaapiEncoder * encoder)
{
  GstVaapiEncoderClass *const klass = GST_VAAPI_ENCODER_GET_CLASS (encoder);
  GstVideoInfo *const vip = GST_VAAPI_ENCODER_VIDEO_INFO (encoder);
  GstVaapiEncoderStatus status;
  GstVaapiVideoPool *pool;
  guint codedbuf_size, target_percentage;
  guint fps_d, fps_n;
#if VA_CHECK_VERSION(0,36,0)
  guint quality_level_max = 0;
#endif

  fps_d = GST_VIDEO_INFO_FPS_D (vip);
  fps_n = GST_VIDEO_INFO_FPS_N (vip);

  /* Generate a keyframe every second */
  if (!encoder->keyframe_period)
    encoder->keyframe_period = (fps_n + fps_d - 1) / fps_d;

  /* Default frame rate parameter */
  if (fps_d > 0 && fps_n > 0)
    GST_VAAPI_ENCODER_VA_FRAME_RATE (encoder).framerate = fps_d << 16 | fps_n;

  target_percentage =
      (GST_VAAPI_ENCODER_RATE_CONTROL (encoder) == GST_VAAPI_RATECONTROL_CBR) ?
      100 : 70;

  /* *INDENT-OFF* */
  /* Default values for rate control parameter */
  GST_VAAPI_ENCODER_VA_RATE_CONTROL (encoder) = (VAEncMiscParameterRateControl) {
    .bits_per_second = encoder->bitrate * 1000,
    .target_percentage = target_percentage,
    .window_size = 500,
  };
  /* *INDENT-ON* */

  status = klass->reconfigure (encoder);
  if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
    return status;

  if (!gst_vaapi_encoder_ensure_context (encoder))
    goto error_reset_context;

  /* Currently only FEI entrypoint needed this.
   *
   * FEI ENC+PAK requires two contexts where the first one is for ENC
   * and the second one is for PAK */
  if (klass->ensure_secondary_context
      && !klass->ensure_secondary_context (encoder))
    goto error_reset_secondary_context;

#if VA_CHECK_VERSION(0,36,0)
  if (get_config_attribute (encoder, VAConfigAttribEncQualityRange,
          &quality_level_max) && quality_level_max > 0) {
    GST_VAAPI_ENCODER_QUALITY_LEVEL (encoder) =
        CLAMP (GST_VAAPI_ENCODER_QUALITY_LEVEL (encoder), 1, quality_level_max);
  } else {
    GST_VAAPI_ENCODER_QUALITY_LEVEL (encoder) = 0;
  }
  GST_INFO ("Quality level is fixed to %d",
      GST_VAAPI_ENCODER_QUALITY_LEVEL (encoder));
#endif

  codedbuf_size = encoder->codedbuf_pool ?
      gst_vaapi_coded_buffer_pool_get_buffer_size (GST_VAAPI_CODED_BUFFER_POOL
      (encoder)) : 0;
  if (codedbuf_size != encoder->codedbuf_size) {
    pool = gst_vaapi_coded_buffer_pool_new (encoder, encoder->codedbuf_size);
    if (!pool)
      goto error_alloc_codedbuf_pool;
    gst_vaapi_video_pool_set_capacity (pool, 5);
    gst_vaapi_video_pool_replace (&encoder->codedbuf_pool, pool);
    gst_vaapi_video_pool_unref (pool);
  }
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_alloc_codedbuf_pool:
  {
    GST_ERROR ("failed to initialize coded buffer pool");
    return GST_VAAPI_ENCODER_STATUS_ERROR_ALLOCATION_FAILED;
  }
error_reset_context:
  {
    GST_ERROR ("failed to update VA context");
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;
  }
error_reset_secondary_context:
  {
    GST_ERROR ("failed to create/update secondary VA context");
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;
  }
}

/**
 * gst_vaapi_encoder_set_codec_state:
 * @encoder: a #GstVaapiEncoder
 * @state : a #GstVideoCodecState
 *
 * Notifies the encoder about the source surface properties. The
 * accepted set of properties is: video resolution, colorimetry,
 * pixel-aspect-ratio and framerate.
 *
 * This function is a synchronization point for codec configuration.
 * This means that, at this point, the encoder is reconfigured to
 * match the new properties and any other change beyond this point has
 * zero effect.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_set_codec_state (GstVaapiEncoder * encoder,
    GstVideoCodecState * state)
{
  GstVaapiEncoderStatus status;

  g_return_val_if_fail (encoder != NULL,
      GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER);
  g_return_val_if_fail (state != NULL,
      GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER);

  if (!gst_video_info_is_equal (&state->info, &encoder->video_info)) {
    status = check_video_info (encoder, &state->info);
    if (status != GST_VAAPI_ENCODER_STATUS_SUCCESS)
      return status;
    encoder->video_info = state->info;
  }
  return gst_vaapi_encoder_reconfigure_internal (encoder);
}

/**
 * gst_vaapi_encoder_set_property:
 * @encoder: a #GstVaapiEncoder
 * @prop_id: the id of the property to change
 * @value: the new value to set
 *
 * Update the requested property, designed by @prop_id, with the
 * supplied @value. A @NULL value argument resets the property to its
 * default value.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
static GstVaapiEncoderStatus
set_property (GstVaapiEncoder * encoder, gint prop_id, const GValue * value)
{
  GstVaapiEncoderStatus status =
      GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER;

  g_assert (value != NULL);

  /* Handle codec-specific properties */
  if (prop_id < 0) {
    GstVaapiEncoderClass *const klass = GST_VAAPI_ENCODER_GET_CLASS (encoder);

    if (klass->set_property) {
      if (encoder->num_codedbuf_queued > 0)
        goto error_operation_failed;
      status = klass->set_property (encoder, prop_id, value);
    }
    return status;
  }

  /* Handle common properties */
  switch (prop_id) {
    case GST_VAAPI_ENCODER_PROP_RATECONTROL:
      status = gst_vaapi_encoder_set_rate_control (encoder,
          g_value_get_enum (value));
      break;
    case GST_VAAPI_ENCODER_PROP_BITRATE:
      status = gst_vaapi_encoder_set_bitrate (encoder,
          g_value_get_uint (value));
      break;
    case GST_VAAPI_ENCODER_PROP_KEYFRAME_PERIOD:
      status = gst_vaapi_encoder_set_keyframe_period (encoder,
          g_value_get_uint (value));
      break;
    case GST_VAAPI_ENCODER_PROP_TUNE:
      status = gst_vaapi_encoder_set_tuning (encoder, g_value_get_enum (value));
      break;
    case GST_VAAPI_ENCODER_PROP_QUALITY_LEVEL:
      status = gst_vaapi_encoder_set_quality_level (encoder,
          g_value_get_uint (value));
      break;
  }
  return status;

  /* ERRORS */
error_operation_failed:
  {
    GST_ERROR ("could not change codec state after encoding started");
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;
  }
}

GstVaapiEncoderStatus
gst_vaapi_encoder_set_property (GstVaapiEncoder * encoder, gint prop_id,
    const GValue * value)
{
  GstVaapiEncoderStatus status;
  GValue default_value = G_VALUE_INIT;

  g_return_val_if_fail (encoder != NULL,
      GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER);

  if (!value) {
    GParamSpec *const pspec = prop_find_pspec (encoder, prop_id);
    if (!pspec)
      goto error_invalid_property;

    g_value_init (&default_value, pspec->value_type);
    g_param_value_set_default (pspec, &default_value);
    value = &default_value;
  }

  status = set_property (encoder, prop_id, value);

  if (default_value.g_type)
    g_value_unset (&default_value);
  return status;

  /* ERRORS */
error_invalid_property:
  {
    GST_ERROR ("unsupported property (%d)", prop_id);
    return GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER;
  }
}

/* Determine the supported rate control modes */
static guint
get_rate_control_mask (GstVaapiEncoder * encoder)
{
  const GstVaapiEncoderClassData *const cdata =
      GST_VAAPI_ENCODER_GET_CLASS (encoder)->class_data;
  guint i, value, rate_control_mask = 0;

  if (encoder->got_rate_control_mask)
    return encoder->rate_control_mask;

  if (get_config_attribute (encoder, VAConfigAttribRateControl, &value)) {
    for (i = 0; i < 32; i++) {
      if (!(value & (1U << i)))
        continue;
      rate_control_mask |= 1 << to_GstVaapiRateControl (1 << i);
    }
    GST_INFO ("supported rate controls: 0x%08x", rate_control_mask);

    encoder->got_rate_control_mask = TRUE;
    encoder->rate_control_mask = cdata->rate_control_mask & rate_control_mask;
  }

  return encoder->rate_control_mask;
}

/**
 * gst_vaapi_encoder_set_rate_control:
 * @encoder: a #GstVaapiEncoder
 * @rate_control: the requested rate control
 *
 * Notifies the @encoder to use the supplied @rate_control mode.
 *
 * If the underlying encoder does not support that rate control mode,
 * then @GST_VAAPI_ENCODER_STATUS_ERROR_UNSUPPORTED_RATE_CONTROL is
 * returned.
 *
 * The rate control mode can only be specified before the first frame
 * is to be encoded. Afterwards, any change to this parameter is
 * invalid and @GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED is
 * returned.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_set_rate_control (GstVaapiEncoder * encoder,
    GstVaapiRateControl rate_control)
{
  guint32 rate_control_mask;

  g_return_val_if_fail (encoder != NULL,
      GST_VAAPI_ENCODER_STATUS_ERROR_INVALID_PARAMETER);

  if (encoder->rate_control != rate_control && encoder->num_codedbuf_queued > 0)
    goto error_operation_failed;

  rate_control_mask = get_rate_control_mask (encoder);
  if (rate_control_mask && !(rate_control_mask & (1U << rate_control)))
    goto error_unsupported_rate_control;

  encoder->rate_control = rate_control;
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_operation_failed:
  {
    GST_ERROR ("could not change rate control mode after encoding started");
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;
  }
error_unsupported_rate_control:
  {
    GST_ERROR ("unsupported rate control mode (%d)", rate_control);
    return GST_VAAPI_ENCODER_STATUS_ERROR_UNSUPPORTED_RATE_CONTROL;
  }
}

/**
 * gst_vaapi_encoder_set_bitrate:
 * @encoder: a #GstVaapiEncoder
 * @bitrate: the requested bitrate (in kbps)
 *
 * Notifies the @encoder to use the supplied @bitrate value.
 *
 * Note: currently, the bitrate can only be specified before the first
 * frame is encoded. Afterwards, any change to this parameter is
 * invalid and @GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED is
 * returned.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_set_bitrate (GstVaapiEncoder * encoder, guint bitrate)
{
  g_return_val_if_fail (encoder != NULL, 0);

  if (encoder->bitrate != bitrate && encoder->num_codedbuf_queued > 0)
    goto error_operation_failed;

  encoder->bitrate = bitrate;
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_operation_failed:
  {
    GST_ERROR ("could not change bitrate value after encoding started");
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;
  }
}

/**
 * gst_vaapi_encoder_set_keyframe_period:
 * @encoder: a #GstVaapiEncoder
 * @keyframe_period: the maximal distance between two keyframes
 *
 * Notifies the @encoder to use the supplied @keyframe_period value.
 *
 * Note: currently, the keyframe period can only be specified before
 * the last call to gst_vaapi_encoder_set_codec_state(), which shall
 * occur before the first frame is encoded. Afterwards, any change to
 * this parameter causes gst_vaapi_encoder_set_keyframe_period() to
 * return @GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_set_keyframe_period (GstVaapiEncoder * encoder,
    guint keyframe_period)
{
  g_return_val_if_fail (encoder != NULL, 0);

  if (encoder->keyframe_period != keyframe_period
      && encoder->num_codedbuf_queued > 0)
    goto error_operation_failed;

  encoder->keyframe_period = keyframe_period;
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_operation_failed:
  {
    GST_ERROR ("could not change keyframe period after encoding started");
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;
  }
}

/**
 * gst_vaapi_encoder_set_tuning:
 * @encoder: a #GstVaapiEncoder
 * @tuning: the #GstVaapiEncoderTune option
 *
 * Notifies the @encoder to use the supplied @tuning option.
 *
 * Note: currently, the tuning option can only be specified before the
 * last call to gst_vaapi_encoder_set_codec_state(), which shall occur
 * before the first frame is encoded. Afterwards, any change to this
 * parameter causes gst_vaapi_encoder_set_tuning() to return
 * @GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_set_tuning (GstVaapiEncoder * encoder,
    GstVaapiEncoderTune tuning)
{
  g_return_val_if_fail (encoder != NULL, 0);

  if (encoder->tune != tuning && encoder->num_codedbuf_queued > 0)
    goto error_operation_failed;

  encoder->tune = tuning;
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_operation_failed:
  {
    GST_ERROR ("could not change tuning options after encoding started");
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;
  }
}

/**
 * gst_vaapi_encoder_set_quality_level:
 * @encoder: a #GstVaapiEncoder
 * @quality_level: the encoder quality level
 *
 * Notifies the @encoder to use the supplied @quality_level value.
 *
 * Note: currently, the quality_level can only be specified before
 * the last call to gst_vaapi_encoder_set_codec_state(), which shall
 * occur before the first frame is encoded. Afterwards, any change to
 * this parameter causes gst_vaapi_encoder_set_quality_level() to
 * return @GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED.
 *
 * Return value: a #GstVaapiEncoderStatus
 */
GstVaapiEncoderStatus
gst_vaapi_encoder_set_quality_level (GstVaapiEncoder * encoder,
    guint quality_level)
{
  g_return_val_if_fail (encoder != NULL, 0);

  if (GST_VAAPI_ENCODER_QUALITY_LEVEL (encoder) != quality_level
      && encoder->num_codedbuf_queued > 0)
    goto error_operation_failed;

  GST_VAAPI_ENCODER_QUALITY_LEVEL (encoder) = quality_level;
  return GST_VAAPI_ENCODER_STATUS_SUCCESS;

  /* ERRORS */
error_operation_failed:
  {
    GST_ERROR ("could not change quality level after encoding started");
    return GST_VAAPI_ENCODER_STATUS_ERROR_OPERATION_FAILED;
  }
}

/* Initialize default values for configurable properties */
static gboolean
gst_vaapi_encoder_init_properties (GstVaapiEncoder * encoder)
{
  GstVaapiEncoderClass *const klass = GST_VAAPI_ENCODER_GET_CLASS (encoder);
  GPtrArray *props;
  guint i;

  props = klass->get_default_properties ();
  if (!props)
    return FALSE;

  encoder->properties = props;
  for (i = 0; i < props->len; i++) {
    GstVaapiEncoderPropInfo *const prop = g_ptr_array_index (props, i);

    if (gst_vaapi_encoder_set_property (encoder, prop->prop,
            NULL) != GST_VAAPI_ENCODER_STATUS_SUCCESS)
      return FALSE;
  }
  return TRUE;
}

/* Base encoder initialization (internal) */
static gboolean
gst_vaapi_encoder_init (GstVaapiEncoder * encoder, GstVaapiDisplay * display)
{
  GstVaapiEncoderClass *const klass = GST_VAAPI_ENCODER_GET_CLASS (encoder);

  g_return_val_if_fail (display != NULL, FALSE);

#define CHECK_VTABLE_HOOK(FUNC) do {            \
    if (!klass->FUNC)                           \
      goto error_invalid_vtable;                \
  } while (0)

  CHECK_VTABLE_HOOK (init);
  CHECK_VTABLE_HOOK (finalize);
  CHECK_VTABLE_HOOK (get_default_properties);
  CHECK_VTABLE_HOOK (reconfigure);
  CHECK_VTABLE_HOOK (encode);
  CHECK_VTABLE_HOOK (reordering);
  CHECK_VTABLE_HOOK (flush);

#undef CHECK_VTABLE_HOOK

  encoder->display = gst_vaapi_display_ref (display);
  encoder->va_display = gst_vaapi_display_get_display (display);
  encoder->va_context = VA_INVALID_ID;

  gst_video_info_init (&encoder->video_info);

  g_mutex_init (&encoder->mutex);
  g_cond_init (&encoder->surface_free);
  g_cond_init (&encoder->codedbuf_free);

  encoder->codedbuf_queue = g_async_queue_new_full ((GDestroyNotify)
      gst_vaapi_coded_buffer_proxy_unref);
  if (!encoder->codedbuf_queue)
    return FALSE;

  if (!klass->init (encoder))
    return FALSE;
  if (!gst_vaapi_encoder_init_properties (encoder))
    return FALSE;
  return TRUE;

  /* ERRORS */
error_invalid_vtable:
  {
    GST_ERROR ("invalid subclass hook (internal error)");
    return FALSE;
  }
}

/* Base encoder cleanup (internal) */
void
gst_vaapi_encoder_finalize (GstVaapiEncoder * encoder)
{
  GstVaapiEncoderClass *const klass = GST_VAAPI_ENCODER_GET_CLASS (encoder);

  klass->finalize (encoder);

  if (encoder->roi_regions)
    g_list_free_full (encoder->roi_regions, g_free);

  gst_vaapi_object_replace (&encoder->context, NULL);
  gst_vaapi_display_replace (&encoder->display, NULL);
  encoder->va_display = NULL;

  if (encoder->properties) {
    g_ptr_array_unref (encoder->properties);
    encoder->properties = NULL;
  }

  gst_vaapi_video_pool_replace (&encoder->codedbuf_pool, NULL);
  if (encoder->codedbuf_queue) {
    g_async_queue_unref (encoder->codedbuf_queue);
    encoder->codedbuf_queue = NULL;
  }
  g_cond_clear (&encoder->surface_free);
  g_cond_clear (&encoder->codedbuf_free);
  g_mutex_clear (&encoder->mutex);
}

/* Helper function to create new GstVaapiEncoder instances (internal) */
GstVaapiEncoder *
gst_vaapi_encoder_new (const GstVaapiEncoderClass * klass,
    GstVaapiDisplay * display)
{
  GstVaapiEncoder *encoder;

  encoder = (GstVaapiEncoder *)
      gst_vaapi_mini_object_new0 (GST_VAAPI_MINI_OBJECT_CLASS (klass));
  if (!encoder)
    return NULL;

  if (!gst_vaapi_encoder_init (encoder, display))
    goto error;
  return encoder;

  /* ERRORS */
error:
  {
    gst_vaapi_encoder_unref (encoder);
    return NULL;
  }
}

static GstVaapiContext *
create_test_context_config (GstVaapiEncoder * encoder, GstVaapiProfile profile)
{
  GstVaapiContextInfo cip = { 0, };
  GstVaapiContext *ctxt;

  if (encoder->context)
    return gst_vaapi_object_ref (encoder->context);

  /* if there is no profile, let's figure out one */
  if (profile == GST_VAAPI_PROFILE_UNKNOWN)
    profile = get_profile (encoder);

  init_context_info (encoder, &cip, profile);
  ctxt = gst_vaapi_context_new (encoder->display, &cip);
  return ctxt;
}

static GArray *
get_profile_surface_formats (GstVaapiEncoder * encoder, GstVaapiProfile profile)
{
  GstVaapiContext *ctxt;
  GArray *formats;

  ctxt = create_test_context_config (encoder, profile);
  if (!ctxt)
    return NULL;
  formats = gst_vaapi_context_get_surface_formats (ctxt);
  gst_vaapi_object_unref (ctxt);
  return formats;
}

static gboolean
merge_profile_surface_formats (GstVaapiEncoder * encoder,
    GstVaapiProfile profile, GArray * formats)
{
  GArray *surface_fmts;
  guint i, j;
  GstVideoFormat fmt, sfmt;

  if (profile == GST_VAAPI_PROFILE_UNKNOWN)
    return FALSE;

  surface_fmts = get_profile_surface_formats (encoder, profile);
  if (!surface_fmts)
    return FALSE;

  for (i = 0; i < surface_fmts->len; i++) {
    sfmt = g_array_index (surface_fmts, GstVideoFormat, i);
    for (j = 0; j < formats->len; j++) {
      fmt = g_array_index (formats, GstVideoFormat, j);
      if (fmt == sfmt)
        break;
    }
    if (j >= formats->len)
      g_array_append_val (formats, sfmt);
  }

  g_array_unref (surface_fmts);
  return TRUE;
}

/**
 * gst_vaapi_encoder_get_surface_formats:
 * @encoder: a #GstVaapiEncoder instances
 *
 * Fetches the valid surface formats for the current VAConfig
 *
 * Returns: a #GArray of valid formats for the current VAConfig
 **/
GArray *
gst_vaapi_encoder_get_surface_formats (GstVaapiEncoder * encoder,
    GstVaapiProfile profile)
{
  const GstVaapiEncoderClassData *const cdata =
      GST_VAAPI_ENCODER_GET_CLASS (encoder)->class_data;
  GArray *profiles, *formats;
  guint i;

  if (profile || encoder->context)
    return get_profile_surface_formats (encoder, profile);

  /* no specific context neither specific profile, let's iterate among
   * the codec's profiles */
  profiles = gst_vaapi_display_get_encode_profiles (encoder->display);
  if (!profiles)
    return NULL;

  formats = g_array_new (FALSE, FALSE, sizeof (GstVideoFormat));
  for (i = 0; i < profiles->len; i++) {
    profile = g_array_index (profiles, GstVaapiProfile, i);
    if (gst_vaapi_profile_get_codec (profile) == cdata->codec) {
      if (!merge_profile_surface_formats (encoder, profile, formats)) {
        g_array_unref (formats);
        formats = NULL;
        break;
      }
    }
  }

  g_array_unref (profiles);

  return formats;
}

/**
 * gst_vaapi_encoder_ensure_num_slices:
 * @encoder: a #GstVaapiEncoder
 * @profile: a #GstVaapiProfile
 * @entrypoint: a #GstVaapiEntrypoint
 * @media_max_slices: the number of the slices permitted by the stream
 * @num_slices: (out): the possible number of slices to process
 *
 * This function will clamp the @num_slices provided by the user,
 * according the limit of the number of slices permitted by the stream
 * and by the hardware.
 *
 * We need to pass the @profile and the @entrypoint, because at the
 * moment the encoder base class, still doesn't have them assigned,
 * and this function is meant to be called by the derived classes
 * while they are configured.
 *
 * Returns: %TRUE if the number of slices is different than zero.
 **/
gboolean
gst_vaapi_encoder_ensure_num_slices (GstVaapiEncoder * encoder,
    GstVaapiProfile profile, GstVaapiEntrypoint entrypoint,
    guint media_max_slices, guint * num_slices)
{
  VAProfile va_profile;
  VAEntrypoint va_entrypoint;
  guint max_slices, num;

  va_profile = gst_vaapi_profile_get_va_profile (profile);
  va_entrypoint = gst_vaapi_entrypoint_get_va_entrypoint (entrypoint);

  if (!gst_vaapi_get_config_attribute (encoder->display, va_profile,
          va_entrypoint, VAConfigAttribEncMaxSlices, &max_slices)) {
    *num_slices = 1;
    return TRUE;
  }

  num = *num_slices;
  if (num > max_slices)
    num = max_slices;
  if (num > media_max_slices)
    num = media_max_slices;

  if (num == 0)
    return FALSE;
  *num_slices = num;
  return TRUE;
}

/**
 * gst_vaapi_encoder_ensure_max_num_ref_frames:
 * @encoder: a #GstVaapiEncoder
 * @profile: a #GstVaapiProfile
 * @entrypoint: a #GstVaapiEntrypoint
 *
 * This function will query VAConfigAttribEncMaxRefFrames to get the
 * maximum number of reference frames in the driver,
 * for both the reference picture list 0 (bottom 16 bits) and
 * the reference picture list 1 (top 16 bits).
 *
 * We need to pass the @profile and the @entrypoint, because at the
 * moment the encoder base class, still doesn't have them assigned,
 * and this function is meant to be called by the derived classes
 * while they are configured.
 *
 * Returns: %TRUE if the number of reference frames is different than zero.
 **/
gboolean
gst_vaapi_encoder_ensure_max_num_ref_frames (GstVaapiEncoder * encoder,
    GstVaapiProfile profile, GstVaapiEntrypoint entrypoint)
{
  VAProfile va_profile;
  VAEntrypoint va_entrypoint;
  guint max_ref_frames;

  va_profile = gst_vaapi_profile_get_va_profile (profile);
  va_entrypoint = gst_vaapi_entrypoint_get_va_entrypoint (entrypoint);

  if (!gst_vaapi_get_config_attribute (encoder->display, va_profile,
          va_entrypoint, VAConfigAttribEncMaxRefFrames, &max_ref_frames)) {
    /* Set the default the number of reference frames */
    encoder->max_num_ref_frames_0 = 1;
    encoder->max_num_ref_frames_1 = 0;
    return TRUE;
  }

  encoder->max_num_ref_frames_0 = max_ref_frames & 0xffff;
  encoder->max_num_ref_frames_1 = (max_ref_frames >> 16) & 0xffff;

  return TRUE;
}

/**
 * gst_vaapi_encoder_add_roi:
 * @encoder: a #GstVaapiEncoder
 * @roi: (transfer none): a #GstVaapiROI
 *
 * Adds a roi region provided by user.
 *
 * This can be called on running a pipeline,
 * Since vaapi encoder set roi regions at every frame encoding.
 * Note that if it exceeds number of supported roi in driver,
 * this will return FALSE.
 *
 * Return value: a #gboolean
 */
gboolean
gst_vaapi_encoder_add_roi (GstVaapiEncoder * encoder, GstVaapiROI * roi)
{
  GstVaapiContextInfo *const cip = &encoder->context_info;
  const GstVaapiConfigInfoEncoder *const config = &cip->config.encoder;
  GstVaapiROI *region = NULL;
  GList *walk;

  g_return_val_if_fail (roi != NULL, FALSE);

  if (!config->roi_capability)
    return FALSE;

  if (encoder->roi_regions &&
      g_list_length (encoder->roi_regions) > config->roi_num_supported)
    return FALSE;

  walk = encoder->roi_regions;
  while (walk) {
    GstVaapiROI *region_ptr = (GstVaapiROI *) walk->data;
    if (region_ptr->rect.x == roi->rect.x &&
        region_ptr->rect.y == roi->rect.y &&
        region_ptr->rect.width == roi->rect.width &&
        region_ptr->rect.height == roi->rect.height) {
      /* Duplicated region */
      goto end;
    }
    walk = walk->next;
  }

  region = g_malloc0 (sizeof (GstVaapiROI));
  if (G_UNLIKELY (!region))
    return FALSE;

  region->rect.x = roi->rect.x;
  region->rect.y = roi->rect.y;
  region->rect.width = roi->rect.width;
  region->rect.height = roi->rect.height;

  encoder->roi_regions = g_list_append (encoder->roi_regions, region);

end:
  return TRUE;
}

/**
 * gst_vaapi_encoder_del_roi:
 * @encoder: a #GstVaapiEncoder
 * @roi: (transfer none): a #GstVaapiROI
 *
 * Deletes a roi region provided by user.
 *
 * This can be called on running a pipeline,
 * Since vaapi encoder set roi regions at every frame encoding.
 *
 * Return value: a #gboolean
 */
gboolean
gst_vaapi_encoder_del_roi (GstVaapiEncoder * encoder, GstVaapiROI * roi)
{
  GstVaapiContextInfo *const cip = &encoder->context_info;
  const GstVaapiConfigInfoEncoder *const config = &cip->config.encoder;
  GList *walk;
  gboolean ret = FALSE;

  g_return_val_if_fail (roi != NULL, FALSE);

  if (!config->roi_capability)
    return FALSE;

  if (encoder->roi_regions && g_list_length (encoder->roi_regions) == 0)
    return FALSE;

  walk = encoder->roi_regions;
  while (walk) {
    GstVaapiROI *region = (GstVaapiROI *) walk->data;
    if (region->rect.x == roi->rect.x &&
        region->rect.y == roi->rect.y &&
        region->rect.width == roi->rect.width &&
        region->rect.height == roi->rect.height) {
      encoder->roi_regions = g_list_remove (encoder->roi_regions, region);
      g_free (region);
      ret = TRUE;
      break;
    }
    walk = walk->next;
  }

  return ret;
}

/** Returns a GType for the #GstVaapiEncoderTune set */
GType
gst_vaapi_encoder_tune_get_type (void)
{
  static volatile gsize g_type = 0;

  static const GEnumValue encoder_tune_values[] = {
    /* *INDENT-OFF* */
    { GST_VAAPI_ENCODER_TUNE_NONE,
      "None", "none" },
    { GST_VAAPI_ENCODER_TUNE_HIGH_COMPRESSION,
      "High compression", "high-compression" },
    { GST_VAAPI_ENCODER_TUNE_LOW_LATENCY,
      "Low latency", "low-latency" },
    { GST_VAAPI_ENCODER_TUNE_LOW_POWER,
      "Low power mode", "low-power" },
    { 0, NULL, NULL },
    /* *INDENT-ON* */
  };

  if (g_once_init_enter (&g_type)) {
    GType type =
        g_enum_register_static ("GstVaapiEncoderTune", encoder_tune_values);
    g_once_init_leave (&g_type, type);
  }
  return g_type;
}
