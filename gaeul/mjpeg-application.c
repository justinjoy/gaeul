/**
 *  Copyright 2020 SK Telecom Co., Ltd.
 *    Author: Jeongseok Kim <jeongseok.kim@sk.com>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include "config.h"

#include "gaeul.h"
#include "mjpeg-application.h"

#include <gst/gst.h>
#include <libsoup/soup.h>
#include <libsoup/soup-server.h>
#include <json-glib/json-glib.h>

struct _GaeulMjpegApplication
{
  GaeulApplication parent;

  SoupServer *soup_server;

  GHashTable *pipelines;

  /* {SoupWebsocketConnection, GList<string>} */
  GHashTable *channels;

  GSettings *settings;
};

/* *INDENT-OFF* */
G_DEFINE_TYPE (GaeulMjpegApplication, gaeul_mjpeg_application, GAEUL_TYPE_APPLICATION)
/* *INDENT-ON* */

static void
_channel_list_free (GList * channels)
{
  g_list_free_full (channels, g_free);
}

static void
gaeul_mjpeg_http_message_wrote_headers_cb (SoupMessage * msg,
    gpointer user_data)
{
  GstElement *pipeline = user_data;

  g_debug ("wrote_headers");

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
}

static void
gaeul_mjpeg_http_request_cb (SoupServer * server, SoupMessage * msg,
    const char *path, GHashTable * query, SoupClientContext * client_ctx,
    gpointer user_data)
{
  GaeulMjpegApplication *self = user_data;

  GstElement *pipeline = NULL;
  GstElement *sink = NULL;
  g_autofree gchar *stream_id = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) id_path = NULL;

  g_debug ("request: path (%s)", path);

  if (!g_str_has_prefix (path, "/mjpeg/")) {
    soup_message_set_status (msg, SOUP_STATUS_NOT_FOUND);
    return;
  }

  stream_id = g_strdup (path + 7);
  id_path = g_strsplit (stream_id, "/", 2);
  g_debug ("uid: [%s], id: [%s]", id_path[0], id_path[1]);

  /* look up id */
  pipeline = g_hash_table_lookup (self->pipelines, stream_id);
  if (g_strcmp0 (GST_ELEMENT_NAME (pipeline), "dummy") != 0) {
    g_info ("pipeline is already started");
    return;
  }

  soup_message_set_http_version (msg, SOUP_HTTP_1_0);
  soup_message_headers_set_encoding (msg->response_headers, SOUP_ENCODING_EOF);
  soup_message_headers_set_content_type (msg->response_headers,
      "multipart/x-mixed-replace; boundary=--endofsection", NULL);
  soup_message_set_status (msg, SOUP_STATUS_OK);

  {
    g_autofree gchar *pipeline_desc = NULL;
    gint port = 0;

    if (g_strcmp0 (id_path[1], "1") == 0)
      port = 50020;
    else if (g_strcmp0 (id_path[1], "2") == 0)
      port = 50021;
    else if (g_strcmp0 (id_path[1], "3") == 0)
      port = 50022;
    pipeline_desc =
        g_strdup_printf
        ("srtsrc uri=\"srt://58.120.27.194:%d?mode=caller\" ! queue ! tsdemux latency=75 ! decodebin ! videoconvert ! videoscale ! videorate ! video/x-raw, framerate=15/1, width=640, height=480 ! jpegenc ! multipartmux boundary=endofsection ! multisocketsink name=msocksink",
        port);

    g_debug ("pipeline : %s", pipeline_desc);
    pipeline = gst_parse_launch (pipeline_desc, &error);

  }
  if (error) {
    g_error (error->message);
  }

  gst_element_set_name (pipeline, stream_id);

  sink = gst_bin_get_by_name (GST_BIN (pipeline), "msocksink");

  gst_element_set_state (pipeline, GST_STATE_READY);
  g_signal_emit_by_name (sink, "add",
      soup_client_context_get_gsocket (client_ctx));

  gst_object_unref (GST_OBJECT (sink));

  g_signal_connect (G_OBJECT (msg), "wrote-headers",
      G_CALLBACK (gaeul_mjpeg_http_message_wrote_headers_cb), pipeline);

  g_hash_table_replace (self->pipelines, g_steal_pointer (&stream_id),
      pipeline);
}

