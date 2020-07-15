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
/* We keep a custom ringbuffer that is backed up by data allocated by
 * audio hal. We must also overide the commit function to write into
 * audio hal audio memory instead. */

#include <sys/time.h>

#define GST_TYPE_AMLRING_BUFFER        \
        (gst_amlringbuffer_get_type())
#define GST_AMLRING_BUFFER(obj)        \
        (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AMLRING_BUFFER,GstAmlRingBuffer))
#define GST_AML_BUFFER_CLASS(klass) \
        (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AMLRING_BUFFER,GstAmlRingBufferClass))
#define GST_AMLRING_BUFFER_GET_CLASS(obj) \
        (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_AMLRING_BUFFER, GstAmlRingBufferClass))
#define GST_AMLRING_BUFFER_CAST(obj)        \
        ((GstAmlRingBuffer *)obj)
#define GST_IS_AMLRING_BUFFER(obj)     \
        (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AMLRING_BUFFER))
#define GST_IS_AMLRING_BUFFER_CLASS(klass)\
        (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AMLRING_BUFFER))

typedef struct _GstAmlRingBuffer GstAmlRingBuffer;
typedef struct _GstAmlRingBufferClass GstAmlRingBufferClass;

#define is_raw_type(type) (type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW)
#define EXTEND_BUF_SIZE (4096*2*2)

struct _GstAmlRingBuffer
{
  GstAudioRingBuffer object;

  struct audio_stream_out *stream_;
  audio_format_t format_;
  uint32_t sr_;
  audio_channel_mask_t channel_mask_;

  GstAmlHalAsink *asink;
  gboolean paused_;
  gboolean flushed_;

  /* for bit stream */
  guint encoded_size;
  guint sample_per_frame;
  gboolean meta_parsed;
  guint frame_sent;
  struct timeval flush_time;
  gboolean extend_channel;
  uint8_t *extend_buf;
};

struct _GstAmlRingBufferClass
{
  GstAudioRingBufferClass parent_class;
};

static GType gst_amlringbuffer_get_type (void);
static void gst_amlringbuffer_finalize (GObject * object);

static gboolean gst_amlringbuffer_open_device (GstAudioRingBuffer * buf);
static gboolean gst_amlringbuffer_close_device (GstAudioRingBuffer * buf);
static gboolean gst_amlringbuffer_acquire (GstAudioRingBuffer * buf,
    GstAudioRingBufferSpec * spec);
static gboolean gst_amlringbuffer_release (GstAudioRingBuffer * buf);
static gboolean gst_amlringbuffer_start (GstAudioRingBuffer * buf);
static gboolean gst_amlringbuffer_pause (GstAudioRingBuffer * buf);
static gboolean gst_amlringbuffer_stop (GstAudioRingBuffer * buf);
static void gst_amlringbuffer_clear (GstAudioRingBuffer * buf);
static guint gst_amlringbuffer_commit (GstAudioRingBuffer * buf,
    guint64 * sample, guchar * data, gint in_samples, gint out_samples,
    gint * accum);

static GstAudioRingBufferClass *ring_parent_class = NULL;

G_DEFINE_TYPE (GstAmlRingBuffer, gst_amlringbuffer,
    GST_TYPE_AUDIO_RING_BUFFER);

static void
gst_amlringbuffer_class_init (GstAmlRingBufferClass * klass)
{
  GObjectClass *gobject_class;
  GstAudioRingBufferClass *gstringbuffer_class;

  gobject_class = (GObjectClass *) klass;
  gstringbuffer_class = (GstAudioRingBufferClass *) klass;

  ring_parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = gst_amlringbuffer_finalize;

  gstringbuffer_class->open_device =
      GST_DEBUG_FUNCPTR (gst_amlringbuffer_open_device);
  gstringbuffer_class->close_device =
      GST_DEBUG_FUNCPTR (gst_amlringbuffer_close_device);
  gstringbuffer_class->acquire =
      GST_DEBUG_FUNCPTR (gst_amlringbuffer_acquire);
  gstringbuffer_class->release =
      GST_DEBUG_FUNCPTR (gst_amlringbuffer_release);
  gstringbuffer_class->start = GST_DEBUG_FUNCPTR (gst_amlringbuffer_start);
  gstringbuffer_class->pause = GST_DEBUG_FUNCPTR (gst_amlringbuffer_pause);
  gstringbuffer_class->resume = GST_DEBUG_FUNCPTR (gst_amlringbuffer_start);
  gstringbuffer_class->stop = GST_DEBUG_FUNCPTR (gst_amlringbuffer_stop);
  gstringbuffer_class->clear_all =
      GST_DEBUG_FUNCPTR (gst_amlringbuffer_clear);

  gstringbuffer_class->commit = GST_DEBUG_FUNCPTR (gst_amlringbuffer_commit);
}

