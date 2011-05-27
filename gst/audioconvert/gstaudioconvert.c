/* GStreamer
 * Copyright (C) 2003 Benjamin Otte <in7y118@public.uni-hamburg.de>
 * Copyright (C) 2005 Thomas Vander Stichele <thomas at apestaart dot org>
 * Copyright (C) 2005 Wim Taymans <wim at fluendo dot com>
 *
 * gstaudioconvert.c: Convert audio to different audio formats automatically
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-audioconvert
 *
 * Audioconvert converts raw audio buffers between various possible formats.
 * It supports integer to float conversion, width/depth conversion,
 * signedness and endianness conversion and channel transformations.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m audiotestsrc ! audioconvert ! audio/x-raw-int,channels=2,width=8,depth=8 ! level ! fakesink silent=TRUE
 * ]| This pipeline converts audio to 8-bit.  The level element shows that
 * the output levels still match the one for a sine wave.
 * |[
 * gst-launch -v -m audiotestsrc ! audioconvert ! vorbisenc ! fakesink silent=TRUE
 * ]| The vorbis encoder takes float audio data instead of the integer data
 * generated by audiotestsrc.
 * </refsect2>
 *
 * Last reviewed on 2006-03-02 (0.10.4)
 */

/*
 * design decisions:
 * - audioconvert converts buffers in a set of supported caps. If it supports
 *   a caps, it supports conversion from these caps to any other caps it
 *   supports. (example: if it does A=>B and A=>C, it also does B=>C)
 * - audioconvert does not save state between buffers. Every incoming buffer is
 *   converted and the converted buffer is pushed out.
 * conclusion:
 * audioconvert is not supposed to be a one-element-does-anything solution for
 * audio conversions.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstaudioconvert.h"
#include "gstchannelmix.h"
#include "gstaudioquantize.h"
#include "plugin.h"

GST_DEBUG_CATEGORY (audio_convert_debug);
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);

/*** DEFINITIONS **************************************************************/

/* type functions */
static void gst_audio_convert_dispose (GObject * obj);

/* gstreamer functions */
static gboolean gst_audio_convert_get_unit_size (GstBaseTransform * base,
    GstCaps * caps, gsize * size);
static GstCaps *gst_audio_convert_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static void gst_audio_convert_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static gboolean gst_audio_convert_set_caps (GstBaseTransform * base,
    GstCaps * incaps, GstCaps * outcaps);
static GstFlowReturn gst_audio_convert_transform (GstBaseTransform * base,
    GstBuffer * inbuf, GstBuffer * outbuf);
static GstFlowReturn gst_audio_convert_transform_ip (GstBaseTransform * base,
    GstBuffer * buf);
static void gst_audio_convert_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_audio_convert_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean structure_has_fixed_channel_positions (GstStructure * s,
    gboolean * unpositioned_layout);

/* AudioConvert signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_DITHERING,
  ARG_NOISE_SHAPING,
};

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (audio_convert_debug, "audioconvert", 0, "audio conversion element"); \
  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");
#define gst_audio_convert_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstAudioConvert, gst_audio_convert,
    GST_TYPE_BASE_TRANSFORM, DEBUG_INIT);

/*** GSTREAMER PROTOTYPES *****************************************************/

#define STATIC_CAPS \
GST_STATIC_CAPS ( \
  "audio/x-raw-float, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, MAX ], " \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, " \
    "width = (int) 64;" \
  "audio/x-raw-float, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, MAX ], " \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, " \
    "width = (int) 32;" \
  "audio/x-raw-int, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, MAX ], " \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, " \
    "width = (int) 32, " \
    "depth = (int) [ 1, 32 ], " \
    "signed = (boolean) { true, false }; " \
  "audio/x-raw-int, "   \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, MAX ], "       \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, "        \
    "width = (int) 24, "        \
    "depth = (int) [ 1, 24 ], " "signed = (boolean) { true, false }; "  \
  "audio/x-raw-int, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, MAX ], " \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, " \
    "width = (int) 16, " \
    "depth = (int) [ 1, 16 ], " \
    "signed = (boolean) { true, false }; " \
  "audio/x-raw-int, " \
    "rate = (int) [ 1, MAX ], " \
    "channels = (int) [ 1, MAX ], " \
    "endianness = (int) { LITTLE_ENDIAN, BIG_ENDIAN }, " \
    "width = (int) 8, " \
    "depth = (int) [ 1, 8 ], " \
    "signed = (boolean) { true, false } " \
)

static GstStaticPadTemplate gst_audio_convert_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    STATIC_CAPS);

static GstStaticPadTemplate gst_audio_convert_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    STATIC_CAPS);

