/* snapcast_session - play Snapcast sessions offered by a media server (PoC).
 *
 * gstreamer-snapcast-rs, docs/dlna-snapcast-protocol.md: a media server offers
 * synchronized multiroom playback of an item as a Snapcast session, by content
 * negotiation on the item's ordinary http:// resource. Asked with an Accept
 * header that lists Snapcast transports (e.g. "audio/x-snapcast-tcp"), it answers
 * with one of them as Content-Type and the session's URI (one line, e.g.
 * snapcast-tcp://...); any other server answers with the media. Only transports
 * whose URI scheme an installed GStreamer source handles are asked for (snapsrc:
 * snapcast-tcp).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _SNAPCAST_SESSION_H
#define _SNAPCAST_SESSION_H

// Turn the negotiation on or off (off by default). Call after GStreamer is
// initialized: turning it on checks which Snapcast transports installed
// GStreamer sources can play, and logs a warning if none (then it stays off).
void snapcast_session_enable(int enable);

// If enabled and uri is an http:// URL whose server offers a Snapcast session for
// it, returns the session's snapcast-tcp:// URI (free with g_free()). Otherwise,
// and on any error, NULL: play uri as it is. Blocks for at most a few seconds.
char *snapcast_session_resolve(const char *uri);

#endif /* _SNAPCAST_SESSION_H */
