/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

	
*/
// net.c

#ifdef SERVERONLY
#include "qwsvdef.h"
#include "sv_webrtc.h"
#else
#include "quakedef.h"
#include "server.h"
#endif

#ifdef _WIN32
WSADATA		winsockdata;
#endif

// BSD style sockaddr with length field, getnameinfo() there may reject a sockaddr without it.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define NET_HAVE_SA_LEN
#endif

#ifndef SERVERONLY
netadr_t	net_local_cl_ipadr;
#endif

#ifndef CLIENTONLY
netadr_t	net_local_sv_ipadr;
netadr_t	net_local_sv_ip6adr;

cvar_t		sv_local_addr = {"sv_local_addr", "", CVAR_ROM};
cvar_t		sv_local_addr6 = {"sv_local_addr6", "", CVAR_ROM};
#endif

netadr_t	net_from;
sizebuf_t	net_message;

static byte	net_message_buffer[MSG_BUF_SIZE];

// forward definition.
qbool NET_GetPacketEx (netsrc_t netsrc, qbool delay);
void NET_SendPacketEx (netsrc_t netsrc, int length, void *data, netadr_t to, qbool delay);

//=============================================================================
//
// LOOPBACK defs.
//

#define MAX_LOOPBACK 4 // must be a power of two

typedef struct
{
	byte	data[MSG_BUF_SIZE];
	int		datalen;
} loopmsg_t;

typedef struct
{
	loopmsg_t	msgs[MAX_LOOPBACK];
	unsigned int	get, send;
} loopback_t;

loopback_t	loopbacks[2];

//=============================================================================
//
// Delayed packets, CLIENT ONLY.
//

#ifndef SERVERONLY

static cl_delayed_packet_t cl_delayed_packets_input[CL_MAX_DELAYED_PACKETS]; // from server to our client.
static cl_delayed_packet_t cl_delayed_packets_output[CL_MAX_DELAYED_PACKETS]; // from our client to server.

//
// Attempt to read packet from network and if succeed we put that packet in array/queue (so they are delayed).
// Packet will be unqued/extracted and used/parsed later, that depends of cl_delay_packet variable.
//
void CL_QueInputPacket(void)
{
	int i;

	// read packets.
	while (NET_GetPacketEx(NS_CLIENT, false))
	{
		// queue one packet.
		for (i = 0; i < CL_MAX_DELAYED_PACKETS; i++)
		{
			if (cl_delayed_packets_input[i].time)
				continue; // busy slot

			// we found unused slot, copy packet, get it later, later depends of cl_delay_packet
			memmove(cl_delayed_packets_input[i].data, net_message.data, net_message.cursize);
			cl_delayed_packets_input[i].length = net_message.cursize;
			cl_delayed_packets_input[i].addr = net_from;
			cl_delayed_packets_input[i].time = Sys_DoubleTime() + 0.001 * bound(0, 0.5 * cl_delay_packet.value, CL_MAX_PACKET_DELAY);
			break; // queued OK.
		}

		if (i >= CL_MAX_DELAYED_PACKETS)
		{
			Con_DPrintf("CL_QueInputPacket: cl_delayed_packets_input overflowed\n");
			break;
		}
	}

	return;
}

//
// Check if delayed output packets must be sent right now.
//
void CL_UnqueOutputPacket(qbool sendall)
{
	int i;
	double time = Sys_DoubleTime();

	for (i = 0; i < CL_MAX_DELAYED_PACKETS; i++)
	{
		if (!cl_delayed_packets_output[i].time)
			continue; // unused slot
		if (cl_delayed_packets_output[i].time > time && !sendall)
			continue; // we are not yet ready for send

		// ok, send it
		NET_SendPacketEx(NS_CLIENT, cl_delayed_packets_output[i].length, 
			cl_delayed_packets_output[i].data, cl_delayed_packets_output[i].addr, false);

		cl_delayed_packets_output[i].time = 0; // mark as unused slot

// perhaps there other packets should be sent
//		return;
	}
}

//
// Get packet from input queue.
//
qbool NET_GetDelayedPacket (netsrc_t netsrc, netadr_t *from, sizebuf_t *message)
{
	int i;
	double time = Sys_DoubleTime();

	for (i = 0; i < CL_MAX_DELAYED_PACKETS; i++)
	{
		if (!cl_delayed_packets_input[i].time)
			continue; // unused slot
		if (cl_delayed_packets_input[i].time > time)
			continue; // we are not yet ready to get this

		// ok, we got something
		SZ_Clear(message);
		SZ_Write(message, cl_delayed_packets_input[i].data, cl_delayed_packets_input[i].length);
		*from = cl_delayed_packets_input[i].addr;

		cl_delayed_packets_input[i].time = 0; // mark as free slot

		return true;
	}

	return false;
}

//
// Put packet in output queue.
//
void NET_SendDelayedPacket (netsrc_t netsrc, int length, void *data, netadr_t to)
{
	int i;

	for (i = 0; i < CL_MAX_DELAYED_PACKETS; i++)
	{
		if (cl_delayed_packets_output[i].time)
			continue; // busy slot
		// we found unused slot, copy packet, send it later, later depends of cl_delay_packet
		memmove(cl_delayed_packets_output[i].data, data, length);
		cl_delayed_packets_output[i].length = length;
		cl_delayed_packets_output[i].addr = to;
		cl_delayed_packets_output[i].time = Sys_DoubleTime() + 0.001 * bound(0, 0.5 * cl_delay_packet.value, CL_MAX_PACKET_DELAY);

		return;
	}

	Con_DPrintf("NET_SendPacketEx: cl_delayed_packets_output overflowed\n");
	return;
}

#endif

//=============================================================================
//
// Geters.
//

int NET_UDPSVPort (void)
{
	// both address families use the same port, IPv4 may be disabled though.
	if (net_local_sv_ipadr.port)
		return ntohs(net_local_sv_ipadr.port);

	return ntohs(net_local_sv_ip6adr.port);
}

// return UDP socket which is used for sending to/receiving from given address type.
static socket_t NET_GetUDPSocketFor (netsrc_t netsrc, netadrtype_t type)
{
	if (netsrc == NS_SERVER)
	{
#ifdef CLIENTONLY
		Sys_Error("NET_GetUDPSocketFor: Bad netsrc");
		return INVALID_SOCKET;
#else
		if (type == NA_IP)
			return svs.socketip;
		if (type == NA_IPV6)
			return svs.socketip6;
		return INVALID_SOCKET;
#endif
	}
	else
	{
#ifdef SERVERONLY
		Sys_Error("NET_GetUDPSocketFor: Bad netsrc");
		return INVALID_SOCKET;
#else
		return type == NA_IP ? cls.socketip : INVALID_SOCKET;
#endif
	}
}

#ifndef CLIENTONLY
socket_t NET_GetTCPListenSocket (int index)
{
	switch (index)
	{
		case 0: return svs.sockettcp;
		case 1: return svs.sockettcp6;
		default: return INVALID_SOCKET;
	}
}

qbool NET_HasTCPListenSocket (void)
{
	return svs.sockettcp != INVALID_SOCKET || svs.sockettcp6 != INVALID_SOCKET;
}

qbool NET_TCPListenSocketMissing (void)
{
	return (svs.socketip != INVALID_SOCKET && svs.sockettcp == INVALID_SOCKET)
		|| (svs.socketip6 != INVALID_SOCKET && svs.sockettcp6 == INVALID_SOCKET);
}
#endif

//=============================================================================
//
// Converters.
//

