/*
 * Dynamic pipelines example, uridecodebin with sinks added and removed
 *
 * Copyright (c) 2014 Sebastian Dröge <sebastian@centricular.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <string.h>
#include <gst/gst.h>

static GMainLoop *loop;
static GstElement *pipeline;
static GstElement *src, *dbin, *conv, *tee, *gsink;
static gboolean linked = FALSE;
static GList *sinks;

typedef struct
{
  GstPad *teepad;
  GstElement *queue;
  GstElement *conv;
  GstElement *enc;
  GstElement *parse;
  GstElement *muxer;
  GstElement *sink;
  GstElement *sbin;
  gboolean removing;
} Sink;

typedef struct _PadPrivate  PadPrivate;

struct _PadPrivate
{
  PadPrivate *priv;
  GstPadDirection dir;
  GstPadEventFunction eventfunc;
  GstPadQueryFunction queryfunc;
};

static GstEvent *translate_outgoing_segment (GstObject * object, GstEvent * event)
{
  const GstSegment *orig;
  GstSegment segment;
  GstEvent *event2;
  guint32 seqnum = GST_EVENT_SEQNUM (event);

  /* only modify the streamtime */
  gst_event_parse_segment (event, &orig);

  g_printerr("Got SEGMENT %" GST_TIME_FORMAT " -- %" GST_TIME_FORMAT " // %"
      GST_TIME_FORMAT, GST_TIME_ARGS (orig->start), GST_TIME_ARGS (orig->stop),
      GST_TIME_ARGS (orig->time));

  if (G_UNLIKELY (orig->format != GST_FORMAT_TIME)) {
    g_printerr("Can't translate segments with format != GST_FORMAT_TIME\n");
    return event;
  }

  gst_segment_copy_into (orig, &segment);

  //gnl_media_to_object_time (object, orig->time, &segment.time);
  static guint64 base = 0;
  
  segment.time = 0;
  
  //segment.stop = 10000000000;

   
  gint64 cur=0;
gboolean ret = gst_element_query_position (pipeline, GST_FORMAT_TIME ,&cur);
  g_print("ret=%d, cur=%ld\n", ret, cur);
  base += 10000000000;
  segment.base -= cur;
  segment.stop = -1;
  if (G_UNLIKELY (segment.time > G_MAXINT64))
    g_printerr("Return value too big...\n");

  g_printerr("Sending SEGMENT %" GST_TIME_FORMAT " -- %" GST_TIME_FORMAT " // %"
      GST_TIME_FORMAT, GST_TIME_ARGS (segment.start),
      GST_TIME_ARGS (segment.stop), GST_TIME_ARGS (segment.time));

  event2 = gst_event_new_segment (&segment);
  GST_EVENT_SEQNUM (event2) = seqnum;
  gst_event_unref (event);

  return event2;
}

static gboolean internalpad_event_function (GstPad * internal, GstObject * parent, GstEvent * event)
{
  PadPrivate *priv = gst_pad_get_element_private (internal);
  gboolean res;

  g_printerr("event:%s (seqnum::%d)\n",
      GST_EVENT_TYPE_NAME (event), GST_EVENT_SEQNUM (event));

  if (G_UNLIKELY (!(priv->eventfunc))) {
    g_printerr("priv->eventfunc == NULL !! What is going on ?\n");
    return FALSE;
  }

  /*switch (priv->dir) {
    case GST_PAD_SINK:{*/
      switch (GST_EVENT_TYPE (event)) {
        case GST_EVENT_SEGMENT:
          event = translate_outgoing_segment (parent, event);
          break;
        default:
          break;
      }
/*
      break;
    }
    default:
      break;
  }*/
  g_printerr("Calling priv->eventfunc %p\n", priv->eventfunc);
  res = priv->eventfunc (internal, parent, event);

  return res;
} 

static gboolean
message_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_error (message, &err, &debug);

      g_printerr ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        g_printerr ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);

      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_WARNING:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_warning (message, &err, &debug);

      g_printerr ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        g_printerr ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);
      break;
    }
    case GST_MESSAGE_EOS:
      g_print ("Got EOS\n");
      g_main_loop_quit (loop);
      break;
    default:
      break;
  }

  return TRUE;
}

