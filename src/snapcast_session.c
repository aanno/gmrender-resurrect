/* snapcast_session - play Snapcast sessions offered by a media server (PoC).
 *
 * See snapcast_session.h. The negotiation: a HEAD request for the resource with
 * "Accept: audio/x-snapcast-tcp". If the answer has that Content-Type, a GET with
 * the same header returns the session URI. Servers that know nothing of Snapcast
 * answer the HEAD with the media's type, so they cost one small extra request.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "snapcast_session.h"

#include <string.h>
#include <gio/gio.h>

#include "logging.h"

#define SNAPCAST_TCP_TYPE "audio/x-snapcast-tcp"
#define SNAPCAST_TCP_SCHEME "snapcast-tcp://"
// Seconds for connecting and for each read.
#define TIMEOUT_S 3
// Longest header or body line read.
#define MAX_LINE 4096

static int enabled = 0;

void snapcast_session_enable(int enable) {
	enabled = enable;
}

// Sends one request for uri; returns whether the response is a 200 with
// Content-Type SNAPCAST_TCP_TYPE. With body != NULL, also reads the first body
// line into *body (g_free()).
static int request(GUri *uri, const char *method, char **body) {
	int result = 0;
	GError *error = NULL;
	const char *host = g_uri_get_host(uri);
	int port = g_uri_get_port(uri);
	if (port <= 0) port = 80;

	GSocketClient *client = g_socket_client_new();
	g_socket_client_set_timeout(client, TIMEOUT_S);
	GSocketConnectable *address = g_network_address_new(host, port);
	GSocketConnection *conn =
		g_socket_client_connect(client, address, NULL, &error);
	g_object_unref(address);
	if (conn == NULL) {
		Log_info("snapcast", "%s %s:%d: %s", method, host, port,
			 error->message);
		g_clear_error(&error);
		g_object_unref(client);
		return 0;
	}

	const char *path = g_uri_get_path(uri);
	const char *query = g_uri_get_query(uri);
	// IPv6 literals need brackets in the Host header.
	char *host_header = strchr(host, ':')
		? g_strdup_printf("[%s]:%d", host, port)
		: g_strdup_printf("%s:%d", host, port);
	char *req = g_strdup_printf(
		"%s %s%s%s HTTP/1.0\r\nHost: %s\r\nAccept: " SNAPCAST_TCP_TYPE
		"\r\nConnection: close\r\n\r\n",
		method, *path ? path : "/", query ? "?" : "", query ? query : "",
		host_header);
	g_free(host_header);

	GOutputStream *out =
		g_io_stream_get_output_stream(G_IO_STREAM(conn));
	GDataInputStream *in = g_data_input_stream_new(
		g_io_stream_get_input_stream(G_IO_STREAM(conn)));
	g_data_input_stream_set_newline_type(in,
					     G_DATA_STREAM_NEWLINE_TYPE_ANY);
	if (!g_output_stream_write_all(out, req, strlen(req), NULL, NULL,
				       &error)) {
		goto done;
	}

	int status_ok = 0, type_ok = 0;
	for (int i = 0; ; i++) {
		gsize len = 0;
		char *line = g_data_input_stream_read_line(in, &len, NULL,
							   &error);
		if (line == NULL || len > MAX_LINE) {
			g_free(line);
			goto done;
		}
		if (*line == '\0') {  // end of the header
			g_free(line);
			break;
		}
		if (i == 0) {
			// "HTTP/1.x 200 OK"
			const char *sp = strchr(line, ' ');
			status_ok = sp && strncmp(sp + 1, "200", 3) == 0;
		} else if (g_ascii_strncasecmp(line, "Content-Type:", 13) == 0) {
			char *value = g_strstrip(line + 13);
			size_t n = strlen(SNAPCAST_TCP_TYPE);
			type_ok = g_ascii_strncasecmp(value, SNAPCAST_TCP_TYPE, n) == 0
				&& (value[n] == '\0' || value[n] == ';'
				    || value[n] == ' ');
		}
		g_free(line);
	}
	result = status_ok && type_ok;
	if (result && body != NULL) {
		gsize len = 0;
		char *line = g_data_input_stream_read_line(in, &len, NULL,
							   &error);
		if (line != NULL && len <= MAX_LINE) {
			*body = g_strdup(g_strstrip(line));
		} else {
			result = 0;
		}
		g_free(line);
	}

done:
	if (error != NULL) {
		Log_info("snapcast", "%s request: %s", method, error->message);
		g_clear_error(&error);
	}
	g_free(req);
	g_object_unref(in);
	g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
	g_object_unref(conn);
	g_object_unref(client);
	return result;
}

char *snapcast_session_resolve(const char *uri_string) {
	if (!enabled || uri_string == NULL
	    || g_ascii_strncasecmp(uri_string, "http://", 7) != 0) {
		return NULL;
	}
	GUri *uri = g_uri_parse(uri_string, G_URI_FLAGS_ENCODED, NULL);
	if (uri == NULL || g_uri_get_host(uri) == NULL) {
		if (uri) g_uri_unref(uri);
		return NULL;
	}
	char *session = NULL;
	if (request(uri, "HEAD", NULL)) {
		char *body = NULL;
		if (request(uri, "GET", &body)
		    && g_ascii_strncasecmp(body, SNAPCAST_TCP_SCHEME,
					   strlen(SNAPCAST_TCP_SCHEME)) == 0) {
			session = body;
			Log_info("snapcast", "Session %s for %s", session,
				 uri_string);
		} else {
			Log_error("snapcast", "%s: offered a session, but "
				  "answered '%s'", uri_string,
				  body ? body : "");
			g_free(body);
		}
	}
	g_uri_unref(uri);
	return session;
}