socklen_t NetadrToSockadr (const netadr_t *a, struct sockaddr_storage *s)
{
	memset (s, 0, sizeof(*s));

	if (a->webrtc)
		return 0; // its packets go through its data channel

	switch (a->type)
	{
		case NA_IP:
		{
			struct sockaddr_in *s4 = (struct sockaddr_in *)s;

			s4->sin_family = AF_INET;
#ifdef NET_HAVE_SA_LEN
			s4->sin_len = sizeof(*s4);
#endif
			memcpy (&s4->sin_addr, a->ip, sizeof(a->ip));
			s4->sin_port = a->port;
			return sizeof(*s4);
		}
		case NA_IPV6:
		{
			struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)s;

			s6->sin6_family = AF_INET6;
#ifdef NET_HAVE_SA_LEN
			s6->sin6_len = sizeof(*s6);
#endif
			memcpy (&s6->sin6_addr, a->ip6, sizeof(a->ip6));
			s6->sin6_port = a->port;
			s6->sin6_scope_id = a->scope_id;
			return sizeof(*s6);
		}
		case NA_INVALID:
		case NA_LOOPBACK:
		default:
			return 0;
	}
}

qbool SockadrToNetadr (const struct sockaddr_storage *s, netadr_t *a)
{
	memset (a, 0, sizeof(*a)); // NA_INVALID, also clears bytes of the inactive address family

	switch (((const struct sockaddr *)s)->sa_family)
	{
		case AF_INET:
		{
			const struct sockaddr_in *s4 = (const struct sockaddr_in *)s;

			a->type = NA_IP;
			memcpy (a->ip, &s4->sin_addr, sizeof(a->ip));
			a->port = s4->sin_port;
			return true;
		}
		case AF_INET6:
		{
			static const byte v4mapped_prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
			const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)s;
			const byte *addr = (const byte *)&s6->sin6_addr;

			if (!memcmp (addr, v4mapped_prefix, sizeof(v4mapped_prefix)))
			{
				// ::ffff:a.b.c.d is an IPv4 address, treat it like one so IPv4 bans etc apply.
				a->type = NA_IP;
				memcpy (a->ip, addr + sizeof(v4mapped_prefix), sizeof(a->ip));
			}
			else
			{
				a->type = NA_IPV6;
				memcpy (a->ip6, addr, sizeof(a->ip6));
				a->scope_id = s6->sin6_scope_id;
			}
			a->port = s6->sin6_port;
			return true;
		}
		default:
			return false;
	}
}

const byte *NET_AdrBytes (const netadr_t *a, int *len)
{
	switch (a->type)
	{
		case NA_IP:
			*len = sizeof(a->ip);
			return a->ip;
		case NA_IPV6:
			*len = sizeof(a->ip6);
			return a->ip6;
		case NA_INVALID:
		case NA_LOOPBACK:
		default:
			*len = 0;
			return NULL;
	}
}

qbool NET_IsLoopbackAdr (const netadr_t a)
{
	static const byte loopback6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

	switch (a.type)
	{
		case NA_LOOPBACK:
			return true;
		case NA_IP:
			return a.ip[0] == 127;
		case NA_IPV6:
			return !memcmp (a.ip6, loopback6, sizeof(loopback6));
		case NA_INVALID:
		default:
			return false;
	}
}

static qbool NET_IsLinkLocalAdr (const netadr_t *a)
{
	return a->type == NA_IPV6 && a->ip6[0] == 0xfe && (a->ip6[1] & 0xc0) == 0x80; // fe80::/10
}

qbool NET_IsPrivateAdr (const netadr_t a)
{
	switch (a.type)
	{
		case NA_IP:
			// RFC1918 networks, multicast and reserved.
			return a.ip[0] == 10 || (a.ip[0] == 172 && (a.ip[1] & 0xF0) == 16)
				|| (a.ip[0] == 192 && a.ip[1] == 168) || a.ip[0] >= 224;
		case NA_IPV6:
			return NET_IsLinkLocalAdr (&a)
				|| (a.ip6[0] & 0xfe) == 0xfc	// fc00::/7 unique local
				|| a.ip6[0] == 0xff;			// ff00::/8 multicast
		case NA_INVALID:
		case NA_LOOPBACK:
		default:
			return false;
	}
}

// 0.0.0.0 or ::
static qbool NET_IsAnyAdr (const netadr_t *a)
{
	static const byte zero[16] = { 0 };
	int len;
	const byte *b = NET_AdrBytes (a, &len);

	return b && !memcmp (b, zero, len);
}

//=============================================================================
//
// Comparators.
//

qbool NET_CompareBaseAdr (const netadr_t a, const netadr_t b)
{
#ifndef SERVERONLY
	if (a.type == NA_LOOPBACK && b.type == NA_LOOPBACK)
		return true;
#endif

	// a WebRTC client and a UDP one from the same address are different clients
	if (a.type != b.type || a.webrtc != b.webrtc)
		return false;

	switch (a.type)
	{
		case NA_IP:
			return !memcmp (a.ip, b.ip, sizeof(a.ip));
		case NA_IPV6:
			// scope is compared only when both sides have one, link-local addresses typed in by admins have none.
			return !memcmp (a.ip6, b.ip6, sizeof(a.ip6))
				&& (!a.scope_id || !b.scope_id || a.scope_id == b.scope_id);
		case NA_INVALID:
		case NA_LOOPBACK:
		default:
			return false; // NA_INVALID never matches anything.
	}
}

qbool NET_CompareAdr (const netadr_t a, const netadr_t b)
{
#ifndef SERVERONLY
	if (a.type == NA_LOOPBACK && b.type == NA_LOOPBACK)
		return true;
#endif

	return a.port == b.port && NET_CompareBaseAdr (a, b);
}

//=============================================================================
//
// Printors.
//

// longest IPv6 string is a bracketed 45 char address, percent sign and interface name, then the port.
#define NET_ADR_STRLEN 80

static void NET_FormatAdr (const netadr_t *a, qbool withport, char *out, size_t outlen)
{
#ifndef SERVERONLY
	if (a->type == NA_LOOPBACK)
	{
		strlcpy (out, "loopback", outlen);
		return;
	}
#endif

	if (a->type == NA_IPV6)
	{
		struct sockaddr_storage ss;
		char host[NET_ADR_STRLEN - 8]; // leaves room for "[", "]:" and a 5 digit port in out
		socklen_t len = NetadrToSockadr (a, &ss);

		// getnameinfo() gives the canonical compressed form, plus the scope for link-local addresses.
		if (getnameinfo ((struct sockaddr *)&ss, len, host, (socklen_t) sizeof(host), NULL, 0, NI_NUMERICHOST) != 0)
		{
			const byte *b = a->ip6;

			snprintf (host, sizeof(host), "%x:%x:%x:%x:%x:%x:%x:%x",
				(b[0] << 8) | b[1], (b[2] << 8) | b[3], (b[4] << 8) | b[5], (b[6] << 8) | b[7],
				(b[8] << 8) | b[9], (b[10] << 8) | b[11], (b[12] << 8) | b[13], (b[14] << 8) | b[15]);
		}

		if (withport)
			snprintf (out, outlen, "[%s]:%i", host, ntohs (a->port));
		else
			strlcpy (out, host, outlen);
		return;
	}

	// NA_IP, NA_INVALID is printed as 0.0.0.0 like it always was.
	if (withport)
		snprintf (out, outlen, "%i.%i.%i.%i:%i", a->ip[0], a->ip[1], a->ip[2], a->ip[3], ntohs (a->port));
	else
		snprintf (out, outlen, "%i.%i.%i.%i", a->ip[0], a->ip[1], a->ip[2], a->ip[3]);
}