static void
gst_amlringbuffer_init (GstAmlRingBuffer * pbuf)
{
  pbuf->stream_ = NULL;
  pbuf->format_ = 0;
  pbuf->sr_ = 0;
  pbuf->channel_mask_ = 0;
  pbuf->encoded_size = 0;
  pbuf->frame_sent = 0;
  pbuf->flushed_ = FALSE;
  pbuf->extend_channel = FALSE;
  pbuf->extend_buf = NULL;
}

/* will be called when the device should be opened. In this case we will connect
 * to the server. We should not try to open any streams in this state. */
static gboolean
gst_amlringbuffer_open_device (GstAudioRingBuffer * buf)
{
  GstAmlRingBuffer *pbuf;

  pbuf = GST_AMLRING_BUFFER_CAST (buf);
  pbuf->asink = GST_AML_HAL_ASINK (GST_OBJECT_PARENT (pbuf));

  g_assert (pbuf->asink);
  return TRUE;
}

/* close the device */
static gboolean
gst_amlringbuffer_close_device (GstAudioRingBuffer * buf)
{
  GstAmlHalAsink *asink;
  GstAmlRingBuffer *pbuf;

  pbuf = GST_AMLRING_BUFFER_CAST (buf);
  asink = pbuf->asink;

  GST_LOG_OBJECT (asink, "closing device");

  if (pbuf->stream_)
    asink->hw_dev_->close_output_stream(asink->hw_dev_,
            pbuf->stream_);
  if (pbuf->extend_buf)
    g_free(pbuf->extend_buf);

  GST_LOG_OBJECT (asink, "closed device");
  return TRUE;
}

static void
gst_amlringbuffer_finalize (GObject * object)
{
  //GstAmlRingBuffer *ringbuffer = GST_AMLRING_BUFFER_CAST (object);
  G_OBJECT_CLASS (ring_parent_class)->finalize (object);
}

static gboolean
sink_parse_spec (GstAmlRingBuffer * pbuf, GstAudioRingBufferSpec * spec)
{
  GstAmlHalAsink *asink = pbuf->asink;
  gint channels;

  switch (spec->type) {
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW:
      switch (GST_AUDIO_INFO_FORMAT (&spec->info)) {
        case GST_AUDIO_FORMAT_S16LE:
          pbuf->format_ = AUDIO_FORMAT_PCM_16_BIT;
          break;
        default:
          goto error;
      }
      break;
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_AC3:
      pbuf->format_ = AUDIO_FORMAT_AC3;
      break;
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_EAC3:
      pbuf->format_ = AUDIO_FORMAT_E_AC3;
      break;
    default:
      goto error;

  }
  pbuf->sr_ = GST_AUDIO_INFO_RATE (&spec->info);
  channels = GST_AUDIO_INFO_CHANNELS (&spec->info);

  if (!asink->direct_mode_ && channels != 2) {
    GST_ERROR_OBJECT (asink, "unsupported channel number:%d", channels);
    goto error;
  }

  if (channels == 2)
    pbuf->channel_mask_ = AUDIO_CHANNEL_OUT_STEREO;
  else if (channels == 6)
    pbuf->channel_mask_ = AUDIO_CHANNEL_OUT_5POINT1;
  else if (channels == 8)
    pbuf->channel_mask_ = AUDIO_CHANNEL_OUT_7POINT1;
  else if (channels == 1) {
    pbuf->channel_mask_ = AUDIO_CHANNEL_OUT_STEREO;
    pbuf->extend_channel = TRUE;
    pbuf->extend_buf = g_malloc0(EXTEND_BUF_SIZE);
    if (!pbuf->extend_buf) {
      GST_DEBUG_OBJECT (asink, "oom");
      goto error;
    }
  } else {
    GST_ERROR_OBJECT (asink, "unsupported channel number:%d", channels);
    goto error;
  }
  GST_DEBUG_OBJECT (asink, "format:0x%x, sr:%d, ch:%d",
          pbuf->format_, pbuf->sr_, channels);
  GST_DEBUG_OBJECT (asink, "buffer_time:%lld, peroid_time:%lld",
          spec->buffer_time, spec->latency_time);

  return TRUE;

  /* ERRORS */
error:
    return FALSE;
}