static GstPadProbeReturn
unlink_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Sink *sink = user_data;
  GstPad *sinkpad;

  if (!g_atomic_int_compare_and_exchange (&sink->removing, FALSE, TRUE))
    return GST_PAD_PROBE_OK;

  sinkpad = gst_element_get_static_pad (sink->sbin, "sink");
  gst_pad_unlink (sink->teepad, sinkpad);
  gst_object_unref (sinkpad);

  gst_bin_remove (GST_BIN (pipeline), sink->sbin);

  gst_element_set_state (sink->sbin, GST_STATE_NULL);

  gst_object_unref (sink->sbin);

  gst_element_release_request_pad (tee, sink->teepad);
  gst_object_unref (sink->teepad);

  g_print ("removed\n");

  return GST_PAD_PROBE_REMOVE;
}

GstPadProbeReturn cb_buffer (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
	g_print("dts:%ld pts:%ld\n", GST_TIME_AS_MSECONDS(GST_BUFFER_DTS(buffer)), GST_TIME_AS_MSECONDS(GST_BUFFER_PTS(buffer)));
	return GST_PAD_PROBE_REMOVE;
}

static gboolean
tick_cb (gpointer data)
{
  if (!sinks) {
    Sink *sink = g_new0 (Sink, 1);
    GstPad *sinkpad;
    GstPadTemplate *templ;

    templ =
        gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (tee),
        "src_%u");

    g_print ("add\n");

    sink->sbin = gst_bin_new ("sbin");

    sink->teepad = gst_element_request_pad (tee, templ, NULL, NULL);

    sink->queue = gst_element_factory_make ("queue", NULL);
    sink->conv = gst_element_factory_make ("videoconvert", NULL);
    sink->enc = gst_element_factory_make ("x264enc", NULL);
    sink->parse = gst_element_factory_make ("h264parse", NULL);
    sink->muxer = gst_element_factory_make ("matroskamux", NULL);
    sink->sink = gst_element_factory_make ("filesink", NULL);
    sink->removing = FALSE;

    g_object_set(sink->enc, "bframes", 0, "key-int-max", 45, "bitrate", 500, NULL);

    GDateTime * date_time = g_date_time_new_now_utc();
    gchar *time_str = g_date_time_format(date_time, "%Y%m%d_%H%M%S.mkv");
    g_object_set(sink->sink, "location", time_str, NULL);
    g_date_time_unref(date_time);
    g_free(time_str);

    gst_bin_add_many (GST_BIN (sink->sbin), sink->queue, sink->conv, sink->enc, sink->parse, sink->muxer, sink->sink, NULL);
    gst_element_link_many (sink->queue, sink->conv, sink->enc, sink->parse, sink->muxer, sink->sink, NULL);

    gst_bin_add_many (GST_BIN (pipeline), gst_object_ref(sink->sbin), NULL);

    sinkpad = gst_element_get_static_pad (sink->queue, "sink");
    gst_pad_add_probe (sinkpad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback)cb_buffer, NULL, NULL);
    gst_object_unref (sinkpad);

    GstPad *pad = gst_element_get_static_pad(sink->queue, "sink");
    gst_element_add_pad(sink->sbin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(pad);


    gst_element_sync_state_with_parent (sink->sbin);

    sinkpad = gst_element_get_static_pad (sink->sbin, "sink");
#if 1
    PadPrivate *priv;
    if (G_UNLIKELY (!(priv = gst_pad_get_element_private (sinkpad)))) {
    g_printerr("Creating a PadPrivate to put in element_private\n");
    priv = g_slice_new0 (PadPrivate);

    /* Remember existing pad functions */
    priv->eventfunc = GST_PAD_EVENTFUNC (sinkpad);
    priv->queryfunc = GST_PAD_QUERYFUNC (sinkpad);
    gst_pad_set_element_private (sinkpad, priv);

    /* add query/event function overrides on internal pad */
    gst_pad_set_event_function (sinkpad, internalpad_event_function);
  }
#else
		gint64 cur=0;
		gst_element_query_position (pipeline, GST_FORMAT_TIME ,&cur);
    gst_pad_set_offset(sinkpad, 0-cur);
#endif

    /*GstSample *sample = NULL;
    g_object_get(gsink, "last-sample", &sample, NULL);
    GstSegment * segment = gst_sample_get_segment(sample);
    GstEvent *evnt = gst_event_new_segment (segment);
    gst_pad_send_event(sinkpad, evnt);*/

    
    gst_pad_link (sink->teepad, sinkpad);
    gst_object_unref (sinkpad);

    g_print ("added\n");

    sinks = g_list_append (sinks, sink);
  } else {
    Sink *sink;

    g_print ("remove\n");

    sink = sinks->data;
    sinks = g_list_delete_link (sinks, sinks);
    gst_pad_add_probe (sink->teepad, GST_PAD_PROBE_TYPE_IDLE, unlink_cb, sink,
        (GDestroyNotify) g_free);
  }

  return TRUE;
}