char *NET_AdrToString (const netadr_t a)
{
	static char s[MAX_STRINGS][NET_ADR_STRLEN];
	static int idx = 0;

	idx %= MAX_STRINGS;

	NET_FormatAdr (&a, true, s[idx], sizeof(s[0]));

	return s[idx++];
}

char *NET_BaseAdrToString (const netadr_t a)
{
	static char s[MAX_STRINGS][NET_ADR_STRLEN];
	static int idx = 0;

	idx %= MAX_STRINGS;

	NET_FormatAdr (&a, false, s[idx], sizeof(s[0]));

	return s[idx++];
}

//
// Split "host", "host:port", "[v6]", "[v6]:port" and bare "v6" (two or more colons, no port possible)
// into host and port. Port is 0 when not present.
//
static qbool NET_SplitHostPort (const char *s, char *host, size_t hostlen, int *port)
{
	const char *portstr;
	size_t len;

	*port = 0;

	if (!s || !*s)
		return false;

	if (s[0] == '[')
	{
		const char *end = strchr (s, ']');

		if (!end)
			return false;

		len = end - (s + 1);
		if (len == 0 || len >= hostlen)
			return false;

		memcpy (host, s + 1, len);
		host[len] = 0;

		if (!end[1])
			return true;
		if (end[1] != ':')
			return false;

		portstr = end + 2;
	}
	else
	{
		const char *colon = strchr (s, ':');

		if (colon && strchr (colon + 1, ':'))
		{
			// bare IPv6 address.
			return strlcpy (host, s, hostlen) < hostlen;
		}

		len = colon ? (size_t)(colon - s) : strlen (s);
		if (len == 0 || len >= hostlen)
			return false;

		memcpy (host, s, len);
		host[len] = 0;

		if (!colon)
			return true;

		portstr = colon + 1;
	}

	// "host:" means no port, like it always did.
	*port = atoi (portstr);

	return *port >= 0 && *port <= 65535;
}