/* prepare resources and state to operate with the given specs */
static gboolean
aml_open_output_stream (GstAmlRingBuffer * pbuf, GstAudioRingBufferSpec * spec)
{
  GstAmlHalAsink *asink = pbuf->asink;
  struct audio_config config;
  int ret;
  audio_output_flags_t flag;
  audio_devices_t device;

  GST_DEBUG_OBJECT (asink, "prepare");

  if (!sink_parse_spec (pbuf, spec))
    return FALSE;

  memset(&config, 0, sizeof(config));
  config.sample_rate = pbuf->sr_;
  config.channel_mask = pbuf->channel_mask_;
  config.format = pbuf->format_;

  if (asink->direct_mode_)
    flag = AUDIO_OUTPUT_FLAG_DIRECT;
  else
    flag = AUDIO_OUTPUT_FLAG_PRIMARY;

  if (asink->output_port_ == 0)
    device = AUDIO_DEVICE_OUT_SPEAKER;
  else if (asink->output_port_ == 1)
    device = AUDIO_DEVICE_OUT_HDMI;
  else if (asink->output_port_ == 2)
    device = AUDIO_DEVICE_OUT_HDMI_ARC;
  else if (asink->output_port_ == 3)
    device = AUDIO_DEVICE_OUT_SPDIF;
  else {
    GST_ERROR_OBJECT(asink, "invalid port:%d", asink->output_port_);
    return FALSE;
  }

  ret = asink->hw_dev_->open_output_stream(asink->hw_dev_,
          0, device,
          flag, &config,
          &pbuf->stream_, NULL);
  if (ret) {
    GST_ERROR_OBJECT(asink, "can not open output stream:%d", ret);
    return FALSE;
  }
  GST_DEBUG_OBJECT (asink, "prepare done");
  return TRUE;
}

/* This method should create a new stream of the given @spec. No playback should
 * start yet so we start in the corked state. */
static gboolean
gst_amlringbuffer_acquire (GstAudioRingBuffer * buf,
    GstAudioRingBufferSpec * spec)
{
  GstAmlRingBuffer *pbuf = GST_AMLRING_BUFFER_CAST (buf);
  GstAmlHalAsink *asink = pbuf->asink;

  if (!aml_open_output_stream (pbuf, spec))
      return FALSE;

  pbuf->paused_ = FALSE;
  /* TODO:: configure volume when we changed it, else we leave the default */
  GST_DEBUG_OBJECT(asink, "buffer required");
  return TRUE;
}

/* free the stream that we acquired before */
static gboolean
gst_amlringbuffer_release (GstAudioRingBuffer * buf)
{
  GstAmlRingBuffer *pbuf = GST_AMLRING_BUFFER_CAST (buf);
  GstAmlHalAsink *asink = pbuf->asink;

  GST_DEBUG_OBJECT (asink, "enter");
  if (pbuf->stream_) {
    asink->hw_dev_->close_output_stream(asink->hw_dev_, pbuf->stream_);
    pbuf->stream_ = NULL;
  }

  GST_DEBUG_OBJECT(asink, "buffer release");
  return TRUE;
}

/* start/resume playback ASAP, we don't uncork here but in the commit method */
static gboolean
gst_amlringbuffer_start (GstAudioRingBuffer * buf)
{
  GstAmlRingBuffer *pbuf = GST_AMLRING_BUFFER_CAST (buf);
  GstAmlHalAsink *asink = pbuf->asink;

  GST_DEBUG_OBJECT (asink, "enter");
  if (!pbuf->stream_) {
    GST_ERROR_OBJECT (asink, "null pointer");
    return FALSE;
  }

  if (pbuf->paused_) {
#if 0
    int ret;
    ret = pbuf->stream_->resume(pbuf->stream_);
    if (ret) {
      GST_ERROR_OBJECT (asink, "resume failure:%d", ret);
      return FALSE;
    }
#endif
    GST_DEBUG_OBJECT (asink, "resume");
    pbuf->paused_ = FALSE;
  }
  return TRUE;
}

