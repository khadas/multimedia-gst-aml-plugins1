/* GStreamer
 * Copyright (C) 2020 <song.zhao@amlogic.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstamlhalasink
 *
 * The amlhalasink element connects to Amlogic audio HAL service
 * and provide android like audio features.
 *
 * <refsect2>
 * |[
 * gst-launch-1.0 filesrc location=/data/1k_2c_48k.wav ! wavparse ! multiqueue ! amlhalasink
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/audio/gstaudiosink.h>
#include "gstamlhalasink.h"

#define DEFAULT_VOLUME          1.0
#define DEFAULT_MUTE            FALSE
#define MAX_VOLUME              1.0

GST_DEBUG_CATEGORY_STATIC (gst_aml_hal_asink_debug_category);
#define GST_CAT_DEFAULT gst_aml_hal_asink_debug_category

/* prototypes */


static void gst_aml_hal_asink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_aml_hal_asink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_aml_hal_asink_finalize (GObject * object);

static GstStateChangeReturn
gst_aml_hal_sink_change_state (GstElement * element, GstStateChange transition);
static gboolean gst_aml_hal_sink_event (GstBaseSink * sink, GstEvent * event);
static gboolean gst_aml_hal_sink_query (GstBaseSink * sink, GstQuery * query);

static gboolean gst_aml_hal_asink_open (GstAmlHalAsink* sink);
static gboolean gst_aml_hal_asink_close (GstAmlHalAsink* asink);

static GstClockTime
gst_aml_hal_sink_get_time (GstClock * clock, GstAudioBaseSink * sink);

enum
{
  PROP_0,
  PROP_DIRECT_MODE,
  PROP_OUTPUT_PORT,
  PROP_VOLUME,
  PROP_MUTE,
  PROP_LAST
};

#define COMMON_AUDIO_CAPS \
  "channels = (int) [ 1, MAX ], " \
  "rate = (int) [ 1, MAX ]"

/* pad templates */
static GstStaticPadTemplate gst_aml_hal_asink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
      "audio/x-raw,format=S16LE,rate=48000,"
      "channels=[1,32],layout=interleaved; "
      "audio/x-ac3, "
      COMMON_AUDIO_CAPS "; "
      "audio/x-eac3, "
      COMMON_AUDIO_CAPS "; "
      )
    );

#define GST_TYPE_AHAL_OUTPUT_PORT \
  (gst_ahal_output_port_get_type ())

static GType
gst_ahal_output_port_get_type (void)
{
  static GType ahal_output_port_type = 0;

  if (!ahal_output_port_type) {
    static const GEnumValue ahal_output_port[] = {
      {0, "Speaker", "speaker"},
      {1, "HDMI-Tx", "hdmitx"},
      {2, "HDMI ARC", "hdmi-arc"},
      {3, "SPDIF", "spdif"},
      {0, NULL, NULL},
    };

    ahal_output_port_type =
        g_enum_register_static ("AmlAsinkOutputPort", ahal_output_port);
  }

  return ahal_output_port_type;
}

#include "gstamlringbuffer.c"

/* class initialization */
#define gst_aml_hal_asink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstAmlHalAsink, gst_aml_hal_asink, GST_TYPE_AUDIO_BASE_SINK,
  GST_DEBUG_CATEGORY_INIT (gst_aml_hal_asink_debug_category, "amlhalasink", 0,
  "debug category for amlhalasink element");
  G_IMPLEMENT_INTERFACE (GST_TYPE_STREAM_VOLUME, NULL)
  );

static GstAudioRingBuffer *
gst_aml_hal_sink_create_ringbuffer (GstAudioBaseSink * sink)
{
  GstAudioRingBuffer *buffer;

  GST_DEBUG_OBJECT (sink, "creating ringbuffer");
  buffer = g_object_new (GST_TYPE_AMLRING_BUFFER, NULL);
  GST_DEBUG_OBJECT (sink, "created ringbuffer @%p", buffer);

  return buffer;
}