//
// Resolve host (without port) to a list of unique addresses.
// Numeric addresses are tried first, so they never cause DNS lookups.
//
static int NET_ResolveHost (const char *host, netadr_t *out, int maxout, qbool numeric_only)
{
	struct addrinfo hints, *res, *ai;
	int n = 0, pass;

	for (pass = 0; pass < (numeric_only ? 1 : 2) && n == 0; pass++)
	{
		memset (&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM; // one result per address instead of one per socket type.
		// AI_ADDRCONFIG is not used on purpose, it hides IPv6 results on hosts which only have IPv6 loopback.
		hints.ai_flags = (pass == 0 ? AI_NUMERICHOST : 0);

		res = NULL;
		if (getaddrinfo (host, NULL, &hints, &res) != 0 || !res)
			continue;

		for (ai = res; ai && n < maxout; ai = ai->ai_next)
		{
			struct sockaddr_storage ss;
			netadr_t adr;
			int i;

			if (!ai->ai_addr || ai->ai_addrlen > sizeof(ss))
				continue;

			memset (&ss, 0, sizeof(ss));
			memcpy (&ss, ai->ai_addr, ai->ai_addrlen);

			if (!SockadrToNetadr (&ss, &adr))
				continue;

			for (i = 0; i < n; i++)
			{
				if (NET_CompareBaseAdr (out[i], adr))
					break;
			}

			if (i == n)
				out[n++] = adr;
		}

		freeaddrinfo (res);
	}

	return n;
}

int NET_StringToAdrList (const char *s, netadr_t *out, int maxout)
{
	char host[256];
	int port, n, i;

	if (maxout <= 0 || !NET_SplitHostPort (s, host, sizeof(host), &port))
		return 0;

	n = NET_ResolveHost (host, out, maxout, false);

	for (i = 0; i < n; i++)
		out[i].port = htons ((unsigned short)port);

	return n;
}

qbool NET_StringToAdrNumeric (const char *s, netadr_t *a)
{
	char host[256];
	int port;

	if (!NET_SplitHostPort (s, host, sizeof(host), &port))
		return false;

	if (NET_ResolveHost (host, a, 1, true) != 1)
		return false;

	a->port = htons ((unsigned short)port);

	return true;
}

qbool NET_StringToAdr (const char *s, netadr_t *a)
{
	netadr_t list[8];
	qbool have4 = true, have6 = false;
	int n, i, pick = -1;

#ifndef SERVERONLY
	if (!strcmp(s, "local"))
	{
		memset(a, 0, sizeof(*a));
		a->type = NA_LOOPBACK;
		return true;
	}
#endif

	n = NET_StringToAdrList (s, list, sizeof(list) / sizeof(list[0]));
	if (n <= 0)
		return false;

#ifndef CLIENTONLY
	// prefer address family of an open server socket, IPv4 if both or none are open.
	have4 = svs.socketip != INVALID_SOCKET;
	have6 = svs.socketip6 != INVALID_SOCKET;
	if (!have4 && !have6)
		have4 = true;
#endif

	for (i = 0; i < n && pick < 0; i++)
	{
		if (have4 && list[i].type == NA_IP)
			pick = i;
	}

	for (i = 0; i < n && pick < 0; i++)
	{
		if (have6 && list[i].type == NA_IPV6)
			pick = i;
	}

	*a = list[pick < 0 ? 0 : pick];

	return true;
}

/*
=============================================================================
LOOPBACK BUFFERS FOR LOCAL PLAYER
=============================================================================
*/

qbool NET_GetLoopPacket (netsrc_t netsrc, netadr_t *from, sizebuf_t *message)
{
	int i;
	loopback_t *loop;

	loop = &loopbacks[netsrc];

	if (loop->send - loop->get > MAX_LOOPBACK)
		loop->get = loop->send - MAX_LOOPBACK;

	if (loop->get >= loop->send)
		return false;

	i = loop->get & (MAX_LOOPBACK-1);
	loop->get++;

	if (message->maxsize < loop->msgs[i].datalen)
		Sys_Error("NET_SendLoopPacket: Loopback buffer was too big");

	memcpy (message->data, loop->msgs[i].data, loop->msgs[i].datalen);
	message->cursize = loop->msgs[i].datalen;
	memset (from, 0, sizeof(*from));
	from->type = NA_LOOPBACK;
	return true;
}

void NET_SendLoopPacket (netsrc_t netsrc, int length, void *data, netadr_t to)
{
	int i;
	loopback_t *loop;

	loop = &loopbacks[netsrc ^ 1];

	i = loop->send & (MAX_LOOPBACK - 1);
	loop->send++;

	if (length > (int) sizeof(loop->msgs[i].data))
		Sys_Error ("NET_SendLoopPacket: length > %d", (int) sizeof(loop->msgs[i].data));

	memcpy (loop->msgs[i].data, data, length);
	loop->msgs[i].datalen = length;
}

void NET_ClearLoopback (void)
{
	loopbacks[0].send = loopbacks[0].get = 0;
	loopbacks[1].send = loopbacks[1].get = 0;
}

//=============================================================================
//
// SV TCP connection.
//

// allocate, may link it in, if requested
svtcpstream_t *sv_tcp_connection_new(socket_t sock, netadr_t from, char *buf, int buf_len, qbool link)
{
	svtcpstream_t *st = NULL;

	st = Q_malloc(sizeof(svtcpstream_t));
	st->waitingforprotocolconfirmation = true;
	st->socketnum = sock;
	st->remoteaddr = from;
	if (buf_len > 0 && buf_len < sizeof(st->inbuffer))
	{
		memmove(st->inbuffer, buf, buf_len);
		st->inlen = buf_len;
	}
	else
		st->drop = true; // yeah, funny

	// link it in if requested
	if (link)
	{
		st->next = svs.tcpstreams;
		svs.tcpstreams = st;
	}

	return st;
}

// free data, may unlink it out if requested
static void sv_tcp_connection_free(svtcpstream_t *drop, qbool unlink)
{
	if (!drop)
		return; // someone kidding us

	// unlink if requested
	if (unlink)
	{
		if (svs.tcpstreams == drop)
		{
			svs.tcpstreams = svs.tcpstreams->next;
		}
		else
		{
			svtcpstream_t *st = NULL;

			for (st = svs.tcpstreams; st; st = st->next)
			{
				if (st->next == drop)
				{
					st->next = st->next->next;
					break;
				}
			}
		}
	}

	// well, think socket may be zero, but most of the time zero is stdin fd, so better not close it
	if (drop->socketnum && drop->socketnum != INVALID_SOCKET)
		closesocket(drop->socketnum);

	Q_free(drop);
}

int sv_tcp_connection_count(void)
{
	svtcpstream_t *st = NULL;
	int cnt = 0;

	for (st = svs.tcpstreams; st; st = st->next)
		cnt++;

	return cnt;
}

//=============================================================================

static qbool NET_GetUDPPacketFromSocket (socket_t sock, netadr_t *from_adr, sizebuf_t *message)
{
	int ret, err;
	struct sockaddr_storage from;
	socklen_t fromlen;

	memset (&from, 0, sizeof(from));
	fromlen = sizeof(from);
	ret = recvfrom (sock, (char *)message->data, message->maxsize, 0, (struct sockaddr *)&from, &fromlen);
	SockadrToNetadr (&from, from_adr); // NA_INVALID when nothing was received.

	if (ret == -1)
	{
		err = qerrno;

		if (err == EWOULDBLOCK)
			return false; // common error, does not spam in logs.

		if (err == EMSGSIZE)
		{
			Con_DPrintf ("Warning: Oversize packet from %s\n", NET_AdrToString (*from_adr));
			return false;
		}

		if (err == ECONNABORTED || err == ECONNRESET)
		{
			Con_DPrintf ("Connection lost or aborted\n");
			return false;
		}

		Con_Printf ("NET_GetPacket: recvfrom: (%i): %s\n", err, strerror(err));
		return false;
	}

	if (from_adr->type == NA_INVALID)
		return false; // unsupported address family.

	if (ret >= message->maxsize)
	{
		Con_Printf ("Oversize packet from %s\n", NET_AdrToString (*from_adr));
		return false;
	}

	message->cursize = ret;

	return ret;
}

qbool NET_GetUDPPacket (netsrc_t netsrc, netadr_t *from_adr, sizebuf_t *message)
{
	static const netadrtype_t types[] = { NA_IP, NA_IPV6 };
	int i;

	for (i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i++)
	{
		socket_t sock = NET_GetUDPSocketFor(netsrc, types[i]);

		if (sock != INVALID_SOCKET && NET_GetUDPPacketFromSocket(sock, from_adr, message))
			return true;
	}

	return false;
}

#ifndef SERVERONLY
qbool NET_GetTCPPacket_CL (netsrc_t netsrc, netadr_t *from, sizebuf_t *message)
{
	int ret, err;

	if (netsrc != NS_CLIENT || cls.sockettcp == INVALID_SOCKET)
		return false;

	ret = recv(cls.sockettcp, (char *) cls.tcpinbuffer + cls.tcpinlen, sizeof(cls.tcpinbuffer) - cls.tcpinlen, 0);

	// FIXME: should we check for ret == 0  for disconnect ???

	if (ret == -1)
	{
		err = qerrno;

		if (err == EWOULDBLOCK)
		{
			ret = 0; // hint for code below that it was not cricial error.
		}
		else if (err == ECONNABORTED || err == ECONNRESET)
		{
			Con_Printf ("Connection lost or aborted\n"); //server died/connection lost.
		}
		else
		{
			Con_Printf ("NET_GetPacket: Error (%i): %s\n", err, strerror(err));
		}

		if (ret)
		{
			// error detected, close socket then.
			closesocket(cls.sockettcp);
			cls.sockettcp = INVALID_SOCKET;
			return false;
		}
	}

	cls.tcpinlen += ret;

	if (cls.tcpinlen < 2)
		return false;

	message->cursize = BigShort(*(short*)cls.tcpinbuffer);
	if (message->cursize >= message->maxsize)
	{
		closesocket(cls.sockettcp);
		cls.sockettcp = INVALID_SOCKET;
		Con_Printf ("Warning: Oversize packet from %s\n", NET_AdrToString (cls.sockettcpdest));
		return false;
	}

	if (message->cursize + 2 > cls.tcpinlen)
	{
		//not enough buffered to read a packet out of it.
		return false;
	}

	memcpy(message->data, cls.tcpinbuffer + 2, message->cursize);
	memmove(cls.tcpinbuffer, cls.tcpinbuffer + message->cursize + 2, cls.tcpinlen - (message->cursize + 2));
	cls.tcpinlen -= message->cursize + 2;

	*from = cls.sockettcpdest;

	return true;
}
#endif

#ifndef CLIENTONLY
qbool NET_GetTCPPacket_SV (netsrc_t netsrc, netadr_t *from, sizebuf_t *message)
{
	int ret;
	float timeval = Sys_DoubleTime();
	svtcpstream_t *st = NULL, *next = NULL;

	if (netsrc != NS_SERVER)
		return false;

	for (st = svs.tcpstreams; st; st = next)
	{
		next = st->next;

		*from = st->remoteaddr;

		if (st->socketnum == INVALID_SOCKET || st->drop)
		{
			sv_tcp_connection_free(st, true); // free and unlink
			continue;
		}

		//due to the above checks about invalid sockets, the socket is always open for st below.

		// check for client timeout
		if (st->timeouttime < timeval)
		{
			st->drop = true;
			continue;
		}

		ret = recv(st->socketnum, st->inbuffer+st->inlen, sizeof(st->inbuffer)-st->inlen, 0);
		if (ret == 0)
		{
			// connection closed
			st->drop = true;
			continue;
		}
		else if (ret == -1)
		{
			int err = qerrno;

			if (err == EWOULDBLOCK)
			{
				ret = 0; // it's OK
			}
			else
			{
				if (err == ECONNABORTED || err == ECONNRESET)
				{
					Con_DPrintf ("Connection lost or aborted\n"); //server died/connection lost.
				}
				else
				{
					Con_DPrintf ("NET_GetPacket: Error (%i): %s\n", err, strerror(err));
				}

				st->drop = true;
				continue;
			}
		}
		else
		{
			// update timeout
			st->timeouttime = Sys_DoubleTime() + 10;
		}

		st->inlen += ret;

		if (st->waitingforprotocolconfirmation)
		{
			// not enough data
			if (st->inlen < 6)
				continue;

			if (strncmp(st->inbuffer, "qizmo\n", 6))
			{
				Con_Printf ("Unknown TCP client\n");
				st->drop = true;
				continue;
			}

			// remove leading 6 bytes
			memmove(st->inbuffer, st->inbuffer+6, st->inlen - (6));
			st->inlen -= 6;
			// confirmed
			st->waitingforprotocolconfirmation = false;
		}

		// need two bytes for packet len
		if (st->inlen < 2)
			continue;

		message->cursize = BigShort(*(short*)st->inbuffer);
		if (message->cursize < 0)
		{
			message->cursize = 0;
			Con_Printf ("Warning: malformed message from %s\n", NET_AdrToString (*from));
			st->drop = true;
			continue;
		}

		if (message->cursize >= message->maxsize)
		{
			Con_Printf ("Warning: Oversize packet from %s\n", NET_AdrToString (*from));
			st->drop = true;
			continue;
		}

		if (message->cursize + 2 > st->inlen)
		{
			//not enough buffered to read a packet out of it.
			continue;
		}

		memcpy(message->data, st->inbuffer + 2, message->cursize);
		memmove(st->inbuffer, st->inbuffer + message->cursize + 2, st->inlen - (message->cursize + 2));
		st->inlen -= message->cursize + 2;

		return true; // we got packet!
	}

	return false; // no packet received.
}
#endif

qbool NET_GetPacketEx (netsrc_t netsrc, qbool delay)
{
#ifndef SERVERONLY
	if (delay)
		return NET_GetDelayedPacket(netsrc, &net_from, &net_message);
#endif

#ifndef SERVERONLY
	if (NET_GetLoopPacket(netsrc, &net_from, &net_message))
		return true;
#endif

	if (NET_GetUDPPacket(netsrc, &net_from, &net_message))
		return true;

// TCPCONNECT -->
#ifndef SERVERONLY
	if (netsrc == NS_CLIENT && cls.sockettcp != INVALID_SOCKET && NET_GetTCPPacket_CL(netsrc, &net_from, &net_message))
		return true;
#endif

#ifndef CLIENTONLY
	if (netsrc == NS_SERVER && svs.tcpstreams && NET_GetTCPPacket_SV(netsrc, &net_from, &net_message))
		return true;
#endif
// <--TCPCONNECT

#ifdef WEBRTC
	if (netsrc == NS_SERVER && SV_WebRTC_GetPacket(&net_from, &net_message))
		return true;
#endif

	return false;
}

qbool NET_GetPacket (netsrc_t netsrc)
{
#ifdef SERVERONLY
	qbool delay = false;
#else
	qbool delay = (netsrc == NS_CLIENT && cl_delay_packet.integer);
#endif

	return NET_GetPacketEx (netsrc, delay);
}

//=============================================================================

#ifndef SERVERONLY
qbool NET_SendTCPPacket_CL (netsrc_t netsrc, int length, void *data, netadr_t to)
{
	unsigned short slen;

	if (netsrc != NS_CLIENT || cls.sockettcp == INVALID_SOCKET)
		return false;

	if (!NET_CompareAdr(to, cls.sockettcpdest))
		return false;

	// this goes to the server so send it via TCP.
	slen = BigShort((unsigned short)length);
	// FIXME: CHECK send() result, we use NON BLOCKIN MODE, FFS!
	send(cls.sockettcp, (char*)&slen, sizeof(slen), 0);
	send(cls.sockettcp, data, length, 0);

	return true;
}
#endif

#ifndef CLIENTONLY
qbool NET_SendTCPPacket_SV (netsrc_t netsrc, int length, void *data, netadr_t to)
{
	svtcpstream_t *st;

	if (netsrc != NS_SERVER)
		return false;

	for (st = svs.tcpstreams; st; st = st->next)
	{
		if (st->socketnum == INVALID_SOCKET)
			continue;

		if (NET_CompareAdr(to, st->remoteaddr))
		{
			int sent;
			unsigned short slen = BigShort((unsigned short)length);

			if (st->outlen + length + sizeof(slen) >= sizeof(st->outbuffer))
			{
				// not enough space, we overflowed
				break; // well, quake should resist to some packet lost.. so we just drop that packet.
			}

			// put data in buffer
			memmove(st->outbuffer + st->outlen, (char*)&slen, sizeof(slen));
			st->outlen += sizeof(slen);
			memmove(st->outbuffer + st->outlen, data, length);
			st->outlen += length;

			sent = send(st->socketnum, st->outbuffer, st->outlen, 0);

			if (sent == 0)
			{
				// think it's OK
			}
			else if (sent > 0) //we put some data through
			{ //move up the buffer
				st->outlen -= sent;
				memmove(st->outbuffer, st->outbuffer + sent, st->outlen);
			}
			else
			{ //error of some kind. would block or something
				if (qerrno != EWOULDBLOCK && qerrno != EAGAIN)
				{
					st->drop = true; // something cricial, drop than
				}
			}

			break;
		}
	}

	// 'st' will be not zero, if we found 'to' in 'svs.tcpstreams'.
	// That does not mean we actualy send packet, since there case of overflow, but who cares,
	// all is matter that we found such 'to' and tried to send packet.
	return !!st;
}
#endif

qbool NET_SendUDPPacket (netsrc_t netsrc, int length, void *data, netadr_t to)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	socket_t sock;
	int ret;

	addrlen = NetadrToSockadr (&to, &addr);
	if (!addrlen)
	{
		Con_DPrintf ("NET_SendPacket: bad address type %d\n", (int)to.type);
		return false;
	}

	sock = NET_GetUDPSocketFor(netsrc, to.type);
	if (sock == INVALID_SOCKET)
	{
		Con_DPrintf ("NET_SendPacket: %s is not enabled, dropping packet to %s\n",
			to.type == NA_IPV6 ? "IPv6" : "IPv4", NET_AdrToString (to));
		return false;
	}

	ret = sendto (sock, data, length, 0, (struct sockaddr *)&addr, addrlen);
	if (ret == -1)
	{
		int err = qerrno;

		if (err == EWOULDBLOCK || err == ECONNREFUSED || err == EADDRNOTAVAIL)
			; // nothing
		else
			Con_Printf ("NET_SendPacket: sendto: (%i): %s %i\n", err, strerror(err), (int)sock);
	}

	return true;
}