/* pause/stop playback ASAP */
static gboolean
gst_amlringbuffer_pause (GstAudioRingBuffer * buf)
{
  GstAmlRingBuffer *pbuf = GST_AMLRING_BUFFER_CAST (buf);
  GstAmlHalAsink *asink = pbuf->asink;

  GST_DEBUG_OBJECT (asink, "enter");
  if (!pbuf->stream_) {
    GST_ERROR_OBJECT (asink, "null pointer");
    return FALSE;
  }

  if (pbuf->paused_) {
    GST_DEBUG_OBJECT (asink, "already in pause state");
    return TRUE;
  }

#if 0
  int ret;
  ret = pbuf->stream_->pause(pbuf->stream_);
  if (ret) {
    GST_ERROR_OBJECT (asink, "pause failure:%d", ret);
    return FALSE;
  }
#endif
  pbuf->paused_ = TRUE;
  GST_DEBUG_OBJECT (asink, "pause");
  return TRUE;
}

/* stop playback, we flush everything. */
static gboolean
gst_amlringbuffer_stop (GstAudioRingBuffer * buf)
{
  GstAmlRingBuffer *pbuf = GST_AMLRING_BUFFER_CAST (buf);
  GstAmlHalAsink *asink = pbuf->asink;
  int ret;

  GST_DEBUG_OBJECT (asink, "enter");
  if (!pbuf->stream_) {
    GST_ERROR_OBJECT (asink, "null pointer");
    return FALSE;
  }

  pbuf->stream_->pause(pbuf->stream_);

  ret = pbuf->stream_->flush(pbuf->stream_);
  if (ret) {
    GST_ERROR_OBJECT (asink, "pause failure:%d", ret);
    return FALSE;
  }
  GST_DEBUG_OBJECT (asink, "stop");
  return TRUE;
}

#define FLUSH_DATA_SIZE (32*1024)
static void
gst_amlringbuffer_clear (GstAudioRingBuffer * buf)
{
  GstAmlRingBuffer *pbuf = GST_AMLRING_BUFFER_CAST (buf);
  GstAmlHalAsink *asink = pbuf->asink;
  gboolean raw_data;

  GST_DEBUG_OBJECT (asink, "clear");
  if (!pbuf->stream_)
    return;

  raw_data = is_raw_type(buf->spec.type);
  if (raw_data) {
    /* for pcm padding enough 0 to flush the data out */
    gint total = FLUSH_DATA_SIZE;
    gint written = 0;

    guchar *data = (guchar *)g_malloc0(FLUSH_DATA_SIZE);
    if (!data) {
      GST_ERROR_OBJECT (asink, "oom");
      return;
    }
    memset(data, 0, FLUSH_DATA_SIZE);
    while (total) {
      written = pbuf->stream_->write(pbuf->stream_, data, total);
      total -= written;
    }
    g_free(data);
    GST_DEBUG_OBJECT (asink, "clear done");
  } else {
    GST_DEBUG_OBJECT (asink, "flush bitstream frame_sent:%d", pbuf->frame_sent);
  }
  pbuf->flushed_ = TRUE;
  gettimeofday(&pbuf->flush_time, NULL);
}

//static guint table_5_1[3] = {48000, 44100, 32000};
static guint table_5_13[38][4] = {
    {96, 69, 64, 32},
    {96, 70, 64, 32},
    {120, 87, 80, 40},
    {120, 88, 80, 40},
    {144, 104, 96, 48},
    {144, 105, 96, 48},
    {168, 121, 112, 56},
    {168, 122, 112, 56},
    {192, 139, 128, 64},
    {192, 140, 128, 64},
    {240, 174, 160, 80},
    {240, 175, 160, 80},
    {288, 208, 192, 96},
    {288, 209, 192, 96},
    {336, 243, 224, 112},
    {336, 244, 224, 112},
    {384, 278, 256, 128},
    {384, 279, 256, 128},
    {480, 348, 320, 160},
    {480, 349, 320, 160},
    {576, 417, 384, 192},
    {576, 418, 384, 192},
    {672, 487, 448, 224},
    {672, 488, 448, 224},
    {768, 557, 512, 256},
    {768, 558, 512, 256},
    {960, 696, 640, 320},
    {960, 697, 640, 320},
    {1152, 835, 768, 384},
    {1152, 836, 768, 384},
    {1344, 975, 896, 448},
    {1344, 976, 896, 448},
    {1536, 1114, 1024, 512},
    {1536, 1115, 1024, 512},
    {1728, 1253, 1152, 576},
    {1728, 1254, 1152, 576},
    {1920, 1393, 1280, 640},
    {1920, 1394, 1280, 640}
};