#define GST_TYPE_AUDIO_CONVERT_DITHERING (gst_audio_convert_dithering_get_type ())
static GType
gst_audio_convert_dithering_get_type (void)
{
  static GType gtype = 0;

  if (gtype == 0) {
    static const GEnumValue values[] = {
      {DITHER_NONE, "No dithering",
          "none"},
      {DITHER_RPDF, "Rectangular dithering", "rpdf"},
      {DITHER_TPDF, "Triangular dithering (default)", "tpdf"},
      {DITHER_TPDF_HF, "High frequency triangular dithering", "tpdf-hf"},
      {0, NULL, NULL}
    };

    gtype = g_enum_register_static ("GstAudioConvertDithering", values);
  }
  return gtype;
}

#define GST_TYPE_AUDIO_CONVERT_NOISE_SHAPING (gst_audio_convert_ns_get_type ())
static GType
gst_audio_convert_ns_get_type (void)
{
  static GType gtype = 0;

  if (gtype == 0) {
    static const GEnumValue values[] = {
      {NOISE_SHAPING_NONE, "No noise shaping (default)",
          "none"},
      {NOISE_SHAPING_ERROR_FEEDBACK, "Error feedback", "error-feedback"},
      {NOISE_SHAPING_SIMPLE, "Simple 2-pole noise shaping", "simple"},
      {NOISE_SHAPING_MEDIUM, "Medium 5-pole noise shaping", "medium"},
      {NOISE_SHAPING_HIGH, "High 8-pole noise shaping", "high"},
      {0, NULL, NULL}
    };

    gtype = g_enum_register_static ("GstAudioConvertNoiseShaping", values);
  }
  return gtype;
}


/*** TYPE FUNCTIONS ***********************************************************/
static void
gst_audio_convert_class_init (GstAudioConvertClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *basetransform_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->dispose = gst_audio_convert_dispose;
  gobject_class->set_property = gst_audio_convert_set_property;
  gobject_class->get_property = gst_audio_convert_get_property;

  g_object_class_install_property (gobject_class, ARG_DITHERING,
      g_param_spec_enum ("dithering", "Dithering",
          "Selects between different dithering methods.",
          GST_TYPE_AUDIO_CONVERT_DITHERING, DITHER_TPDF,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, ARG_NOISE_SHAPING,
      g_param_spec_enum ("noise-shaping", "Noise shaping",
          "Selects between different noise shaping methods.",
          GST_TYPE_AUDIO_CONVERT_NOISE_SHAPING, NOISE_SHAPING_NONE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_audio_convert_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_audio_convert_sink_template));
  gst_element_class_set_details_simple (element_class,
      "Audio converter", "Filter/Converter/Audio",
      "Convert audio to different formats", "Benjamin Otte <otte@gnome.org>");

  basetransform_class->get_unit_size =
      GST_DEBUG_FUNCPTR (gst_audio_convert_get_unit_size);
  basetransform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform_caps);
  basetransform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_fixate_caps);
  basetransform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_set_caps);
  basetransform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform_ip);
  basetransform_class->transform =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform);

  basetransform_class->passthrough_on_same_caps = TRUE;
}

static void
gst_audio_convert_init (GstAudioConvert * this)
{
  this->dither = DITHER_TPDF;
  this->ns = NOISE_SHAPING_NONE;
  memset (&this->ctx, 0, sizeof (AudioConvertCtx));

  gst_base_transform_set_gap_aware (GST_BASE_TRANSFORM (this), TRUE);
}

static void
gst_audio_convert_dispose (GObject * obj)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (obj);

  audio_convert_clean_context (&this->ctx);

  G_OBJECT_CLASS (parent_class)->dispose (obj);
}

/*** GSTREAMER FUNCTIONS ******************************************************/