void NET_SendPacketEx (netsrc_t netsrc, int length, void *data, netadr_t to, qbool delay)
{
#ifndef SERVERONLY
	if (delay)
	{
		NET_SendDelayedPacket (netsrc, length, data, to);
		return;
	}
#endif

#ifndef SERVERONLY
	if (to.type == NA_LOOPBACK)
	{
		NET_SendLoopPacket (netsrc, length, data, to);
		return;
	}
#endif

// TCPCONNECT -->
#ifndef SERVERONLY
	if (netsrc == NS_CLIENT && cls.sockettcp != INVALID_SOCKET && NET_SendTCPPacket_CL(netsrc, length, data, to))
		return;
#endif

#ifndef CLIENTONLY
	if (netsrc == NS_SERVER && svs.tcpstreams && NET_SendTCPPacket_SV(netsrc, length, data, to))
		return;
#endif
// <--TCPCONNECT

#ifdef WEBRTC
	if (to.webrtc)
	{
		// its data channel; as UDP, never mind if it is gone
		if (netsrc == NS_SERVER)
			SV_WebRTC_SendPacket(length, data, to);
		return;
	}
#endif

	NET_SendUDPPacket(netsrc, length, data, to);
}

void NET_SendPacket (netsrc_t netsrc, int length, void *data, netadr_t to)
{
#ifdef SERVERONLY
	qbool delay = false;
#else
	qbool delay = (netsrc == NS_CLIENT && cl_delay_packet.integer);
#endif

	NET_SendPacketEx (netsrc, length, data, to, delay);
}

//=============================================================================