static int parse_bit_stream(GstAmlRingBuffer *pbuf,
         guchar * data, gint size)
{
  GstAudioRingBuffer* buf = GST_AUDIO_RING_BUFFER_CAST(pbuf);
  GstAudioRingBufferSpec * spec = &buf->spec;
  GstAmlHalAsink *asink = pbuf->asink;

  if (spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_AC3) {
    guint8 frmsizecod;
    guint8 fscod;

    if (size < 5) {
      return -1;
    }

    /* check sync word */
    if (data[0] != 0x0b || data[1] != 0x77)
      return -1;

    fscod = (data[4] >> 6);
    frmsizecod = data[4]&0x3F;

    GST_DEBUG_OBJECT (asink, "fscod:%d frmsizecod:%d", fscod, frmsizecod);
    if (fscod > 2)
        return -1;
    if (frmsizecod > 37)
        return -1;

    pbuf->encoded_size = table_5_13[frmsizecod][2 - fscod] * 2;
    pbuf->sample_per_frame = 1536;
    GST_DEBUG_OBJECT (asink, "encoded_size:%d", pbuf->encoded_size);
    return 0;
  } else if (spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_EAC3) {
    guint16 frmsizecod;
    guint8 fscod, fscod2;
    guint8 numblkscod;

    if (size < 5) {
      return -1;
    }

    /* check sync word */
    if (data[0] != 0x0b || data[1] != 0x77)
      return -1;

    fscod = (data[4] >> 6);
    frmsizecod = data[3] + ((data[2]&0x7) << 8) + 1;

    GST_DEBUG_OBJECT (asink, "fscod:%d frmsizecod:%d", fscod, frmsizecod);
    if (fscod > 3)
      return -1;
    if (frmsizecod > 2048)
      return -1;

    if (fscod == 3) {
      fscod2 = (data[4] >> 4) & 0x3;
      GST_DEBUG_OBJECT (asink, "fscod2:%d", fscod2);
      if (fscod2 == 0)
        pbuf->sr_ = 24000;
      else if (fscod2 == 1)
        pbuf->sr_ = 22050;
      else if (fscod2 == 2)
        pbuf->sr_ = 16000;
      else {
        return -1;
      }
      pbuf->sample_per_frame = 256*6;
    } else {
      numblkscod = (data[4] >> 4) & 0x3;
      GST_DEBUG_OBJECT (asink, "numblkscod:%d", numblkscod);
      if (numblkscod == 0)
        pbuf->sample_per_frame = 256;
      else if (numblkscod == 1)
        pbuf->sample_per_frame = 256 * 2;
      else if (numblkscod == 2)
        pbuf->sample_per_frame = 256 * 3;
      else if (numblkscod == 3)
        pbuf->sample_per_frame = 256 * 6;
    }
    pbuf->encoded_size = frmsizecod * 2;
    GST_DEBUG_OBJECT (asink, "encoded_size:%d spf:%d",
            pbuf->encoded_size, pbuf->sample_per_frame);
    return 0;

  }
  return -1;
}

/* our custom commit function because we write into the buffer of audio HAL
 * instead of keeping our own buffer */