/* convert the given GstCaps to our format */
static gboolean
gst_audio_convert_parse_caps (const GstCaps * caps, AudioConvertFmt * fmt)
{
  GstStructure *structure = gst_caps_get_structure (caps, 0);

  GST_DEBUG ("parse caps %p and %" GST_PTR_FORMAT, caps, caps);

  g_return_val_if_fail (gst_caps_is_fixed (caps), FALSE);
  g_return_val_if_fail (fmt != NULL, FALSE);

  /* cleanup old */
  audio_convert_clean_fmt (fmt);

  fmt->endianness = G_BYTE_ORDER;
  fmt->is_int =
      (strcmp (gst_structure_get_name (structure), "audio/x-raw-int") == 0);

  /* parse common fields */
  if (!gst_structure_get_int (structure, "channels", &fmt->channels))
    goto no_values;
  if (!(fmt->pos = gst_audio_get_channel_positions (structure)))
    goto no_values;

  fmt->unpositioned_layout = FALSE;
  structure_has_fixed_channel_positions (structure, &fmt->unpositioned_layout);

  if (!gst_structure_get_int (structure, "width", &fmt->width))
    goto no_values;
  if (!gst_structure_get_int (structure, "rate", &fmt->rate))
    goto no_values;
  /* width != 8 needs an endianness field */
  if (fmt->width != 8) {
    if (!gst_structure_get_int (structure, "endianness", &fmt->endianness))
      goto no_values;
  }

  if (fmt->is_int) {
    /* int specific fields */
    if (!gst_structure_get_boolean (structure, "signed", &fmt->sign))
      goto no_values;
    if (!gst_structure_get_int (structure, "depth", &fmt->depth))
      goto no_values;

    /* depth cannot be bigger than the width */
    if (fmt->depth > fmt->width)
      goto not_allowed;
  }

  fmt->unit_size = (fmt->width * fmt->channels) / 8;

  return TRUE;

  /* ERRORS */
no_values:
  {
    GST_DEBUG ("could not get some values from structure");
    audio_convert_clean_fmt (fmt);
    return FALSE;
  }
not_allowed:
  {
    GST_DEBUG ("width > depth, not allowed - make us advertise correct fmt");
    audio_convert_clean_fmt (fmt);
    return FALSE;
  }
}

/* BaseTransform vmethods */
static gboolean
gst_audio_convert_get_unit_size (GstBaseTransform * base, GstCaps * caps,
    gsize * size)
{
  AudioConvertFmt fmt = { 0 };

  g_assert (size);

  if (!gst_audio_convert_parse_caps (caps, &fmt))
    goto parse_error;

  GST_INFO_OBJECT (base, "unit_size = %u", fmt.unit_size);
  *size = fmt.unit_size;

  audio_convert_clean_fmt (&fmt);

  return TRUE;

parse_error:
  {
    GST_INFO_OBJECT (base, "failed to parse caps to get unit_size");
    return FALSE;
  }
}

/* Set widths (a list); multiples of 8 between min and max */
static void
set_structure_widths (GstStructure * s, int min, int max)
{
  GValue list = { 0 };
  GValue val = { 0 };
  int width;

  if (min == max) {
    gst_structure_set (s, "width", G_TYPE_INT, min, NULL);
    return;
  }

  g_value_init (&list, GST_TYPE_LIST);
  g_value_init (&val, G_TYPE_INT);
  for (width = min; width <= max; width += 8) {
    g_value_set_int (&val, width);
    gst_value_list_append_value (&list, &val);
  }
  gst_structure_set_value (s, "width", &list);
  g_value_unset (&val);
  g_value_unset (&list);
}

/* Set widths of 32 bits and 64 bits (as list) */
static void
set_structure_widths_32_and_64 (GstStructure * s)
{
  GValue list = { 0 };
  GValue val = { 0 };

  g_value_init (&list, GST_TYPE_LIST);
  g_value_init (&val, G_TYPE_INT);
  g_value_set_int (&val, 32);
  gst_value_list_append_value (&list, &val);
  g_value_set_int (&val, 64);
  gst_value_list_append_value (&list, &val);
  gst_structure_set_value (s, "width", &list);
  g_value_unset (&val);
  g_value_unset (&list);
}

/* Modify the structure so that things that must always have a single
 * value (for float), or can always be losslessly converted (for int), have
 * appropriate values.
 */
static GstStructure *
make_lossless_changes (GstStructure * s, gboolean isfloat)
{
  GValue list = { 0 };
  GValue val = { 0 };
  int i;
  const gint endian[] = { G_LITTLE_ENDIAN, G_BIG_ENDIAN };
  const gboolean booleans[] = { TRUE, FALSE };

  g_value_init (&list, GST_TYPE_LIST);
  g_value_init (&val, G_TYPE_INT);
  for (i = 0; i < 2; i++) {
    g_value_set_int (&val, endian[i]);
    gst_value_list_append_value (&list, &val);
  }
  gst_structure_set_value (s, "endianness", &list);
  g_value_unset (&val);
  g_value_unset (&list);

  if (isfloat) {
    /* float doesn't have a depth or signedness field and only supports
     * widths of 32 and 64 bits */
    gst_structure_remove_field (s, "depth");
    gst_structure_remove_field (s, "signed");
    set_structure_widths_32_and_64 (s);
  } else {
    /* int supports signed and unsigned. GValues are a pain */
    g_value_init (&list, GST_TYPE_LIST);
    g_value_init (&val, G_TYPE_BOOLEAN);
    for (i = 0; i < 2; i++) {
      g_value_set_boolean (&val, booleans[i]);
      gst_value_list_append_value (&list, &val);
    }
    gst_structure_set_value (s, "signed", &list);
    g_value_unset (&val);
    g_value_unset (&list);
  }

  return s;
}

