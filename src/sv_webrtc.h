/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

// sv_webrtc.h - QuakeWorld clients over WebRTC data channels
//
// Clients reach the server through a broker speaking FTE's WebRTC broker
// protocol (FTE's master, or an FTE server with net_enable_rtcbroker 1): the
// server registers there as a host, answers the clients' offers, and their
// game packets then travel over a data channel, with their real addresses
// (IPv4 or IPv6) marked webrtc.
//
// Include after qwsvdef.h.

#ifndef __SV_WEBRTC_H__
#define __SV_WEBRTC_H__

#ifdef WEBRTC

// sv_webrtc.c
void	SV_WebRTC_Init (void);		// cvars and commands
void	SV_WebRTC_Frame (void);		// once a server frame: starts, stops, signals, times out
void	SV_WebRTC_Shutdown (void);
qbool	SV_WebRTC_GetPacket (netadr_t *from, sizebuf_t *message);
void	SV_WebRTC_SendPacket (int length, const void *data, netadr_t to);

// sv_webrtc_cert.c
// Loads the certificate and key PEM files (relative to the base directory),
// or generates and writes both when neither exists. On success returns the
// PEM texts (Q_malloc'd) and the certificate's SHA-256 fingerprint, as in an
// SDP's a=fingerprint line ("AB:CD:..."; 96 bytes are enough).
qbool	SV_WebRTC_LoadCertificate (const char *certfile, const char *keyfile,
			char **cert_pem, char **key_pem, char *fingerprint, size_t fingerprint_size);
// Reads the CA certificates to check a TLS server's certificate with: cafile's
// (relative to the base directory), or the system's when it is empty. Returns
// them as PEM text (Q_malloc'd), or NULL; about says how many and from where,
// or why there are none.
char	*SV_WebRTC_LoadCAs (const char *cafile, char *about, size_t about_size);

#endif // WEBRTC

#endif // __SV_WEBRTC_H__