static void
_invoke_ws_jsonrpc_start (GaeulMjpegApplication * self,
    SoupWebsocketConnection * connection, const gchar * call_id,
    const JsonNode * params)
{
  JsonObject *json_obj = json_node_get_object (params);
  JsonNode *json_node = NULL;
  const gchar *uid = NULL;
  gint channel = -1;
  GList *channels = NULL;
  g_autofree gchar *stream_id = NULL;

  if (json_object_has_member (json_obj, "uid")) {
    json_node = json_object_get_member (json_obj, "uid");
    uid = json_node_get_string (json_node);
  }

  if (json_object_has_member (json_obj, "channel")) {
    json_node = json_object_get_member (json_obj, "channel");
    channel = json_node_get_int (json_node);
  }

  stream_id = g_strdup_printf ("%s/%d", uid, channel);
  channels = g_hash_table_lookup (self->channels, connection);

  if (channels != NULL) {
    GList *found = g_list_find_custom (channels, stream_id, g_strcmp0);
    if (found) {
      // TODO: send error
      return;
    }
  }

  {
    // TODO: use json message builder;

    g_autofree gchar *json_response = NULL;

    json_response =
        g_strdup_printf
        ("{\"result\": { \"url\": \"http://d.poopu.com:9222/mjpeg/%s\" }, \"id\": %d }",
        stream_id, call_id);
    soup_websocket_connection_send_text (connection, json_response);
  }

  g_hash_table_insert (self->pipelines, g_strdup (stream_id),
      gst_pipeline_new ("dummy"));

  channels = g_list_prepend (channels, g_steal_pointer (&stream_id));
  g_hash_table_replace (self->channels, g_object_ref (connection), channels);
}

static void
_invoke_ws_jsonrpc_stop (GaeulMjpegApplication * self,
    SoupWebsocketConnection * connection, gint call_id, const JsonNode * params)
{
  JsonObject *json_obj = json_node_get_object (params);
  JsonNode *json_node = NULL;
  const gchar *uid = NULL;
  gint channel = -1;
  GList *channels = NULL;

  g_autofree gchar *stream_id = NULL;
  GList *found = NULL;

  if (json_object_has_member (json_obj, "uid")) {
    json_node = json_object_get_member (json_obj, "uid");
    uid = json_node_get_string (json_node);
  }

  if (json_object_has_member (json_obj, "channel")) {
    json_node = json_object_get_member (json_obj, "channel");
    channel = json_node_get_int (json_node);
  }

  stream_id = g_strdup_printf ("%s/%d", uid, channel);

  channels = g_hash_table_lookup (self->channels, connection);
  if (channels == NULL) {
    // TODO: return error? or just ignore? 
    return;
  }

  found = g_list_find_custom (channels, stream_id, g_strcmp0);
  if (found) {
    channels = g_list_remove_link (channels, found);
    g_list_free_full (found, g_free);
  }

  g_hash_table_replace (self->channels, g_object_ref (connection), channels);

  {
    GstElement *pipeline = g_hash_table_lookup (self->pipelines, stream_id);

    if (pipeline != NULL) {
      gst_element_set_state (pipeline, GST_STATE_NULL);
    }

    if (g_hash_table_remove (self->pipelines, stream_id)) {
      g_debug ("pipeline removed");
    }
  }
}

static void
_invoke_ws_jsonrpc_method (GaeulMjpegApplication * self,
    SoupWebsocketConnection * connection, gint call_id,
    const gchar * method, JsonNode * params)
{

  // TODO: method -> enum
  if (g_strcmp0 ("start", method) == 0) {
    _invoke_ws_jsonrpc_start (self, connection, call_id, params);
  } else if (g_strcmp0 ("stop", method) == 0) {
    _invoke_ws_jsonrpc_stop (self, connection, call_id, params);
  }
}

static void
gaeul_mjpeg_ws_message_cb (SoupWebsocketConnection * connection,
    gint type, GBytes * message, gpointer user_data)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  g_autoptr (GError) error = NULL;

  gsize len = 0;
  JsonNode *json_node = NULL;
  JsonObject *json_obj = NULL;
  const gchar *content = g_bytes_get_data (message, &len);
  const gchar *method_name = NULL;
  gint call_id = 0;

  g_debug ("%s", content);
  if (!json_parser_load_from_data (parser, content, -1, &error)) {
    g_error ("failed to parse json-rpc message (reason: %s)", error->message);
    return;
  }

  json_obj = json_node_get_object (json_parser_get_root (parser));

  if (json_object_has_member (json_obj, "method")) {
    json_node = json_object_get_member (json_obj, "method");
    method_name = json_node_get_string (json_node);
  }

  if (json_object_has_member (json_obj, "id")) {
    json_node = json_object_get_member (json_obj, "id");
    call_id = json_node_get_int (json_node);
  }

  if (json_object_has_member (json_obj, "params")) {
    json_node = json_object_get_member (json_obj, "params");
  } else {
    json_node = NULL;
  }

  _invoke_ws_jsonrpc_method (user_data, connection, call_id, method_name,
      json_node);
}