static void
strip_width_64 (GstStructure * s)
{
  const GValue *v = gst_structure_get_value (s, "width");
  GValue widths = { 0 };

  if (GST_VALUE_HOLDS_LIST (v)) {
    int i;
    int len = gst_value_list_get_size (v);

    g_value_init (&widths, GST_TYPE_LIST);

    for (i = 0; i < len; i++) {
      const GValue *width = gst_value_list_get_value (v, i);

      if (g_value_get_int (width) != 64)
        gst_value_list_append_value (&widths, width);
    }
    gst_structure_set_value (s, "width", &widths);
    g_value_unset (&widths);
  }
}

/* Little utility function to create a related structure for float/int */
static void
append_with_other_format (GstCaps * caps, const GstStructure * s,
    gboolean isfloat)
{
  GstStructure *s2;

  if (isfloat) {
    s2 = gst_structure_copy (s);
    gst_structure_set_name (s2, "audio/x-raw-int");
    make_lossless_changes (s2, FALSE);
    /* If 64 bit float was allowed; remove width 64: we don't support it for 
     * integer*/
    strip_width_64 (s2);
    gst_caps_merge_structure (caps, s2);
  } else {
    s2 = gst_structure_copy (s);
    gst_structure_set_name (s2, "audio/x-raw-float");
    make_lossless_changes (s2, TRUE);
    gst_caps_merge_structure (caps, s2);
  }
}

static gboolean
structure_has_fixed_channel_positions (GstStructure * s,
    gboolean * unpositioned_layout)
{
  GstAudioChannelPosition *pos;
  const GValue *val;
  gint channels = 0;

  if (!gst_structure_get_int (s, "channels", &channels))
    return FALSE;               /* probably a range */

  val = gst_structure_get_value (s, "channel-positions");
  if ((val == NULL || !gst_value_is_fixed (val)) && channels <= 8) {
    GST_LOG ("no or unfixed channel-positions in %" GST_PTR_FORMAT, s);
    return FALSE;
  } else if (val == NULL || !gst_value_is_fixed (val)) {
    GST_LOG ("implicit undefined channel-positions");
    *unpositioned_layout = TRUE;
    return TRUE;
  }

  pos = gst_audio_get_channel_positions (s);
  if (pos && pos[0] == GST_AUDIO_CHANNEL_POSITION_NONE) {
    GST_LOG ("fixed undefined channel-positions in %" GST_PTR_FORMAT, s);
    *unpositioned_layout = TRUE;
  } else {
    GST_LOG ("fixed defined channel-positions in %" GST_PTR_FORMAT, s);
    *unpositioned_layout = FALSE;
  }
  g_free (pos);

  return TRUE;
}

/* Audioconvert can perform all conversions on audio except for resampling. 
 * However, there are some conversions we _prefer_ not to do. For example, it's
 * better to convert format (float<->int, endianness, etc) than the number of
 * channels, as the latter conversion is not lossless.
 *
 * So, we return, in order (assuming input caps have only one structure; 
 * which is enforced by basetransform):
 *  - input caps with a different format (lossless conversions).
 *  - input caps with a different format (slightly lossy conversions).
 *  - input caps with a different number of channels (very lossy!)
 */