qbool TCP_Set_KEEPALIVE(socket_t sock)
{
	int		iOptVal = 1;

	if (sock == INVALID_SOCKET)
	{
		Con_Printf("TCP_Set_KEEPALIVE: invalid socket\n");
		return false;
	}

	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*)&iOptVal, sizeof(iOptVal)) == SOCKET_ERROR)
	{
		Con_Printf ("TCP_Set_KEEPALIVE: setsockopt: (%i): %s\n", qerrno, strerror (qerrno));
		return false;
	}

#if defined(__linux__)

//	The time (in seconds) the connection needs to remain idle before TCP starts sending keepalive probes, 
//  if the socket option SO_KEEPALIVE has been set on this socket.

	iOptVal = 60;

	if (setsockopt(sock, SOL_TCP, TCP_KEEPIDLE, (void*)&iOptVal, sizeof(iOptVal)) == -1)
	{
		Con_Printf ("TCP_Set_KEEPALIVE: setsockopt TCP_KEEPIDLE: (%i): %s\n", qerrno, strerror(qerrno));
		return false;
	}

//  The time (in seconds) between individual keepalive probes.
	iOptVal = 30;

	if (setsockopt(sock, SOL_TCP, TCP_KEEPINTVL, (void*)&iOptVal, sizeof(iOptVal)) == -1)
	{
		Con_Printf ("TCP_Set_KEEPALIVE: setsockopt TCP_KEEPINTVL: (%i): %s\n", qerrno, strerror(qerrno));
		return false;
	}

//  The maximum number of keepalive probes TCP should send before dropping the connection. 
	iOptVal = 6;

	if (setsockopt(sock, SOL_TCP, TCP_KEEPCNT, (void*)&iOptVal, sizeof(iOptVal)) == -1)
	{
		Con_Printf ("TCP_Set_KEEPALIVE: setsockopt TCP_KEEPCNT: (%i): %s\n", qerrno, strerror(qerrno));
		return false;
	}
#else
	// FIXME: windows, bsd etc...
#endif

	return true;
}