static void
gaeul_mjpeg_ws_error_cb (SoupWebsocketConnection * self,
    GError * error, gpointer user_data)
{
  g_debug ("error: %s", error->message);
}

static void
gaeul_mjpeg_ws_pong_cb (SoupWebsocketConnection * self,
    GBytes * message, gpointer user_data)
{
  g_debug ("pong len: %d", g_bytes_get_size (message));
}

static void
gaeul_mjpeg_ws_closed_cb (SoupWebsocketConnection * connection,
    gpointer user_data)
{
  g_debug ("ws closed");
  g_object_unref (connection);
}

static void
gaeul_mjpeg_ws_cb (SoupServer * server,
    SoupWebsocketConnection * connection,
    const char *path, SoupClientContext * client_ctx, gpointer user_data)
{
  GaeulMjpegApplication *self = user_data;

  g_debug ("ws: path (%s)", path);

  g_object_ref (connection);
//  g_object_set (connection, "keepalive-interval", 1, NULL);

  g_signal_connect (connection, "message",
      G_CALLBACK (gaeul_mjpeg_ws_message_cb), self);
  g_signal_connect (connection, "error", G_CALLBACK (gaeul_mjpeg_ws_error_cb),
      self);
  g_signal_connect (connection, "pong", G_CALLBACK (gaeul_mjpeg_ws_pong_cb),
      self);
  g_signal_connect (connection, "closed", G_CALLBACK (gaeul_mjpeg_ws_closed_cb),
      self);

}

static void
gaeul_mjpeg_application_dispose (GObject * object)
{
  GaeulMjpegApplication *self = GAEUL_MJPEG_APPLICATION (object);

  g_clear_object (&self->settings);

  soup_server_disconnect (self->soup_server);
  g_clear_object (&self->soup_server);

  g_hash_table_remove_all (self->pipelines);
  g_clear_pointer (&self->pipelines, g_hash_table_destroy);

  G_OBJECT_CLASS (gaeul_mjpeg_application_parent_class)->dispose (object);
}

static void
gaeul_mjpeg_application_startup (GApplication * app)
{
  GaeulMjpegApplication *self = GAEUL_MJPEG_APPLICATION (app);
  g_autoptr (GError) error = NULL;

  guint port = 9222;
  g_clear_object (&self->settings);
  self->settings = gaeul_gsettings_new (GAEUL_MJPEG_APPLICATION_SCHEMA_ID,
      gaeul_application_get_config_path (GAEUL_APPLICATION (self)));

  g_settings_bind (self->settings, "uid", self, "uid", G_SETTINGS_BIND_DEFAULT);

  g_debug ("startup");

  soup_server_add_handler (self->soup_server, "/mjpeg",
      gaeul_mjpeg_http_request_cb, self, NULL);
  soup_server_add_websocket_handler (self->soup_server, "/ws", NULL, NULL,
      gaeul_mjpeg_ws_cb, self, NULL);

  if (!soup_server_listen_all (self->soup_server, port, 0, &error)) {
    g_error ("failed to start http server (reason: %s)", error->message);
  }

  G_APPLICATION_CLASS (gaeul_mjpeg_application_parent_class)->startup (app);
}

static void
gaeul_mjpeg_application_activate (GApplication * app)
{
  g_debug ("activate");
  G_APPLICATION_CLASS (gaeul_mjpeg_application_parent_class)->activate (app);
}

static void
gaeul_mjpeg_application_shutdown (GApplication * app)
{
  g_debug ("shutdown");
  G_APPLICATION_CLASS (gaeul_mjpeg_application_parent_class)->shutdown (app);
}

static void
gaeul_mjpeg_application_class_init (GaeulMjpegApplicationClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  object_class->dispose = gaeul_mjpeg_application_dispose;

  app_class->startup = gaeul_mjpeg_application_startup;
  app_class->activate = gaeul_mjpeg_application_activate;
  app_class->shutdown = gaeul_mjpeg_application_shutdown;
}

static void
gaeul_mjpeg_application_init (GaeulMjpegApplication * self)
{
  gst_init (NULL, NULL);

  self->soup_server = soup_server_new (NULL, NULL);

  self->pipelines =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, gst_object_unref);
  self->channels =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref,
      NULL);
}