static GstCaps *
gst_audio_convert_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *ret;
  GstStructure *s, *structure;
  gboolean isfloat, allow_mixing;
  gint width, depth, channels = 0;
  const gchar *fields_used[] = {
    "width", "depth", "rate", "channels", "endianness", "signed"
  };
  const gchar *structure_name;
  gint n, j;
  int i;

  n = gst_caps_get_size (caps);

  ret = gst_caps_new_empty ();

  for (j = 0; j < n; j++) {
    structure = gst_caps_get_structure (caps, j);

    if (j > 0) {
      /* If the new structure is a subset of the already existing transformed
       * caps we can safely skip it because we would transform it to the
       * same caps again.
       */
      if (gst_caps_is_subset_structure (ret, structure))
        continue;
    }

    structure_name = gst_structure_get_name (structure);

    isfloat = strcmp (structure_name, "audio/x-raw-float") == 0;

    /* We operate on a version of the original structure with any additional
     * fields absent */
    s = gst_structure_empty_new (structure_name);
    for (i = 0; i < sizeof (fields_used) / sizeof (*fields_used); i++) {
      if (gst_structure_has_field (structure, fields_used[i]))
        gst_structure_set_value (s, fields_used[i],
            gst_structure_get_value (structure, fields_used[i]));
    }

    if (!isfloat) {
      /* Commonly, depth is left out: set it equal to width if we have a fixed
       * width, if so */
      if (!gst_structure_has_field (s, "depth") &&
          gst_structure_get_int (s, "width", &width))
        gst_structure_set (s, "depth", G_TYPE_INT, width, NULL);
    }

    /* All lossless conversions */
    s = make_lossless_changes (s, isfloat);
    gst_caps_merge_structure (ret, gst_structure_copy (s));

    /* Same, plus a float<->int conversion */
    append_with_other_format (ret, s, isfloat);
    GST_DEBUG_OBJECT (base, "  step1: (%d) %" GST_PTR_FORMAT,
        gst_caps_get_size (ret), ret);

    /* We don't mind increasing width/depth/channels, but reducing them is 
     * Very Bad. Only available if width, depth, channels are already fixed. */
    if (!isfloat) {
      if (gst_structure_get_int (structure, "width", &width))
        set_structure_widths (s, width, 32);
      if (gst_structure_get_int (structure, "depth", &depth)) {
        if (depth == 32)
          gst_structure_set (s, "depth", G_TYPE_INT, 32, NULL);
        else
          gst_structure_set (s, "depth", GST_TYPE_INT_RANGE, depth, 32, NULL);
      }
    }

    allow_mixing = TRUE;
    if (gst_structure_get_int (structure, "channels", &channels)) {
      gboolean unpositioned;

      /* we don't support mixing for channels without channel positions */
      if (structure_has_fixed_channel_positions (structure, &unpositioned))
        allow_mixing = (unpositioned == FALSE);
    }

    if (!allow_mixing) {
      gst_structure_set (s, "channels", G_TYPE_INT, channels, NULL);
      if (gst_structure_has_field (structure, "channel-positions"))
        gst_structure_set_value (s, "channel-positions",
            gst_structure_get_value (structure, "channel-positions"));
    } else {
      if (channels == 0)
        gst_structure_set (s, "channels", GST_TYPE_INT_RANGE, 1, 11, NULL);
      else if (channels == 11)
        gst_structure_set (s, "channels", G_TYPE_INT, 11, NULL);
      else
        gst_structure_set (s, "channels", GST_TYPE_INT_RANGE, channels, 11,
            NULL);
      gst_structure_remove_field (s, "channel-positions");
    }
    gst_caps_merge_structure (ret, gst_structure_copy (s));

    /* Same, plus a float<->int conversion */
    append_with_other_format (ret, s, isfloat);

    /* We'll reduce depth if we must. We reduce as low as 16 bits (for integer); 
     * reducing to less than this is even worse than dropping channels. We only 
     * do this if we haven't already done the equivalent above. */
    if (!gst_structure_get_int (structure, "width", &width) || width > 16) {
      if (isfloat) {
        GstStructure *s2 = gst_structure_copy (s);

        set_structure_widths_32_and_64 (s2);
        append_with_other_format (ret, s2, TRUE);
        gst_structure_free (s2);
      } else {
        GstStructure *s2 = gst_structure_copy (s);

        set_structure_widths (s2, 16, 32);
        gst_structure_set (s2, "depth", GST_TYPE_INT_RANGE, 16, 32, NULL);
        gst_caps_merge_structure (ret, s2);
      }
    }

    /* Channel conversions to fewer channels is only done if needed - generally
     * it's very bad to drop channels entirely.
     */
    if (allow_mixing) {
      gst_structure_set (s, "channels", GST_TYPE_INT_RANGE, 1, 11, NULL);
      gst_structure_remove_field (s, "channel-positions");
    } else {
      /* allow_mixing can only be FALSE if we got a fixed number of channels */
      gst_structure_set (s, "channels", G_TYPE_INT, channels, NULL);
      if (gst_structure_has_field (structure, "channel-positions"))
        gst_structure_set_value (s, "channel-positions",
            gst_structure_get_value (structure, "channel-positions"));
    }
    gst_caps_merge_structure (ret, gst_structure_copy (s));

    /* Same, plus a float<->int conversion */
    append_with_other_format (ret, s, isfloat);

    /* And, finally, for integer only, we allow conversion to any width/depth we
     * support: this should be equivalent to our (non-float) template caps. (the
     * floating point case should be being handled just above) */
    set_structure_widths (s, 8, 32);
    gst_structure_set (s, "depth", GST_TYPE_INT_RANGE, 1, 32, NULL);

    if (isfloat) {
      append_with_other_format (ret, s, TRUE);
      gst_structure_free (s);
    } else
      gst_caps_merge_structure (ret, s);
  }

  GST_DEBUG_OBJECT (base, "Caps transformed to %" GST_PTR_FORMAT, ret);

  if (filter) {
    GstCaps *intersection;

    GST_DEBUG_OBJECT (base, "Using filter caps %" GST_PTR_FORMAT, filter);
    intersection =
        gst_caps_intersect_full (filter, ret, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = intersection;
    GST_DEBUG_OBJECT (base, "Intersection %" GST_PTR_FORMAT, ret);
  }

  return ret;
}

