/* snapcast_session - play Snapcast sessions offered by a media server (PoC).
 *
 * gstreamer-snapcast-rs, docs/dlna-snapcast-protocol.md: a media server offers
 * synchronized multiroom playback of an item as a Snapcast session, by content
 * negotiation on the item's ordinary http:// resource. Asked with
 * "Accept: audio/x-snapcast-tcp", it answers with that Content-Type and a
 * snapcast-tcp:// URI (one line); any other server answers with the media.
 * Playing the snapcast-tcp:// URI needs GStreamer's snapsrc element.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _SNAPCAST_SESSION_H
#define _SNAPCAST_SESSION_H

// Turn the negotiation on or off (off by default).
void snapcast_session_enable(int enable);

// If enabled and uri is an http:// URL whose server offers a Snapcast session for
// it, returns the session's snapcast-tcp:// URI (free with g_free()). Otherwise,
// and on any error, NULL: play uri as it is. Blocks for at most a few seconds.
char *snapcast_session_resolve(const char *uri);

#endif /* _SNAPCAST_SESSION_H */
