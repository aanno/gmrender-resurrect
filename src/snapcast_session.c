/* snapcast_session - play Snapcast sessions offered by a media server (PoC).
 *
 * See snapcast_session.h. The negotiation: a HEAD request for the resource whose
 * Accept header lists the Snapcast transports this renderer can play (one MIME
 * type per transport, see transports[]). If the answer has one of them as its
 * Content-Type, a GET with the same header returns the session URI. Servers that
 * know nothing of Snapcast answer the HEAD with the media's type, so they cost one
 * small extra request.
 *
 * Which transports can be played is GStreamer's knowledge: a transport counts if
 * an installed source element handles its URI scheme (snapsrc: snapcast-tcp).
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
#ifdef HAVE_GST
#  include <gst/gst.h>
#endif

#include "logging.h"

// Snapcast transports, most preferred first: the URI scheme of a session and the
// MIME type that names it in the negotiation.
static const struct {
	const char *scheme;
	const char *mime_type;
} transports[] = {
	{ "snapcast-tcp", "audio/x-snapcast-tcp" },  // Snapcast's binary protocol
	{ "snapcast-ws", "audio/x-snapcast-ws" },    // reserved: over WebSocket
};
#define N_TRANSPORTS (sizeof(transports) / sizeof(transports[0]))
// Seconds for connecting and for each read.
#define TIMEOUT_S 3
// Longest header or body line read.
#define MAX_LINE 4096

static int enabled = 0;
// The transports this renderer can play, and the Accept header listing them;
// determined on first use (GStreamer is initialized by then).
static int playable[N_TRANSPORTS];
static char *accept_header = NULL;

static const char *playable_accept_header(void) {
	if (accept_header != NULL)
		return *accept_header ? accept_header : NULL;
	GString *accept = g_string_new(NULL);
	double q = 1.0;
	for (size_t i = 0; i < N_TRANSPORTS; i++) {
#ifdef HAVE_GST
		playable[i] = gst_uri_protocol_is_supported(
			GST_URI_SRC, transports[i].scheme);
#endif
		if (!playable[i])
			continue;
		if (accept->len)
			g_string_append(accept, ", ");
		if (q >= 1.0)
			g_string_append(accept, transports[i].mime_type);
		else
			g_string_append_printf(accept, "%s;q=%.1f",
					       transports[i].mime_type, q);
		q -= 0.1;
	}
	accept_header = g_string_free(accept, FALSE);
	if (*accept_header)
		Log_info("snapcast", "Asking servers for Snapcast sessions: "
			 "Accept: %s", accept_header);
	else
		Log_error("snapcast", "Warning: --snapcast-sessions is on, but no "
			  "GStreamer source handles a Snapcast URI scheme "
			  "(snapsrc not installed?), so sessions are never asked "
			  "for; playing media only");
	return *accept_header ? accept_header : NULL;
}

// The playable transport whose MIME type is the Content-Type value, or -1.
static int transport_of_type(const char *value) {
	for (size_t i = 0; i < N_TRANSPORTS; i++) {
		size_t n = strlen(transports[i].mime_type);
		if (playable[i]
		    && g_ascii_strncasecmp(value, transports[i].mime_type, n) == 0
		    && (value[n] == '\0' || value[n] == ';' || value[n] == ' '))
			return (int) i;
	}
	return -1;
}

static const char *playable_accept_header(void);

void snapcast_session_enable(int enable) {
	enabled = enable;
	if (enabled)
		playable_accept_header();  // logs what will be asked for
}

// Sends one request for uri; returns the transport if the response is a 200
// with the Content-Type of a playable transport, else -1. With body != NULL, also
// reads the first body line into *body (g_free()).
static int request(GUri *uri, const char *method, const char *accept,
		   char **body) {
	int result = -1;
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
		return -1;
	}

	const char *path = g_uri_get_path(uri);
	const char *query = g_uri_get_query(uri);
	// IPv6 literals need brackets in the Host header.
	char *host_header = strchr(host, ':')
		? g_strdup_printf("[%s]:%d", host, port)
		: g_strdup_printf("%s:%d", host, port);
	char *req = g_strdup_printf(
		"%s %s%s%s HTTP/1.0\r\nHost: %s\r\nAccept: %s"
		"\r\nConnection: close\r\n\r\n",
		method, *path ? path : "/", query ? "?" : "", query ? query : "",
		host_header, accept);
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

	int status_ok = 0, transport = -1;
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
			transport = transport_of_type(g_strstrip(line + 13));
		}
		g_free(line);
	}
	result = status_ok ? transport : -1;
	if (result >= 0 && body != NULL) {
		gsize len = 0;
		char *line = g_data_input_stream_read_line(in, &len, NULL,
							   &error);
		if (line != NULL && len <= MAX_LINE) {
			*body = g_strdup(g_strstrip(line));
		} else {
			result = -1;
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
	const char *accept = playable_accept_header();
	if (accept == NULL) {
		g_uri_unref(uri);
		return NULL;
	}
	char *session = NULL;
	if (request(uri, "HEAD", accept, NULL) >= 0) {
		char *body = NULL;
		int t = request(uri, "GET", accept, &body);
		// The URI's scheme must be the transport the Content-Type named.
		char *scheme = t >= 0 && body ? g_uri_parse_scheme(body) : NULL;
		if (scheme && g_ascii_strcasecmp(scheme,
						 transports[t].scheme) == 0) {
			session = body;
			Log_info("snapcast", "Session %s for %s", session,
				 uri_string);
		} else {
			Log_error("snapcast", "%s: offered a session, but "
				  "answered '%s'", uri_string,
				  body ? body : "");
			g_free(body);
		}
		g_free(scheme);
	}
	g_uri_unref(uri);
	return session;
}