static const GstAudioChannelPosition default_positions[8][8] = {
  /* 1 channel */
  {
        GST_AUDIO_CHANNEL_POSITION_FRONT_MONO,
      },
  /* 2 channels */
  {
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
      },
  /* 3 channels (2.1) */
  {
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_LFE, /* or FRONT_CENTER for 3.0? */
      },
  /* 4 channels (4.0 or 3.1?) */
  {
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
      },
  /* 5 channels */
  {
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
      },
  /* 6 channels */
  {
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_LFE,
      },
  /* 7 channels */
  {
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_LFE,
        GST_AUDIO_CHANNEL_POSITION_REAR_CENTER,
      },
  /* 8 channels */
  {
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
        GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
        GST_AUDIO_CHANNEL_POSITION_LFE,
        GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
        GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT,
      }
};

static const GValue *
find_suitable_channel_layout (const GValue * val, guint chans)
{
  /* if output layout is fixed already and looks sane, we're done */
  if (GST_VALUE_HOLDS_ARRAY (val) && gst_value_array_get_size (val) == chans)
    return val;

  /* if it's a list, go through it recursively and return the first
   * sane-enough looking value we find */
  if (GST_VALUE_HOLDS_LIST (val)) {
    gint i;

    for (i = 0; i < gst_value_list_get_size (val); ++i) {
      const GValue *v, *ret;

      v = gst_value_list_get_value (val, i);
      if ((ret = find_suitable_channel_layout (v, chans)))
        return ret;
    }
  }

  return NULL;
}

static void
gst_audio_convert_fixate_channels (GstBaseTransform * base, GstStructure * ins,
    GstStructure * outs)
{
  const GValue *in_layout, *out_layout;
  gint in_chans, out_chans;

  if (!gst_structure_get_int (ins, "channels", &in_chans))
    return;                     /* this shouldn't really happen, should it? */

  if (!gst_structure_has_field (outs, "channels")) {
    /* we could try to get the implied number of channels from the layout,
     * but that seems overdoing it for a somewhat exotic corner case */
    gst_structure_remove_field (outs, "channel-positions");
    return;
  }

  /* ok, let's fixate the channels if they are not fixated yet */
  gst_structure_fixate_field_nearest_int (outs, "channels", in_chans);

  if (!gst_structure_get_int (outs, "channels", &out_chans)) {
    /* shouldn't really happen ... */
    gst_structure_remove_field (outs, "channel-positions");
    return;
  }

  /* check if the output has a channel layout (or a list of layouts) */
  out_layout = gst_structure_get_value (outs, "channel-positions");

  /* get the channel layout of the input if any */
  in_layout = gst_structure_get_value (ins, "channel-positions");

  if (out_layout == NULL) {
    if (out_chans <= 2 && (in_chans != out_chans || in_layout == NULL))
      return;                   /* nothing to do, default layout will be assumed */
    GST_WARNING_OBJECT (base, "downstream caps contain no channel layout");
  }

  if (in_chans == out_chans && in_layout != NULL) {
    GValue res = { 0, };

    /* same number of channels and no output layout: just use input layout */
    if (out_layout == NULL) {
      gst_structure_set_value (outs, "channel-positions", in_layout);
      return;
    }

    /* if output layout is fixed already and looks sane, we're done */
    if (GST_VALUE_HOLDS_ARRAY (out_layout) &&
        gst_value_array_get_size (out_layout) == out_chans) {
      return;
    }

    /* if the output layout is not fixed, check if the output layout contains
     * the input layout */
    if (gst_value_intersect (&res, in_layout, out_layout)) {
      gst_structure_set_value (outs, "channel-positions", in_layout);
      g_value_unset (&res);
      return;
    }

    /* output layout is not fixed and does not contain the input layout, so
     * just pick the first layout in the list (it should be a list ...) */
    if ((out_layout = find_suitable_channel_layout (out_layout, out_chans))) {
      gst_structure_set_value (outs, "channel-positions", out_layout);
      return;
    }

    /* ... else fall back to default layout (NB: out_layout is NULL here) */
    GST_WARNING_OBJECT (base, "unexpected output channel layout");
  }

  /* number of input channels != number of output channels:
   * if this value contains a list of channel layouts (or even worse: a list
   * with another list), just pick the first value and repeat until we find a
   * channel position array or something else that's not a list; we assume
   * the input if half-way sane and don't try to fall back on other list items
   * if the first one is something unexpected or non-channel-pos-array-y */
  if (out_layout != NULL && GST_VALUE_HOLDS_LIST (out_layout))
    out_layout = find_suitable_channel_layout (out_layout, out_chans);

  if (out_layout != NULL) {
    if (GST_VALUE_HOLDS_ARRAY (out_layout) &&
        gst_value_array_get_size (out_layout) == out_chans) {
      /* looks sane enough, let's use it */
      gst_structure_set_value (outs, "channel-positions", out_layout);
      return;
    }

    /* what now?! Just ignore what we're given and use default positions */
    GST_WARNING_OBJECT (base, "invalid or unexpected channel-positions");
  }

  /* missing or invalid output layout and we can't use the input layout for
   * one reason or another, so just pick a default layout (we could be smarter
   * and try to add/remove channels from the input layout, or pick a default
   * layout based on LFE-presence in input layout, but let's save that for
   * another day) */
  if (out_chans > 0 && out_chans <= G_N_ELEMENTS (default_positions[0])) {
    GST_DEBUG_OBJECT (base, "using default channel layout as fallback");
    gst_audio_set_channel_positions (outs, default_positions[out_chans - 1]);
  }
}