static void
pad_added_cb (GstElement * element, GstPad * pad, gpointer user_data)
{
  GstCaps *caps;
  GstStructure *s;
  const gchar *name;

  if (linked)
    return;

  caps = gst_pad_get_current_caps (pad);
  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);

  if (strcmp (name, "video/x-raw") == 0) {
    GstPad *sinkpad, *teepad;
    GstElement *queue, *sink;
    GstPadTemplate *templ;

    sinkpad = gst_element_get_static_pad (conv, "sink");
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK) {
      g_printerr ("Failed to link dbin with conv\n");
      gst_object_unref (sinkpad);
      g_main_loop_quit (loop);
      return;
    }
    gst_object_unref (sinkpad);

    templ =
        gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (tee),
        "src_%u");
    teepad = gst_element_request_pad (tee, templ, NULL, NULL);
    queue = gst_element_factory_make ("queue", NULL);
    sink = gst_element_factory_make ("xvimagesink", NULL);
    gsink = sink;

    g_object_set (sink, "sync", TRUE, NULL);

    gst_bin_add_many (GST_BIN (pipeline), queue, sink, NULL);

    gst_element_sync_state_with_parent (sink);
    gst_element_sync_state_with_parent (queue);

    gst_element_link_many (queue, sink, NULL);

    sinkpad = gst_element_get_static_pad (queue, "sink");
    gst_pad_link (teepad, sinkpad);
    gst_object_unref (sinkpad);

    g_timeout_add_seconds (10, tick_cb, NULL);
    linked = TRUE;
  }

  gst_caps_unref (caps);
}

int
main (int argc, char **argv)
{
  GstBus *bus;

  if (argc != 2) {
    g_error ("Usage: %s filename", argv[0]);
    return 0;
  }

  gst_init (&argc, &argv);

  pipeline = gst_pipeline_new (NULL);
  src = gst_element_factory_make ("filesrc", NULL);
  dbin = gst_element_factory_make ("decodebin", NULL);
  conv = gst_element_factory_make ("videoconvert", NULL);
  tee = gst_element_factory_make ("tee", NULL);

  if (!pipeline || !src || !dbin || !conv || !tee) {
    g_error ("Failed to create elements");
    return -1;
  }

  g_object_set (src, "location", argv[1], NULL);

  gst_bin_add_many (GST_BIN (pipeline), src, dbin, conv, tee, NULL);
  if (!gst_element_link_many (src, dbin, NULL) ||
      !gst_element_link_many (conv, tee, NULL)) {
    g_error ("Failed to link elements");
    return -2;
  }

  g_signal_connect (dbin, "pad-added", G_CALLBACK (pad_added_cb), NULL);

  loop = g_main_loop_new (NULL, FALSE);

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message", G_CALLBACK (message_cb), NULL);
  gst_object_unref (GST_OBJECT (bus));

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  g_main_loop_run (loop);

  gst_element_set_state (pipeline, GST_STATE_NULL);

  g_main_loop_unref (loop);

  gst_object_unref (pipeline);

  return 0;
}