socket_t TCP_OpenStream (netadr_t remoteaddr)
{
	unsigned long _true = true;
	socket_t newsocket;
	socklen_t addrlen;
	struct sockaddr_storage qs;

	if (!(addrlen = NetadrToSockadr(&remoteaddr, &qs)))
	{
		Con_Printf ("TCP_OpenStream: bad address type %d\n", (int)remoteaddr.type);
		return INVALID_SOCKET;
	}

	if ((newsocket = socket (((struct sockaddr *)&qs)->sa_family, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
	{
		Con_Printf ("TCP_OpenStream: socket: (%i): %s\n", qerrno, strerror(qerrno));
		return INVALID_SOCKET;
	}

	if (connect (newsocket, (struct sockaddr *)&qs, addrlen) == SOCKET_ERROR)
	{
		Con_Printf ("TCP_OpenStream: connect: (%i): %s\n", qerrno, strerror(qerrno));
		closesocket(newsocket);
		return INVALID_SOCKET;
	}

#ifndef _WIN32
	if ((fcntl (newsocket, F_SETFL, O_NONBLOCK)) == -1)
	{ // O'Rly?! @@@
		Con_Printf ("TCP_OpenStream: fcntl: (%i): %s\n", qerrno, strerror(qerrno));
		closesocket(newsocket);
		return INVALID_SOCKET;
	}
#endif

	if (ioctlsocket (newsocket, FIONBIO, &_true) == -1)
	{ // make asynchronous
		Con_Printf ("TCP_OpenStream: ioctl: (%i): %s\n", qerrno, strerror(qerrno));
		closesocket(newsocket);
		return INVALID_SOCKET;
	}

	return newsocket;
}

//
// Fill in bind address for given family, bindaddr NULL or empty means any address.
// Only numeric addresses are accepted, IPv6 addresses may be put in brackets and may have a scope.
//
static socklen_t NET_MakeBindAddr (int family, const char *bindaddr, unsigned short int port, struct sockaddr_storage *out)
{
	netadrtype_t type = (family == AF_INET6) ? NA_IPV6 : NA_IP;
	netadr_t adr;

	memset (&adr, 0, sizeof(adr)); // all zero address is 0.0.0.0 or ::
	adr.type = type;

	// a port is not allowed, it is given with -port.
	if (bindaddr && *bindaddr && (!NET_StringToAdrNumeric (bindaddr, &adr) || adr.type != type || adr.port))
	{
		Con_Printf ("Bad bind address '%s' for %s\n", bindaddr, family == AF_INET6 ? "IPv6" : "IPv4");
		return 0;
	}

	adr.port = htons (port == PORT_ANY ? 0 : port);

	return NetadrToSockadr (&adr, out);
}

//
// Create non blocking socket of given family and type and bind it.
//
static socket_t NET_OpenBoundSocket (int family, int type, const char *bindaddr, unsigned short int port, const char *caller)
{
	socket_t newsocket;
	struct sockaddr_storage address;
	socklen_t addrlen;
	unsigned long nonblocking = true;

	if (!(addrlen = NET_MakeBindAddr (family, bindaddr, port, &address)))
		return INVALID_SOCKET;

	if ((newsocket = socket (family, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP)) == INVALID_SOCKET)
	{
		Con_Printf ("%s: socket: (%i): %s\n", caller, qerrno, strerror(qerrno));
		return INVALID_SOCKET;
	}

	if (family == AF_INET6)
	{
		// IPv4 has its own socket, so IPv6 socket must not take IPv4 traffic (or the port) from it.
		int on = 1;

		if (setsockopt (newsocket, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&on, sizeof(on)) == SOCKET_ERROR)
		{
			Con_Printf ("%s: setsockopt IPV6_V6ONLY: (%i): %s\n", caller, qerrno, strerror(qerrno));
			closesocket(newsocket);
			return INVALID_SOCKET;
		}
	}

#ifndef _WIN32
	if (type == SOCK_STREAM)
	{
		// allow listening again right away while old connections are in TIME_WAIT, it does not allow two listeners.
		// not on windows, there it would allow other processes to steal the port.
		int on = 1;

		if (setsockopt (newsocket, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) == SOCKET_ERROR)
			Con_DPrintf ("%s: setsockopt SO_REUSEADDR: (%i): %s\n", caller, qerrno, strerror(qerrno));
	}

	if ((fcntl (newsocket, F_SETFL, O_NONBLOCK)) == -1)
	{ // O'Rly?! @@@
		Con_Printf ("%s: fcntl: (%i): %s\n", caller, qerrno, strerror(qerrno));
		closesocket(newsocket);
		return INVALID_SOCKET;
	}
#endif

	if (ioctlsocket (newsocket, FIONBIO, &nonblocking) == -1)
	{ // make asynchronous
		Con_Printf ("%s: ioctl: (%i): %s\n", caller, qerrno, strerror(qerrno));
		closesocket(newsocket);
		return INVALID_SOCKET;
	}

	if (bind (newsocket, (struct sockaddr *)&address, addrlen) == SOCKET_ERROR)
	{
		Con_Printf ("%s: bind: (%i): %s\n", caller, qerrno, strerror(qerrno));
		closesocket(newsocket);
		return INVALID_SOCKET;
	}

	Con_DPrintf ("Binding %s %s socket to %s\n", type == SOCK_STREAM ? "TCP" : "UDP",
		family == AF_INET6 ? "IPv6" : "IPv4", bindaddr && *bindaddr ? bindaddr : "any address");

	return newsocket;
}

socket_t TCP_OpenListenSocket (int family, const char *bindaddr, unsigned short int port)
{
	socket_t newsocket = NET_OpenBoundSocket (family, SOCK_STREAM, bindaddr, port, "TCP_OpenListenSocket");

	if (newsocket == INVALID_SOCKET)
		return INVALID_SOCKET;

	if (listen (newsocket, 2) == SOCKET_ERROR)
	{
		Con_Printf ("TCP_OpenListenSocket: listen: (%i): %s\n", qerrno, strerror(qerrno));
		closesocket(newsocket);
		return INVALID_SOCKET;
	}

	if (!TCP_Set_KEEPALIVE(newsocket))
	{
		Con_Printf ("TCP_OpenListenSocket: TCP_Set_KEEPALIVE: failed\n");
		closesocket(newsocket);
		return INVALID_SOCKET;
	}

	return newsocket;
}

socket_t UDP_OpenSocket (int family, const char *bindaddr, unsigned short int port)
{
	return NET_OpenBoundSocket (family, SOCK_DGRAM, bindaddr, port, "UDP_OpenSocket");
}

#ifdef WEBRTC
// WebRTC packets arrive on libdatachannel's threads; a byte sent to this
// loopback socket, which NET_Sleep waits on too, has the main loop read them
// at once rather than at the end of its select() timeout.
static socket_t net_wakesocket = INVALID_SOCKET;

qbool NET_WakeInit (void)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);
	socket_t s;
#ifdef _WIN32
	unsigned long _true = true;
#endif

	if (net_wakesocket != INVALID_SOCKET)
		return true;
	if ((s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET)
		return false;

	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;

	if (
#ifdef _WIN32
		ioctlsocket(s, FIONBIO, &_true) == -1
#else
		fcntl(s, F_SETFL, O_NONBLOCK) == -1
#endif
		|| bind(s, (struct sockaddr *) &address, sizeof(address)) == -1
		|| getsockname(s, (struct sockaddr *) &address, &length) == -1
		|| connect(s, (struct sockaddr *) &address, sizeof(address)) == -1) // to itself only
	{
		closesocket(s);
		return false;
	}

	net_wakesocket = s;
	return true;
}

void NET_WakeShutdown (void)
{
	if (net_wakesocket != INVALID_SOCKET)
	{
		closesocket(net_wakesocket);
		net_wakesocket = INVALID_SOCKET;
	}
}

void NET_Wake (void)
{
	socket_t s = net_wakesocket;

	if (s != INVALID_SOCKET)
		send(s, "", 1, 0); // when its buffer is full a wakeup is due anyway
}
#endif

qbool NET_Sleep(int msec, qbool stdinissocket)
{
	struct timeval	timeout;
	fd_set			fdset;
	qbool			stdin_ready = false;
	int				maxfd = 0;

	FD_ZERO (&fdset);

	if (stdinissocket)
	{
		FD_SET (0, &fdset); // stdin is processed too (tends to be socket 0)
		maxfd = max(0, maxfd);
	}

	if (svs.socketip != INVALID_SOCKET)
	{
		FD_SET(svs.socketip, &fdset); // network socket, IPv4
		maxfd = max((int)svs.socketip, maxfd);
	}

	if (svs.socketip6 != INVALID_SOCKET)
	{
		FD_SET(svs.socketip6, &fdset); // network socket, IPv6
		maxfd = max((int)svs.socketip6, maxfd);
	}

#ifdef WEBRTC
	if (net_wakesocket != INVALID_SOCKET)
	{
		FD_SET(net_wakesocket, &fdset);
		maxfd = max((int)net_wakesocket, maxfd);
	}
#endif

	timeout.tv_sec = msec/1000;
	timeout.tv_usec = (msec%1000)*1000;
	switch (select(maxfd + 1, &fdset, NULL, NULL, &timeout))
	{
		case -1: break;
		case  0: break;
		default:

		if (stdinissocket)
			stdin_ready = FD_ISSET (0, &fdset);

#ifdef WEBRTC
		if (net_wakesocket != INVALID_SOCKET && FD_ISSET(net_wakesocket, &fdset))
		{
			char drain[64];

			while (recv(net_wakesocket, drain, sizeof(drain), 0) > 0)
				;
		}
#endif

		break;
	}

	return stdin_ready;
}

void NET_GetLocalAddress (socket_t sock, netadr_t *out)
{
	char buff[512];
	struct sockaddr_storage address;
	socklen_t namelen;
	netadr_t list[16];
	int n, i, pick = -1;

	memset (out, 0, sizeof(*out)); // NA_INVALID

	if (sock == INVALID_SOCKET)
		return;

	memset (&address, 0, sizeof(address));
	namelen = sizeof(address);
	if (getsockname (sock, (struct sockaddr *)&address, &namelen) == -1 || !SockadrToNetadr (&address, out))
	{
		memset (out, 0, sizeof(*out));
#ifndef SERVERONLY
		Com_Printf_State (PRINT_FAIL, "Couldn't detect local ip\n");
#endif
		return;
	}

	if (NET_IsAnyAdr (out))
	{
		// socket was bound to any address, change it to what the machine says it is, rather than the socket.
		strlcpy (buff, "localhost", sizeof (buff));
		gethostname (buff, sizeof (buff));
		buff[sizeof(buff) - 1] = 0;

		n = NET_StringToAdrList (buff, list, sizeof(list) / sizeof(list[0]));

		// prefer address of the same family which other hosts can reach.
		for (i = 0; i < n && pick < 0; i++)
		{
			if (list[i].type == out->type && !NET_IsLoopbackAdr (list[i]) && !NET_IsLinkLocalAdr (&list[i]))
				pick = i;
		}

		for (i = 0; i < n && pick < 0; i++)
		{
			if (list[i].type == out->type)
				pick = i;
		}

		if (pick >= 0)
		{
			memcpy (out->ip, list[pick].ip, sizeof(out->ip));
			memcpy (out->ip6, list[pick].ip6, sizeof(out->ip6));
			out->scope_id = list[pick].scope_id;
		}
		else if (out->type == NA_IP)
		{
			out->ip[0] = 127; // 127.0.0.1
			out->ip[3] = 1;
		}
		else
		{
			out->ip6[15] = 1; // ::1
		}
	}

#ifndef SERVERONLY
	Com_Printf_State (PRINT_OK, "IP address %s\n", NET_AdrToString (*out));
#endif
}

void NET_Init (void)
{
#ifdef _WIN32
	WORD wVersionRequested;
	int r;

	wVersionRequested = MAKEWORD(2, 2);
	r = WSAStartup (wVersionRequested, &winsockdata);
	if (r)
		Sys_Error ("Winsock initialization failed.");
#endif

	// init the message buffer
	SZ_Init (&net_message, net_message_buffer, sizeof(net_message_buffer));

	Con_DPrintf("UDP Initialized\n");

#ifndef SERVERONLY
	cls.socketip = INVALID_SOCKET;
// TCPCONNECT -->
	cls.sockettcp = INVALID_SOCKET;
// <--TCPCONNECT
#endif

#ifndef CLIENTONLY

	Cvar_Register (&sv_local_addr);
	Cvar_Register (&sv_local_addr6);

	svs.socketip = INVALID_SOCKET;
	svs.socketip6 = INVALID_SOCKET;
// TCPCONNECT -->
	svs.sockettcp = INVALID_SOCKET;
	svs.sockettcp6 = INVALID_SOCKET;
// <--TCPCONNECT
#endif

#ifdef SERVERONLY
	// As client+server we init it in SV_SpawnServer().
	// As serveronly we do it here.
	NET_InitServer();
#endif
}

void NET_Shutdown (void)
{
#ifndef CLIENTONLY
	NET_CloseServer();
#endif

#ifndef SERVERONLY
	NET_CloseClient();
#endif

#ifdef _WIN32
	WSACleanup ();
#endif
}

#ifndef SERVERONLY
void NET_InitClient(void)
{
	int port = PORT_CLIENT;
	int p;

	p = COM_CheckParm ("-clientport");
	if (p && p < COM_Argc())
	{
		port = atoi(COM_Argv(p+1));
	}

	if (cls.socketip == INVALID_SOCKET)
		cls.socketip = UDP_OpenSocket (AF_INET, NULL, port);

	if (cls.socketip == INVALID_SOCKET)
		cls.socketip = UDP_OpenSocket (AF_INET, NULL, PORT_ANY); // any dynamic port

	if (cls.socketip == INVALID_SOCKET)
		Sys_Error ("Couldn't allocate client socket");

	// determine my name & address
	NET_GetLocalAddress (cls.socketip, &net_local_cl_ipadr);

	Com_Printf_State (PRINT_OK, "Client port Initialized\n");
}

void NET_CloseClient (void)
{
	if (cls.socketip != INVALID_SOCKET)
	{
		closesocket(cls.socketip);
		cls.socketip = INVALID_SOCKET;
	}

// TCPCONNECT -->
	// FIXME: is it OK? Probably we should send disconnect?
	if (cls.sockettcp != INVALID_SOCKET)
	{
		closesocket(cls.sockettcp);
		cls.sockettcp = INVALID_SOCKET;
	}
// <--TCPCONNECT
}

#endif

#ifndef CLIENTONLY

//
// Command line options which control on which addresses server sockets are opened.
//
typedef struct
{
	const char	*bind4;		// -ip, NULL means any address
	const char	*bind6;		// -ip6, NULL means any address
	qbool		want4;
	qbool		want6;
} netbindopts_t;

static void NET_ParseBindOptions (netbindopts_t *o)
{
	int p;

	memset (o, 0, sizeof(*o));

	if ((p = COM_CheckParm ("-ip")) && p + 1 < COM_Argc())
		o->bind4 = COM_Argv (p + 1);

	if ((p = COM_CheckParm ("-ip6")) && p + 1 < COM_Argc())
		o->bind6 = COM_Argv (p + 1);

	o->want4 = !COM_CheckParm ("-noipv4");
	o->want6 = !COM_CheckParm ("-noipv6");

	// binding one address family to a specific address means the other family is not wanted, unless it is bound too.
	if (o->bind4 && !o->bind6)
		o->want6 = false;
	if (o->bind6 && !o->bind4)
		o->want4 = false;
}

//
// Open server TCP port, one socket per address family which has UDP socket open.
// NOTE: Zero port will actually close already opened port.
//
void NET_InitServer_TCP(unsigned short int port)
{
	// close sockets first.
	if (svs.sockettcp != INVALID_SOCKET || svs.sockettcp6 != INVALID_SOCKET)
	{
		Con_Printf("Server TCP port closed\n");

		if (svs.sockettcp != INVALID_SOCKET)
		{
			closesocket(svs.sockettcp);
			svs.sockettcp = INVALID_SOCKET;
		}

		if (svs.sockettcp6 != INVALID_SOCKET)
		{
			closesocket(svs.sockettcp6);
			svs.sockettcp6 = INVALID_SOCKET;
		}
	}

	NET_OpenServer_TCP (port);
}

void NET_OpenServer_TCP(unsigned short int port)
{
	netbindopts_t o;

	if (!port)
		return;

	NET_ParseBindOptions (&o);

	if (svs.socketip != INVALID_SOCKET && svs.sockettcp == INVALID_SOCKET)
	{
		svs.sockettcp = TCP_OpenListenSocket (AF_INET, o.bind4, port);
		Con_Printf("%s server TCP port %u (IPv4)\n", svs.sockettcp != INVALID_SOCKET ? "Opening" : "Failed to open", (unsigned int)port);
	}

	if (svs.socketip6 != INVALID_SOCKET && svs.sockettcp6 == INVALID_SOCKET)
	{
		svs.sockettcp6 = TCP_OpenListenSocket (AF_INET6, o.bind6, port);
		Con_Printf("%s server TCP port %u (IPv6)\n", svs.sockettcp6 != INVALID_SOCKET ? "Opening" : "Failed to open", (unsigned int)port);
	}
}

void NET_InitServer (void)
{
	int port = PORT_SERVER;
	int p;
	netbindopts_t o;

	p = COM_CheckParm ("-port");
	if (p && p + 1 < COM_Argc())
	{
		port = atoi(COM_Argv(p+1));
	}

	NET_ParseBindOptions (&o);

	if (!o.want4 && !o.want6)
	{
		Sys_Error ("NET_InitServer: both IPv4 and IPv6 are disabled, check -ip, -ip6, -noipv4 and -noipv6");
		return;
	}

	if (o.want4 && svs.socketip == INVALID_SOCKET)
	{
		svs.socketip = UDP_OpenSocket (AF_INET, o.bind4, (unsigned short) port);

		if (svs.socketip == INVALID_SOCKET)
		{
#ifdef SERVERONLY
			Sys_Error
#else
			Con_Printf
#endif
				("WARNING: Couldn't allocate server IPv4 socket\n");
		}
	}

	if (o.want6 && svs.socketip6 == INVALID_SOCKET)
	{
		svs.socketip6 = UDP_OpenSocket (AF_INET6, o.bind6, (unsigned short) port);

		if (svs.socketip6 == INVALID_SOCKET)
		{
			if (o.bind6 || !o.want4)
			{
				// IPv6 was asked for explicitly.
#ifdef SERVERONLY
				Sys_Error
#else
				Con_Printf
#endif
					("WARNING: Couldn't allocate server IPv6 socket\n");
			}
			else
			{
				Con_Printf ("IPv6 is not available, server is IPv4 only\n");
			}
		}
	}

	NET_GetLocalAddress (svs.socketip, &net_local_sv_ipadr);
	NET_GetLocalAddress (svs.socketip6, &net_local_sv_ip6adr);
	Cvar_SetROM (&sv_local_addr, net_local_sv_ipadr.type != NA_INVALID ? NET_AdrToString (net_local_sv_ipadr) : "");
	Cvar_SetROM (&sv_local_addr6, net_local_sv_ip6adr.type != NA_INVALID ? NET_AdrToString (net_local_sv_ip6adr) : "");

	if (svs.socketip == INVALID_SOCKET && svs.socketip6 == INVALID_SOCKET)
	{
#ifdef SERVERONLY
		Sys_Error
#else
		Con_Printf
#endif
			("WARNING: Couldn't allocate server socket\n");
	}
}

void NET_CloseServer (void)
{
	if (svs.socketip != INVALID_SOCKET)
	{
		closesocket(svs.socketip);
		svs.socketip = INVALID_SOCKET;
	}

	if (svs.socketip6 != INVALID_SOCKET)
	{
		closesocket(svs.socketip6);
		svs.socketip6 = INVALID_SOCKET;
	}

	// NOTE: port is kept, so NET_UDPSVPort() keeps working after close.
	net_local_sv_ipadr.type = NA_INVALID;
	net_local_sv_ip6adr.type = NA_INVALID;

// TCPCONNECT -->
	NET_InitServer_TCP(0);
// <--TCPCONNECT
}
#endif