/* try to keep as many of the structure members the same by fixating the
 * possible ranges; this way we convert the least amount of things as possible
 */
static void
gst_audio_convert_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstStructure *ins, *outs;
  gint rate, endianness, depth, width;
  gboolean signedness;

  g_return_if_fail (gst_caps_is_fixed (caps));

  GST_DEBUG_OBJECT (base, "trying to fixate othercaps %" GST_PTR_FORMAT
      " based on caps %" GST_PTR_FORMAT, othercaps, caps);

  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  gst_audio_convert_fixate_channels (base, ins, outs);

  if (gst_structure_get_int (ins, "rate", &rate)) {
    if (gst_structure_has_field (outs, "rate")) {
      gst_structure_fixate_field_nearest_int (outs, "rate", rate);
    }
  }
  if (gst_structure_get_int (ins, "endianness", &endianness)) {
    if (gst_structure_has_field (outs, "endianness")) {
      gst_structure_fixate_field_nearest_int (outs, "endianness", endianness);
    }
  }
  if (gst_structure_get_int (ins, "width", &width)) {
    if (gst_structure_has_field (outs, "width")) {
      gst_structure_fixate_field_nearest_int (outs, "width", width);
    }
  } else {
    /* this is not allowed */
  }

  if (gst_structure_get_int (ins, "depth", &depth)) {
    if (gst_structure_has_field (outs, "depth")) {
      gst_structure_fixate_field_nearest_int (outs, "depth", depth);
    }
  } else {
    /* set depth as width */
    if (gst_structure_has_field (outs, "depth")) {
      gst_structure_fixate_field_nearest_int (outs, "depth", width);
    }
  }

  if (gst_structure_get_boolean (ins, "signed", &signedness)) {
    if (gst_structure_has_field (outs, "signed")) {
      gst_structure_fixate_field_boolean (outs, "signed", signedness);
    }
  }

  GST_DEBUG_OBJECT (base, "fixated othercaps to %" GST_PTR_FORMAT, othercaps);
}

static gboolean
gst_audio_convert_set_caps (GstBaseTransform * base, GstCaps * incaps,
    GstCaps * outcaps)
{
  AudioConvertFmt in_ac_caps = { 0 };
  AudioConvertFmt out_ac_caps = { 0 };
  GstAudioConvert *this = GST_AUDIO_CONVERT (base);

  GST_DEBUG_OBJECT (base, "incaps %" GST_PTR_FORMAT ", outcaps %"
      GST_PTR_FORMAT, incaps, outcaps);

  if (!gst_audio_convert_parse_caps (incaps, &in_ac_caps))
    return FALSE;
  if (!gst_audio_convert_parse_caps (outcaps, &out_ac_caps))
    return FALSE;

  if (!audio_convert_prepare_context (&this->ctx, &in_ac_caps, &out_ac_caps,
          this->dither, this->ns))
    goto no_converter;

  return TRUE;

no_converter:
  {
    return FALSE;
  }
}

static GstFlowReturn
gst_audio_convert_transform_ip (GstBaseTransform * base, GstBuffer * buf)
{
  /* nothing to do here */
  return GST_FLOW_OK;
}