static void
gst_aml_hal_asink_class_init (GstAmlHalAsinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);
  GstBaseSinkClass *bc;
  GstAudioBaseSinkClass *gstaudiosink_class = GST_AUDIO_BASE_SINK_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS(klass),
      &gst_aml_hal_asink_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Amlogic audio HAL sink", "Sink/Audio", "gstream plugin to connect AML audio HAL",
      "song.zhao@amlogic.com");

  gobject_class->set_property = gst_aml_hal_asink_set_property;
  gobject_class->get_property = gst_aml_hal_asink_get_property;
  gobject_class->finalize = gst_aml_hal_asink_finalize;


  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_aml_hal_sink_event);
  gstbasesink_class->query = GST_DEBUG_FUNCPTR (gst_aml_hal_sink_query);

  /* restore the original basesink pull methods */
  bc = g_type_class_peek (GST_TYPE_BASE_SINK);
  gstbasesink_class->activate_pull = GST_DEBUG_FUNCPTR (bc->activate_pull);

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_aml_hal_sink_change_state);

  gstaudiosink_class->create_ringbuffer =
      GST_DEBUG_FUNCPTR (gst_aml_hal_sink_create_ringbuffer);
  /* not need to add 61937 header, audio hal will do it */
  //gstaudiosink_class->payload = GST_DEBUG_FUNCPTR (gst_aml_hal_sink_payload);

  g_object_class_install_property (gobject_class, PROP_OUTPUT_PORT,
      g_param_spec_enum ("output-port", "Output Port",
          "select active output port for audio",
          GST_TYPE_AHAL_OUTPUT_PORT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DIRECT_MODE,
      g_param_spec_boolean ("direct-mode", "Direct Mode",
          "Select this mode for main mixing port, unselect it for system sound mixing port",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_VOLUME,
      g_param_spec_double ("volume", "Volume",
          "Linear volume of this stream, 1.0=100%", 0.0, MAX_VOLUME,
          DEFAULT_VOLUME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_MUTE,
      g_param_spec_boolean ("mute", "Mute",
          "Mute state of this stream", DEFAULT_MUTE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_aml_hal_asink_init (GstAmlHalAsink *asink)
{
  asink->hw_dev_ = NULL;
  asink->output_port_ = 0;
  asink->direct_mode_ = TRUE;
  asink->volume_ = 1.0f;
  asink->mute_ = FALSE;

  /* override with a custom clock */
  if (GST_AUDIO_BASE_SINK (asink)->provided_clock)
    gst_object_unref (GST_AUDIO_BASE_SINK (asink)->provided_clock);

  GST_AUDIO_BASE_SINK (asink)->provided_clock =
      gst_audio_clock_new ("GstAmlSinkClock",
      (GstAudioClockGetTimeFunc) gst_aml_hal_sink_get_time, asink, NULL);

  if (!gst_aml_hal_asink_open (asink))
    GST_ERROR_OBJECT(asink, "asink open failure");
}

static gdouble
gst_aml_hal_sink_get_volume (GstAmlHalAsink * asink)
{
  GST_LOG_OBJECT(asink, "return vol %f", asink->volume_);
  return asink->volume_;
}


static void
gst_aml_hal_sink_set_volume (GstAmlHalAsink * asink, gdouble volume)
{
  struct audio_port_config config;
  int ret;
  GST_DEBUG_OBJECT (asink, "set vol:%f", volume);

  volume  = gst_stream_volume_convert_volume (GST_STREAM_VOLUME_FORMAT_LINEAR,
          GST_STREAM_VOLUME_FORMAT_DB, volume);

  memset(&config, 0, sizeof(config));
  config.id = 2;
  config.role = AUDIO_PORT_ROLE_SINK;
  config.type = AUDIO_PORT_TYPE_DEVICE;
  config.config_mask = AUDIO_PORT_CONFIG_GAIN;
  /* audio_hal use dB * 100 to keep the accuracy */
  config.gain.values[0] = volume * 100;

  ret = asink->hw_dev_->set_audio_port_config(asink->hw_dev_, &config);
  if (ret) {
    GST_ERROR_OBJECT(asink, "port_config faile:%d",ret);
  } else {
    GST_LOG_OBJECT(asink, "hal volume set to %f", volume);
    asink->volume_ = volume;
  }
}

static void
gst_aml_hal_sink_set_mute (GstAmlHalAsink * asink, gboolean mute)
{
  int ret;

  GST_DEBUG_OBJECT (asink, "set mute:%d", mute);
  //TODO: implement in audio hal, mute the amplifier
  ret = asink->hw_dev_->set_master_mute( asink->hw_dev_, mute);
  if (ret)
    GST_ERROR_OBJECT(asink, "mute fail:%d", ret);
  else
    asink->mute_ = mute;
}

static void
gst_aml_hal_asink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAmlHalAsink *asink = GST_AML_HAL_ASINK (object);

  switch (property_id) {
    case PROP_DIRECT_MODE:
      asink->direct_mode_ = g_value_get_boolean(value);
      GST_DEBUG_OBJECT (asink, "set direct mode:%d", asink->direct_mode_);
      break;
    case PROP_OUTPUT_PORT:
      asink->output_port_ = g_value_get_enum (value);
      GST_DEBUG_OBJECT (asink, "set output port:%d", asink->output_port_);
      break;
    case PROP_VOLUME:
      gst_aml_hal_sink_set_volume (asink, g_value_get_double (value));
      break;
    case PROP_MUTE:
      gst_aml_hal_sink_set_mute (asink, g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_aml_hal_asink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstAmlHalAsink *asink = GST_AML_HAL_ASINK (object);

  switch (property_id) {
    case PROP_DIRECT_MODE:
      g_value_set_boolean(value, asink->direct_mode_);
      GST_DEBUG_OBJECT (asink, "get direct mode:%d", asink->direct_mode_);
      break;
    case PROP_OUTPUT_PORT:
      g_value_set_enum(value, asink->output_port_);
      GST_DEBUG_OBJECT (asink, "get output port:%d", asink->output_port_);
      break;
    case PROP_VOLUME:
      g_value_set_double (value, gst_aml_hal_sink_get_volume(asink));
      break;
    case PROP_MUTE:
      g_value_set_boolean (value, asink->mute_);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_aml_hal_asink_finalize (GObject * object)
{
  GstAmlHalAsink *asink = GST_AML_HAL_ASINK (object);

  GST_DEBUG_OBJECT (asink, "finalize");

  /* clean up object here */
  gst_aml_hal_asink_close(asink);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_aml_hal_sink_event (GstBaseSink * sink, GstEvent * event)
{
  GstAudioBaseSink* bs = GST_AUDIO_BASE_SINK_CAST(sink);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_GAP:{
      GstClockTime timestamp, duration;

      if (!bs->ringbuffer)
        break;
      gst_event_parse_gap (event, &timestamp, &duration);
      if (duration == GST_CLOCK_TIME_NONE)
        gst_amlringbuffer_clear(bs->ringbuffer);
      break;
    }
    case GST_EVENT_EOS:
      if (!bs->ringbuffer)
        break;
      gst_amlringbuffer_clear(bs->ringbuffer);
      break;
    default:
      ;
  }

  return GST_BASE_SINK_CLASS (parent_class)->event (sink, event);
}

static gboolean
gst_aml_hal_sink_query_acceptcaps (GstAmlHalAsink * asink, GstCaps * caps)
{
  GstCaps *pad_caps;
  gboolean ret;

  pad_caps = gst_pad_get_pad_template_caps (GST_BASE_SINK_PAD (asink));
  ret = gst_caps_is_subset (caps, pad_caps);
  gst_caps_unref (pad_caps);

  GST_DEBUG_OBJECT (asink, "caps %" GST_PTR_FORMAT, caps);

  /* Template caps didn't match */
  if (!ret)
    return FALSE;

  return TRUE;
}

static gboolean
gst_aml_hal_sink_query (GstBaseSink * sink, GstQuery * query)
{
  GstAmlHalAsink *asink = GST_AML_HAL_ASINK (sink);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *caps, *filter;

      gst_query_parse_caps (query, &filter);
      caps = gst_pad_get_pad_template_caps (GST_AUDIO_BASE_SINK_PAD (asink));

      if (caps) {
        gst_query_set_caps_result (query, caps);
        gst_caps_unref (caps);
        ret = TRUE;
      }
      break;
    }
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps;

      gst_query_parse_accept_caps (query, &caps);
      ret = gst_aml_hal_sink_query_acceptcaps (asink, caps);
      gst_query_set_accept_caps_result (query, ret);
      ret = TRUE;
      break;
    }
    default:
      ret = GST_BASE_SINK_CLASS (parent_class)->query (sink, query);
      break;
  }
  return ret;
}

/* open the device with given specs */
static gboolean
gst_aml_hal_asink_open (GstAmlHalAsink* asink)
{
  int ret;

  GST_DEBUG_OBJECT (asink, "open");
  ret = audio_hw_load_interface(&asink->hw_dev_);
  if (ret) {
    GST_ERROR_OBJECT(asink, "fail to load hw:%d", ret);
    return FALSE;
  }
  GST_DEBUG_OBJECT (asink, "load hw done");

  asink->source_.id = 1;
  asink->source_.role = AUDIO_PORT_ROLE_SOURCE;
  asink->source_.type = AUDIO_PORT_TYPE_MIX;
  asink->source_.config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE |
      AUDIO_PORT_CONFIG_FORMAT;
  asink->source_.sample_rate = 48000;
  asink->source_.format = AUDIO_FORMAT_PCM_16_BIT;

  asink->sink_.id = 2;
  asink->sink_.role = AUDIO_PORT_ROLE_SINK;
  asink->sink_.type = AUDIO_PORT_TYPE_DEVICE;
  asink->sink_.config_mask = AUDIO_PORT_CONFIG_SAMPLE_RATE |
      AUDIO_PORT_CONFIG_FORMAT;
  asink->sink_.sample_rate = 48000;
  asink->sink_.format = AUDIO_FORMAT_PCM_16_BIT;
  asink->sink_.ext.device.type = AUDIO_DEVICE_OUT_SPEAKER;

  GST_DEBUG_OBJECT(asink, "create mix --> speaker patch...");
  ret = asink->hw_dev_->create_audio_patch(asink->hw_dev_,
          1, &asink->source_,
          1, &asink->sink_,
          &asink->patch_h_);
  if (ret)
    GST_ERROR_OBJECT(asink, "patch fail ret:%d",ret);
  else
    GST_DEBUG_OBJECT(asink, "success");

  return TRUE;
}

static gboolean
gst_aml_hal_asink_close (GstAmlHalAsink* asink)
{
  int ret;
  GST_DEBUG_OBJECT(asink, "close");
  if (asink->patch_h_) {
    ret = asink->hw_dev_->release_audio_patch(asink->hw_dev_, asink->patch_h_);
    if (ret)
      GST_ERROR_OBJECT(asink, "destroy patch fail ret:%d",ret);
    asink->patch_h_ = 0;
    GST_DEBUG_OBJECT(asink, "patch destroyed");
  }
  audio_hw_unload_interface(asink->hw_dev_);
  GST_DEBUG_OBJECT(asink, "unload hw");
  return TRUE;
}

static GstStateChangeReturn
gst_aml_hal_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstAmlHalAsink *asink = GST_AML_HAL_ASINK (element);

  GST_LOG_OBJECT(asink, "start");
  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_element_post_message (element,
          gst_message_new_clock_provide (GST_OBJECT_CAST (element),
              GST_AUDIO_BASE_SINK (element)->provided_clock, TRUE));
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      /* format_lost is reset in release() in audiobasesink */
      gst_element_post_message (element,
          gst_message_new_clock_lost (GST_OBJECT_CAST (element),
              GST_AUDIO_BASE_SINK (element)->provided_clock));
      break;
    default:
      break;
  }

  GST_LOG_OBJECT(asink, "done");
  return ret;
}

/* Returns the current time of the sink ringbuffer. The timing_info is updated
 * on every data write/flush and every 100ms (PA_STREAM_AUTO_TIMING_UPDATE).
 */
static GstClockTime
gst_aml_hal_sink_get_time (GstClock * clock, GstAudioBaseSink * sink)
{
  GstAmlHalAsink *asink = GST_AML_HAL_ASINK (sink);
  GstAmlRingBuffer *pbuf = GST_AMLRING_BUFFER_CAST (sink->ringbuffer);
  GstClockTime ret;

  if (!sink->ringbuffer || !sink->ringbuffer->acquired)
    return GST_CLOCK_TIME_NONE;

  ret = aml_ringbuffer_get_time(pbuf);

  GST_LOG_OBJECT (asink, "current time is %" GST_TIME_FORMAT,
      GST_TIME_ARGS (ret));
  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "amlhalasink", GST_RANK_PRIMARY,
      GST_TYPE_AML_HAL_ASINK);
}

#ifndef VERSION
#define VERSION "0.0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "aml_package"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "aml_media"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "http://amlogic.com"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    amlhalasink,
    "Amlogic plugin for connecting to audio server",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