static guint
gst_amlringbuffer_commit (GstAudioRingBuffer * buf, guint64 * sample,
    guchar * data, gint in_samples, gint out_samples, gint * accum)
{
  GstAmlRingBuffer *pbuf = GST_AMLRING_BUFFER_CAST (buf);
  GstAmlHalAsink *asink = pbuf->asink;
  guint bufsize, towrite;
  gint bpf;
  gboolean raw_data;
  guint offset = 0;

  if (in_samples < 0 || in_samples != out_samples) {
    /* don't support negative rate */
    GST_ERROR_OBJECT(asink, "unsupported use case %d/%d", in_samples, out_samples);
    return 0;
  }

  bpf = GST_AUDIO_INFO_BPF (&buf->spec.info);
  raw_data = is_raw_type(buf->spec.type);

  bufsize = towrite = in_samples * bpf;

  if (!raw_data && !pbuf->meta_parsed) {
    if (parse_bit_stream(pbuf, data, towrite)) {
      GST_WARNING_OBJECT(asink, "parse header info fails, discard %d bytes", bufsize);
      return out_samples;
    } else
      pbuf->meta_parsed = TRUE;
  }

  /* Frame aligned */
  if (!raw_data) {
    g_assert(pbuf->encoded_size);
    if (towrite % pbuf->encoded_size) {
      GST_ERROR_OBJECT(asink, "not frame aligned %d %d", towrite, pbuf->encoded_size);
      return 0;
    }
  }

  /* make sure the ringbuffer is started */
  if (G_UNLIKELY (g_atomic_int_get (&buf->state) !=
          GST_AUDIO_RING_BUFFER_STATE_STARTED)) {
    /* see if we are allowed to start it */
    if (G_UNLIKELY (g_atomic_int_get (&buf->may_start) == FALSE)) {
      GST_LOG_OBJECT (asink, "we can not start");
      return 0;
    }

    GST_DEBUG_OBJECT (buf, "start!");
    if (!gst_audio_ring_buffer_start (buf)) {
      GST_LOG_OBJECT (asink, "failed to start the ringbuffer");
      return 0;
    }
  }

  GST_LOG_OBJECT (asink, "entering commit");
  if (pbuf->paused_) {
    GST_WARNING_OBJECT (asink, "drop %d frame in pause state", bufsize/bpf);
    return bufsize/bpf;
  }

  /* handle 1 channel PCM */
  if (pbuf->extend_channel && raw_data) {
    uint8_t *from = data, *to = pbuf->extend_buf;
    int i;

    towrite *= 2;
    if (towrite > EXTEND_BUF_SIZE) {
      GST_ERROR_OBJECT (asink, "extend buff too small %d vs %d", towrite, EXTEND_BUF_SIZE);
      return 0;
    }
    for (i = 0 ; i < in_samples ; i++) {
      memcpy(to, from, bpf);
      to += bpf;
      memcpy(to, from, bpf);
      to += bpf;
      from += bpf;
    }
    data = pbuf->extend_buf;
    bpf *= 2;
  }

  while (towrite > 0) {
    int written;
    int cur_size;

    if (!raw_data)
      cur_size = pbuf->encoded_size;
    else
      cur_size = towrite;

    written = pbuf->stream_->write(pbuf->stream_, data + offset, cur_size);
    towrite -= written;
    offset += written;


    if (!raw_data) {
      *sample += pbuf->sample_per_frame;
      pbuf->frame_sent++;
    } else
      *sample += written/bpf;

    GST_LOG_OBJECT (asink,
        "write %d/%d left %d sample:%lld", written, cur_size, towrite, *sample);
  }

  return in_samples;
}

static GstClockTime
aml_ringbuffer_get_time(GstAmlRingBuffer *pbuf)
{
  int ret;
  uint64_t frames;
  gint64 delta = 0;
  struct timespec time;
  GstAmlHalAsink *asink = pbuf->asink;
  GstAudioRingBuffer * buf = GST_AUDIO_RING_BUFFER(pbuf);

  if (!pbuf->stream_) {
    GST_ERROR_OBJECT (asink, "null pointer");
    return GST_CLOCK_TIME_NONE;
  }

  ret = pbuf->stream_->get_presentation_position(pbuf->stream_,
          &frames, &time);

  if (ret) {
    GST_WARNING_OBJECT (asink, "get_presentation_position failure %d", ret);
    return GST_CLOCK_TIME_NONE;
  }

  if (pbuf->flushed_ && !is_raw_type(buf->spec.type)) {
    struct timeval cur;

    gettimeofday(&cur, NULL);
    delta = (cur.tv_sec - pbuf->flush_time.tv_sec) * 1000000 +
        cur.tv_usec - pbuf->flush_time.tv_usec;
    GST_DEBUG_OBJECT (asink, "flush time passed %lld", delta);
  }
  return gst_util_uint64_scale_int (frames, GST_SECOND, pbuf->sr_) + delta*1000;
}