static void
gst_audio_convert_create_silence_buffer (GstAudioConvert * this, gpointer dst,
    gint size)
{
  if (this->ctx.out.is_int && !this->ctx.out.sign) {
    gint i;

    switch (this->ctx.out.width) {
      case 8:{
        guint8 zero = 0x80 >> (8 - this->ctx.out.depth);

        memset (dst, zero, size);
        break;
      }
      case 16:{
        guint16 *data = (guint16 *) dst;
        guint16 zero = 0x8000 >> (16 - this->ctx.out.depth);

        if (this->ctx.out.endianness == G_LITTLE_ENDIAN)
          zero = GUINT16_TO_LE (zero);
        else
          zero = GUINT16_TO_BE (zero);

        size /= 2;

        for (i = 0; i < size; i++)
          data[i] = zero;
        break;
      }
      case 24:{
        guint32 zero = 0x800000 >> (24 - this->ctx.out.depth);
        guint8 *data = (guint8 *) dst;

        if (this->ctx.out.endianness == G_LITTLE_ENDIAN) {
          for (i = 0; i < size; i += 3) {
            data[i] = zero & 0xff;
            data[i + 1] = (zero >> 8) & 0xff;
            data[i + 2] = (zero >> 16) & 0xff;
          }
        } else {
          for (i = 0; i < size; i += 3) {
            data[i + 2] = zero & 0xff;
            data[i + 1] = (zero >> 8) & 0xff;
            data[i] = (zero >> 16) & 0xff;
          }
        }
        break;
      }
      case 32:{
        guint32 *data = (guint32 *) dst;
        guint32 zero = (0x80000000 >> (32 - this->ctx.out.depth));

        if (this->ctx.out.endianness == G_LITTLE_ENDIAN)
          zero = GUINT32_TO_LE (zero);
        else
          zero = GUINT32_TO_BE (zero);

        size /= 4;

        for (i = 0; i < size; i++)
          data[i] = zero;
        break;
      }
      default:
        memset (dst, 0, size);
        g_return_if_reached ();
        break;
    }
  } else {
    memset (dst, 0, size);
  }
}

static GstFlowReturn
gst_audio_convert_transform (GstBaseTransform * base, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstFlowReturn ret;
  GstAudioConvert *this = GST_AUDIO_CONVERT (base);
  gsize srcsize, dstsize;
  gint insize, outsize;
  gint samples;
  gpointer src, dst;

  /* get amount of samples to convert. */
  samples = gst_buffer_get_size (inbuf) / this->ctx.in.unit_size;

  /* get in/output sizes, to see if the buffers we got are of correct
   * sizes */
  if (!audio_convert_get_sizes (&this->ctx, samples, &insize, &outsize))
    goto error;

  if (insize == 0 || outsize == 0)
    return GST_FLOW_OK;

  /* get src and dst data */
  src = gst_buffer_map (inbuf, &srcsize, NULL, GST_MAP_READ);
  dst = gst_buffer_map (outbuf, &dstsize, NULL, GST_MAP_WRITE);

  /* check in and outsize */
  if (srcsize < insize)
    goto wrong_size;
  if (dstsize < outsize)
    goto wrong_size;

  /* and convert the samples */
  if (!GST_BUFFER_FLAG_IS_SET (inbuf, GST_BUFFER_FLAG_GAP)) {
    if (!audio_convert_convert (&this->ctx, src, dst,
            samples, gst_buffer_is_writable (inbuf)))
      goto convert_error;
  } else {
    /* Create silence buffer */
    gst_audio_convert_create_silence_buffer (this, dst, outsize);
  }
  ret = GST_FLOW_OK;

done:
  gst_buffer_unmap (outbuf, dst, outsize);
  gst_buffer_unmap (inbuf, src, srcsize);

  return ret;

  /* ERRORS */
error:
  {
    GST_ELEMENT_ERROR (this, STREAM, FORMAT,
        (NULL), ("cannot get input/output sizes for %d samples", samples));
    return GST_FLOW_ERROR;
  }
wrong_size:
  {
    GST_ELEMENT_ERROR (this, STREAM, FORMAT,
        (NULL),
        ("input/output buffers are of wrong size in: %d < %d or out: %d < %d",
            srcsize, insize, dstsize, outsize));
    ret = GST_FLOW_ERROR;
    goto done;
  }
convert_error:
  {
    GST_ELEMENT_ERROR (this, STREAM, FORMAT,
        (NULL), ("error while converting"));
    ret = GST_FLOW_ERROR;
    goto done;
  }
}

static void
gst_audio_convert_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (object);

  switch (prop_id) {
    case ARG_DITHERING:
      this->dither = g_value_get_enum (value);
      break;
    case ARG_NOISE_SHAPING:
      this->ns = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_audio_convert_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (object);

  switch (prop_id) {
    case ARG_DITHERING:
      g_value_set_enum (value, this->dither);
      break;
    case ARG_NOISE_SHAPING:
      g_value_set_enum (value, this->ns);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
