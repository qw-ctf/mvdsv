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

// sv_webrtc.c - QuakeWorld clients over WebRTC data channels (libdatachannel)
//
// The server opens a WebSocket to a broker speaking FTE's protocol (FTE's
// master, or an FTE server with net_enable_rtcbroker 1) as "rtc_host" and gets
// a room; clients connect to rtc://broker/room. For each client the broker
// relays the client's offer and candidates, the server answers, and the
// client's packets then arrive on a data channel. FTE's native client opens
// no channel of its own and reads the one the server opens, so the server
// always opens one ("quake", unordered and never retransmitted); browsers and
// SoftWorld open one too, and either is read.
//
// A connection lasts as long as the game client it carries: one that does
// not join the game soon after opening, or stays after its player has gone,
// is closed. Connections still connecting have a pool of their own, so that a
// flood of them through the broker displaces only each other.
//
// Threads: libdatachannel calls back on its own threads. Those callbacks only
// queue events, mark peers with packets waiting, or wake the main loop
// (NET_Wake); everything else, packets included, happens on the main thread.

#ifdef WEBRTC

#include "qwsvdef.h"
#include "sv_webrtc.h"

#include <stdint.h>
#include <rtc/rtc.h>

#define RTC_BROKER_PROTOCOL	"FTE-Quake"	// the first token of FTE's com_protocolname for Quake
#define RTC_BROKER_PORT		27950		// FTE's PORT_ICEBROKER

#define MAX_RTCPEERS		(MAX_CLIENTS + 4)
#define MAX_RTCCONNECTING	8		// peers whose channel is not open yet; the oldest makes way
#define MAX_RTCJOINING		4		// open peers from one IP not in the game yet
#define MAX_RTCLOGS			256		// libdatachannel's log lines waiting for the main thread
#define MAX_RTCSERVERS		8
#define RTC_MAX_MESSAGE		65536			// largest data channel or broker message
#define RTC_MAX_BUFFERED	(64 * 1024)		// bytes waiting on a channel before packets are dropped

#define RTC_CONNECT_TIME	20.0	// seconds for a new client's channel to open
#define RTC_JOIN_TIME		10.0	// seconds an open peer may be without a client in the game
#define RTC_BANNED_TIME		2.0		// seconds a banned address keeps its peer, for the ban message
#define RTC_BROKER_TIMEOUT	15.0	// seconds for the broker's WebSocket to open, and to greet
#define RTC_BROKER_RETRY	10.0	// seconds between attempts to reach the broker
#define RTC_NAMEINUSE_RETRY	60.0	// seconds before asking again for a room that was taken
#define RTC_INFO_TIME		30.0	// seconds between serverinfo updates to the broker
#define RTC_FLUSH_TIME		0.25	// seconds to let queued packets out when stopping

// a bit for each peer in rtc_waiting
typedef char rtc_peers_fit_in_64_bits[MAX_RTCPEERS <= 64 ? 1 : -1];

// FTE's broker messages (engine/common/netinc.h), in binary WebSocket frames
// of [type][client number, 16 bits little-endian][text]
enum
{
	ICEMSG_PEERLOST = 0,	// a client left, or (-1) the broker is going
	ICEMSG_GREETING = 1,	// broker to host: the room, "FTE-Quake/<room>"
	ICEMSG_NEWPEER = 2,		// broker to host: a client came, "<address>\0<relays>\0"
	ICEMSG_OFFER = 3,		// either way: an offer or an answer
	ICEMSG_CANDIDATE = 4,	// either way: an ICE candidate
	ICEMSG_SERVERINFO = 6,	// host to broker: the serverinfo, for server lists
	ICEMSG_NAMEINUSE = 8	// broker to host: the room is taken (this one has no client number)
};

static cvar_t sv_webrtc                 = {"sv_webrtc", "0"};
static cvar_t sv_webrtc_broker          = {"sv_webrtc_broker", "wss://master.frag-net.com:27950"};
static cvar_t sv_webrtc_broker_ca       = {"sv_webrtc_broker_ca", ""};
static cvar_t sv_webrtc_broker_insecure = {"sv_webrtc_broker_insecure", "0"};
static cvar_t sv_webrtc_room            = {"sv_webrtc_room", ""};
static cvar_t sv_webrtc_port            = {"sv_webrtc_port", "0"};
static cvar_t sv_webrtc_stun            = {"sv_webrtc_stun", ""};
static cvar_t sv_webrtc_cert            = {"sv_webrtc_cert", "cert.pem"};
static cvar_t sv_webrtc_key             = {"sv_webrtc_key", "privkey.pem"};
static cvar_t sv_webrtc_mtu             = {"sv_webrtc_mtu", "0"};
static cvar_t sv_webrtc_debug           = {"sv_webrtc_debug", "0"};

typedef struct
{
	qbool		inuse;
	int			brokerid;	// the broker's number for the client, -1 once the broker no longer matters
	int			pc;			// peer connection
	int			dc_remote;	// the channel the client opened, or -1
	int			dc_local;	// the channel the server opened, or -1
	int			dc_send;	// the open one packets go on (the client's first), or -1
	int			state;		// rtcState
	netadr_t	adr;		// a webrtc one once a channel is open, NA_INVALID before
	qbool		relayed;	// through a TURN relay at the client's end: adr is the relay's
	qbool		banned;		// adr is banned: the peer lasts for the ban message only
	qbool		joined;		// a game client has had adr
	char		from[64];	// the broker's (partial) address for the client, for messages
	double		created;
	double		lastrecv;	// the last packet, for webrtc_status
	double		lastclient;	// when a game client last had adr (or the channel opened)
} rtcpeer_t;

static struct
{
	qbool		running;			// started, with a certificate
	qbool		failed;				// could not start with config; waits for a change
	char		config[512];		// what a change of restarts everything
	char		*cert_pem, *key_pem;
	char		fingerprint[96];
	char		bind[64];			// the address libjuice binds, empty for any of either family
	qbool		ipv4, ipv6;			// the families of addresses the server has, and so WebRTC

	int			ws;					// the broker's WebSocket, -1 when there is none
	qbool		ws_open;
	char		ws_config[512];		// sv_webrtc_broker and sv_webrtc_room it was opened with
	char		host[128];
	int			port;
	qbool		tls;
	char		room[64];			// the room hosted, once greeted
	int			failures;			// attempts in a row that did not reach the broker
	double		ws_started, ws_opened, retry, nextinfo;

	rtcpeer_t	peers[MAX_RTCPEERS];
	int			nextpeer;			// where SV_WebRTC_GetPacket looks first
	int			loglevel;
} rtc;

/*
=============================================================================

From libdatachannel's threads to the main one

=============================================================================
*/

typedef enum { EV_DESCRIPTION, EV_CANDIDATE, EV_STATE, EV_CHANNEL, EV_LOG } rtceventkind_t;

typedef struct rtcevent_s
{
	rtceventkind_t		kind;
	int					slot;	// rtc.peers index, -1 for none
	int					pc;		// to tell a reused slot from the one an event was for
	int					value;	// EV_STATE: rtcState, EV_CHANNEL: channel, EV_LOG: rtcLogLevel
	char				*text;
	char				*extra;
	struct rtcevent_s	*next;
} rtcevent_t;

// all of these under rtc_lock
static mutex_t		rtc_lock;
static rtcevent_t	*rtc_first, *rtc_last;	// a queue: only log lines are ever dropped from it
static int			rtc_logs;				// log lines in it
static uint64_t		rtc_waiting;			// a bit for each peer with packets perhaps waiting

static void RTC_FreeEvent (rtcevent_t *ev)
{
	free(ev->text);
	free(ev->extra);
	free(ev);
}

static void RTC_PushEvent (rtceventkind_t kind, int slot, int pc, int value, const char *text, const char *extra)
{
	// calloc and strdup, not the Q_ ones: no Sys_Error off the main thread
	rtcevent_t *ev = calloc(1, sizeof(*ev));

	if (!ev)
		return;
	ev->kind = kind;
	ev->slot = slot;
	ev->pc = pc;
	ev->value = value;
	ev->text = text ? strdup(text) : NULL;
	ev->extra = extra ? strdup(extra) : NULL;
	if ((text && !ev->text) || (extra && !ev->extra))
	{
		RTC_FreeEvent(ev);
		return;
	}

	Sys_MutexLock(&rtc_lock);
	if (kind == EV_LOG && rtc_logs >= MAX_RTCLOGS)
	{
		Sys_MutexUnlock(&rtc_lock);
		RTC_FreeEvent(ev); // log lines piling up: lose this one
		return;
	}
	if (kind == EV_LOG)
		rtc_logs++;
	if (rtc_last)
		rtc_last->next = ev;
	else
		rtc_first = ev;
	rtc_last = ev;
	Sys_MutexUnlock(&rtc_lock);

	if (kind != EV_LOG)
		NET_Wake();
}

static rtcevent_t *RTC_PopEvent (void)
{
	rtcevent_t *ev;

	Sys_MutexLock(&rtc_lock);
	if ((ev = rtc_first))
	{
		if (!(rtc_first = ev->next))
			rtc_last = NULL;
		if (ev->kind == EV_LOG)
			rtc_logs--;
	}
	Sys_MutexUnlock(&rtc_lock);
	return ev;
}

static void RTC_SetWaiting (int slot, qbool waiting)
{
	if (slot < 0 || slot >= MAX_RTCPEERS)
		return;
	Sys_MutexLock(&rtc_lock);
	if (waiting)
		rtc_waiting |= (uint64_t) 1 << slot;
	else
		rtc_waiting &= ~((uint64_t) 1 << slot);
	Sys_MutexUnlock(&rtc_lock);
}

static uint64_t RTC_Waiting (void)
{
	uint64_t waiting;

	Sys_MutexLock(&rtc_lock);
	waiting = rtc_waiting;
	Sys_MutexUnlock(&rtc_lock);
	return waiting;
}

// libdatachannel's callbacks. A peer connection's user pointer is its slot
// + 1, and so is its channels' (libdatachannel gives a channel the client
// opens its peer connection's; the server sets its own channel's).
#define RTC_SLOT(ptr) ((int) (intptr_t) (ptr) - 1)

static void RTC_API RTC_OnDescription (int pc, const char *sdp, const char *type, void *ptr)
{
	RTC_PushEvent(EV_DESCRIPTION, RTC_SLOT(ptr), pc, 0, sdp, type);
}

static void RTC_API RTC_OnCandidate (int pc, const char *candidate, const char *mid, void *ptr)
{
	RTC_PushEvent(EV_CANDIDATE, RTC_SLOT(ptr), pc, 0, candidate, mid);
}

static void RTC_API RTC_OnState (int pc, rtcState state, void *ptr)
{
	RTC_PushEvent(EV_STATE, RTC_SLOT(ptr), pc, (int) state, NULL, NULL);
}

static void RTC_API RTC_OnDataChannel (int pc, int dc, void *ptr)
{
	RTC_PushEvent(EV_CHANNEL, RTC_SLOT(ptr), pc, dc, NULL, NULL);
}

static void RTC_API RTC_OnAvailable (int id, void *ptr)
{
	RTC_SetWaiting(RTC_SLOT(ptr), true); // the broker's WebSocket has no slot
	NET_Wake(); // the main thread reads it
}

static void RTC_API RTC_OnLog (rtcLogLevel level, const char *message)
{
	RTC_PushEvent(EV_LOG, -1, -1, (int) level, message, NULL);
}

/*
=============================================================================

JSON, as much as the signalling needs: flat objects of strings and numbers

=============================================================================
*/

// Reads a JSON string from just past its opening quote into out (when not
// NULL), and returns where it ends, past its closing quote, or NULL.
static const char *RTC_JsonRead (const char *s, char *out, size_t size)
{
	size_t n = 0;
	unsigned int code;
	int i;
	char c;

	while (*s && *s != '"')
	{
		c = *s++;
		if (c == '\\')
		{
			switch ((c = *s++))
			{
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'u':
				for (i = 0, code = 0; i < 4; i++, s++)
				{
					if (!isxdigit((unsigned char) *s))
						return NULL;
					code = code * 16 + (isdigit((unsigned char) *s) ? *s - '0' : (tolower((unsigned char) *s) - 'a' + 10));
				}
				c = code < 0x80 ? (char) code : '?'; // the signalling is ASCII
				break;
			case 0:
				return NULL;
			default:
				break; // \" \\ \/, and FTE's \' and \$, stand for themselves
			}
		}
		if (out)
		{
			if (n + 1 >= size)
				return NULL;
			out[n++] = c;
		}
	}

	if (*s != '"')
		return NULL;
	if (out)
		out[n] = 0;
	return s + 1;
}

static const char *RTC_JsonSpace (const char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	return s;
}

// The string value of key in a flat JSON object.
static qbool RTC_JsonString (const char *json, const char *key, char *out, size_t size)
{
	char name[64];
	const char *s = RTC_JsonSpace(json);
	qbool match;

	if (*s++ != '{')
		return false;

	for (;;)
	{
		s = RTC_JsonSpace(s);
		if (*s != '"' || !(s = RTC_JsonRead(s + 1, name, sizeof(name))))
			return false;
		match = !strcmp(name, key);

		s = RTC_JsonSpace(s);
		if (*s++ != ':')
			return false;
		s = RTC_JsonSpace(s);

		if (*s == '"')
		{
			if (!(s = RTC_JsonRead(s + 1, match ? out : NULL, size)))
				return false;
			if (match)
				return true;
		}
		else
		{
			if (match)
				return false; // not a string
			while (*s && *s != ',' && *s != '}')
				s++; // a number, true, false or null
		}

		s = RTC_JsonSpace(s);
		if (*s++ != ',')
			return false; // the end, without key
	}
}

static qbool RTC_JsonEscape (const char *in, char *out, size_t size)
{
	size_t n = 0, length;
	char single[8];
	const char *put;

	for (; *in; in++)
	{
		switch (*in)
		{
		case '"': put = "\\\""; break;
		case '\\': put = "\\\\"; break;
		case '\n': put = "\\n"; break;
		case '\r': put = "\\r"; break;
		case '\t': put = "\\t"; break;
		default:
			if ((unsigned char) *in < 0x20)
				snprintf(single, sizeof(single), "\\u%04x", (unsigned char) *in);
			else
			{
				single[0] = *in;
				single[1] = 0;
			}
			put = single;
			break;
		}
		length = strlen(put);
		if (n + length >= size)
			return false;
		memcpy(out + n, put, length);
		n += length;
	}
	out[n] = 0;
	return true;
}

/*
=============================================================================

Peers

=============================================================================
*/

static const char *RTC_StateName (int state)
{
	switch (state)
	{
	case RTC_NEW: return "new";
	case RTC_CONNECTING: return "connecting";
	case RTC_CONNECTED: return "connected";
	case RTC_DISCONNECTED: return "disconnected";
	case RTC_FAILED: return "failed";
	case RTC_CLOSED: return "closed";
	default: return "?";
	}
}

static const char *RTC_PeerName (const rtcpeer_t *p)
{
	if (p->adr.webrtc)
		return va("WebRTC client %s", NET_AdrToString(p->adr));
	return va("WebRTC client #%d (%s)", p->brokerid, p->from[0] ? p->from : "?");
}

static void RTC_ClearPeer (rtcpeer_t *p)
{
	memset(p, 0, sizeof(*p));
	p->brokerid = p->pc = p->dc_remote = p->dc_local = p->dc_send = -1;
}

static void RTC_DropPeer (rtcpeer_t *p, const char *why)
{
	if (!p->inuse)
		return;

	if (why && ((p->adr.webrtc && !p->banned) || sv_webrtc_debug.value))
		Con_Printf("WebRTC: %s dropped: %s\n", RTC_PeerName(p), why);

	// deleting a peer connection leaves its channels to be deleted
	if (p->dc_remote >= 0)
		rtcDeleteDataChannel(p->dc_remote);
	if (p->dc_local >= 0)
		rtcDeleteDataChannel(p->dc_local);
	rtcDeletePeerConnection(p->pc);
	RTC_SetWaiting((int) (p - rtc.peers), false);
	RTC_ClearPeer(p);
}

static rtcpeer_t *RTC_BrokerPeer (int brokerid)
{
	int i;

	if (brokerid < 0)
		return NULL;
	for (i = 0; i < MAX_RTCPEERS; i++)
		if (rtc.peers[i].inuse && rtc.peers[i].brokerid == brokerid)
			return &rtc.peers[i];
	return NULL;
}

static qbool RTC_ChannelOpen (int dc)
{
	return dc >= 0 && rtcIsOpen(dc);
}

// the client's own channel when it has one open, else the server's
static void RTC_ChooseChannel (rtcpeer_t *p)
{
	p->dc_send = RTC_ChannelOpen(p->dc_remote) ? p->dc_remote : RTC_ChannelOpen(p->dc_local) ? p->dc_local : -1;
}

// a game client has the peer's address
static qbool RTC_InGame (const rtcpeer_t *p)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		if (svs.clients[i].state >= cs_preconnected && NET_CompareAdr(svs.clients[i].netchan.remote_address, p->adr))
			return true;
	return false;
}

// "host:port" from rtcGetRemoteAddress, as a WebRTC address. libjuice writes
// an IPv6 host without brackets, so the port is after the last colon. Never
// looks a name up.
static qbool RTC_ParseAddress (const char *s, netadr_t *adr)
{
	const char *colon = strrchr(s, ':');
	char text[128];

	if (!colon || colon == s || strlen(s) + 3 > sizeof(text))
		return false;
	if (memchr(s, ':', colon - s))
		snprintf(text, sizeof(text), "[%.*s]%s", (int) (colon - s), s, colon);
	else
		strlcpy(text, s, sizeof(text));
	if (!NET_StringToAdrNumeric(text, adr) || (adr->type != NA_IP && adr->type != NA_IPV6) || !adr->port)
		return false;
	adr->webrtc = true;
	return true;
}

// One host: an IPv4 address, or an IPv6 /64, as a host has one of those to
// itself.
static qbool RTC_SameHost (const netadr_t *a, const netadr_t *b)
{
	if (a->type == NA_IPV6 && b->type == NA_IPV6)
		return !memcmp(a->ip6, b->ip6, 8);
	return NET_CompareBaseAdr(*a, *b);
}

static const char *RTC_HostName (const netadr_t *adr)
{
	netadr_t network = *adr;

	if (adr->type != NA_IPV6)
		return NET_BaseAdrToString(*adr);
	memset(network.ip6 + 8, 0, 8);
	network.scope_id = 0;
	return va("%s/64", NET_BaseAdrToString(network));
}

// Once a channel is open the peer gets its address (the client's real IP and
// port, or its relay's), and its packets count.
static qbool RTC_PeerReady (rtcpeer_t *p)
{
	char address[128], local[512], remote[512];
	netadr_t adr;
	int i, joining = 0;
	rtcpeer_t *other;

	if (p->adr.webrtc)
		return true;
	if (!RTC_ChannelOpen(p->dc_remote) && !RTC_ChannelOpen(p->dc_local))
		return false;

	if (rtcGetRemoteAddress(p->pc, address, sizeof(address)) < 0)
	{
		RTC_DropPeer(p, "its address is unknown");
		return false;
	}
	if (!RTC_ParseAddress(address, &adr))
	{
		RTC_DropPeer(p, va("its address %s could not be read", address));
		return false;
	}
	for (i = 0; i < MAX_RTCPEERS; i++)
	{
		other = &rtc.peers[i];
		if (other == p || !other->inuse || !other->adr.webrtc)
			continue;
		if (NET_CompareAdr(other->adr, adr))
		{
			RTC_DropPeer(p, va("%s is in use by another client", address));
			return false;
		}
		if (RTC_SameHost(&other->adr, &adr) && !RTC_InGame(other))
			joining++;
	}
	if (joining >= MAX_RTCJOINING)
	{
		RTC_DropPeer(p, va("%d others from %s are joining already", joining, RTC_HostName(&adr)));
		return false;
	}

	p->adr = adr;
	p->lastrecv = p->lastclient = curtime;
	// banned: kept a moment, so that its connect gets the ban message
	p->banned = SV_FilterAddress(&adr);
	p->relayed = rtcGetSelectedCandidatePair(p->pc, local, sizeof(local), remote, sizeof(remote)) >= 0
		&& strstr(remote, " typ relay") != NULL;
	RTC_ChooseChannel(p);
	RTC_SetWaiting((int) (p - rtc.peers), true); // what came before it was ready
	if (!p->banned || sv_webrtc_debug.value)
		Con_Printf("WebRTC: %s connected%s%s\n", RTC_PeerName(p),
			p->relayed ? " through a TURN relay: the address is the relay's" : "", p->banned ? " (banned)" : "");
	return true;
}

// For a URL's user or password: what libdatachannel's url_decode reads back.
static qbool RTC_UrlEncode (const char *in, char *out, size_t size)
{
	size_t n = 0;

	for (; *in; in++)
	{
		if (n + 4 > size)
			return false;
		if (isalnum((unsigned char) *in) || strchr("-._~", *in))
			out[n++] = *in;
		else
			n += snprintf(out + n, size - n, "%%%02X", (unsigned char) *in);
	}
	out[n] = 0;
	return true;
}

// FTE's relays, "turn:host:port?user=u?auth=p" separated by spaces, as
// libdatachannel's "turn:u:p@host:port", with u and p percent-encoded.
static int RTC_Relays (const char *relays, char servers[][256], int count)
{
	char token[256], user[256], auth[256], *u, *a, *query;
	int length, scheme;

	while (*relays && count < MAX_RTCSERVERS)
	{
		while (*relays == ' ')
			relays++;
		for (length = 0; relays[length] && relays[length] != ' '; length++)
			;
		if (!length)
			break;

		if (length < (int) sizeof(token))
		{
			memcpy(token, relays, length);
			token[length] = 0;
			scheme = !strncmp(token, "turn:", 5) ? 5 : !strncmp(token, "turns:", 6) ? 6 : 0;
			// libdatachannel's URLs take no IPv6 literal ("[...]"), and one
			// it cannot read fails the whole peer connection
			if (scheme && token[scheme] != '[')
			{
				u = strstr(token, "?user=");
				a = strstr(token, "?auth=");
				if ((query = strchr(token, '?')))
					*query = 0;
				if (u && a)
				{
					u += 6;
					a += 6;
					u[strcspn(u, "?")] = 0;
					a[strcspn(a, "?")] = 0;
					if (RTC_UrlEncode(u, user, sizeof(user)) && RTC_UrlEncode(a, auth, sizeof(auth))
						&& snprintf(servers[count], 256, "%.*s%s:%s@%s", scheme, token, user, auth, token + scheme) < 256)
						count++;
				}
				else
				{
					strlcpy(servers[count], token, 256);
					count++;
				}
			}
		}
		relays += length;
	}
	return count;
}

static void RTC_NewPeer (int brokerid, const char *from, const char *relays)
{
	char servers[MAX_RTCSERVERS][256];
	const char *list[MAX_RTCSERVERS];
	rtcConfiguration config;
	rtcpeer_t *p, *slot = NULL, *oldest = NULL;
	int i, count = 0, connecting = 0, pc;

	for (i = 0; i < MAX_RTCPEERS; i++)
	{
		p = &rtc.peers[i];
		if (p->inuse && p->brokerid == brokerid)
		{
			// the broker reuses a number once its client has left it
			if (p->adr.webrtc)
				p->brokerid = -1;
			else
				RTC_DropPeer(p, "the broker gave its number to a new client");
		}
		if (p->inuse && !p->adr.webrtc)
		{
			connecting++;
			if (!oldest || p->created < oldest->created)
				oldest = p;
		}
		if (!p->inuse && !slot)
			slot = p;
	}

	// Peers still connecting have a pool of their own: when it is full, or
	// there is no slot, the oldest of them makes way, and the clients in the
	// game stay.
	if (oldest && (connecting >= MAX_RTCCONNECTING || !slot))
	{
		RTC_DropPeer(oldest, "it made way for a new client");
		if (!slot)
			slot = oldest;
	}
	if (!slot)
	{
		Con_Printf("WebRTC: no room for another client (%d connections)\n", MAX_RTCPEERS);
		return;
	}

	// STUN: libjuice takes one server, so the server's own when it has one,
	// else the broker's (FTE's sockets answer it); then the broker's TURN
	// relays
	if (sv_webrtc_stun.string[0])
		snprintf(servers[count++], sizeof(servers[0]), "%s%s",
			strncmp(sv_webrtc_stun.string, "stun:", 5) ? "stun:" : "", sv_webrtc_stun.string);
	else
		snprintf(servers[count++], sizeof(servers[0]), "stun:%s:%d", rtc.host, rtc.port);
	count = RTC_Relays(relays, servers, count);
	for (i = 0; i < count; i++)
		list[i] = servers[i];

	memset(&config, 0, sizeof(config));
	config.iceServers = list;
	config.iceServersCount = count;
	config.bindAddress = rtc.bind[0] ? rtc.bind : NULL; // NULL: a socket for both families (RTC_Bind)
	config.certificatePemFile = rtc.cert_pem;
	config.keyPemFile = rtc.key_pem;
	config.enableIceUdpMux = true; // one UDP port for all clients
	if ((int) sv_webrtc_port.value > 0 && (int) sv_webrtc_port.value <= 65535)
		config.portRangeBegin = config.portRangeEnd = (uint16_t) sv_webrtc_port.value;
	if ((int) sv_webrtc_mtu.value > 0)
		config.mtu = (int) sv_webrtc_mtu.value;
	config.maxMessageSize = RTC_MAX_MESSAGE;

	if ((pc = rtcCreatePeerConnection(&config)) < 0)
	{
		Con_Printf("WebRTC: could not make a peer connection (error %d)\n", pc);
		return;
	}

	RTC_ClearPeer(slot);
	slot->inuse = true;
	slot->brokerid = brokerid;
	slot->pc = pc;
	slot->state = RTC_NEW;
	slot->created = curtime;
	strlcpy(slot->from, from, sizeof(slot->from));
	RTC_SetWaiting((int) (slot - rtc.peers), false);

	rtcSetUserPointer(pc, (void *) (intptr_t) (slot - rtc.peers + 1));
	rtcSetLocalDescriptionCallback(pc, RTC_OnDescription);
	rtcSetLocalCandidateCallback(pc, RTC_OnCandidate);
	rtcSetStateChangeCallback(pc, RTC_OnState);
	rtcSetDataChannelCallback(pc, RTC_OnDataChannel);

	if (sv_webrtc_debug.value)
		Con_Printf("WebRTC: %s is connecting\n", RTC_PeerName(slot));
}

static void RTC_CheckPeers (void)
{
	rtcpeer_t *p;
	int i;

	for (i = 0; i < MAX_RTCPEERS; i++)
	{
		p = &rtc.peers[i];
		if (!p->inuse)
			continue;

		if (!RTC_PeerReady(p))
		{
			if (p->inuse && curtime - p->created > RTC_CONNECT_TIME)
				RTC_DropPeer(p, "it did not connect in time");
			continue;
		}

		RTC_ChooseChannel(p);
		if (!p->banned && RTC_InGame(p))
		{
			p->joined = true;
			p->lastclient = curtime;
		}
		else if (curtime - p->lastclient > (p->banned ? RTC_BANNED_TIME : RTC_JOIN_TIME))
			RTC_DropPeer(p, p->banned ? "it is banned" : p->joined ? "it left the game" : "it did not join the game");
	}
}

/*
=============================================================================

The broker

=============================================================================
*/

static const char *RTC_HostedURL (void)
{
	if (rtc.port == RTC_BROKER_PORT)
		return va("%s://%s/%s", rtc.tls ? "rtcs" : "rtc", rtc.host, rtc.room);
	return va("%s://%s:%d/%s", rtc.tls ? "rtcs" : "rtc", rtc.host, rtc.port, rtc.room);
}

static void RTC_BrokerSend (int type, int brokerid, const char *text)
{
	static byte message[RTC_MAX_MESSAGE];
	size_t length = strlen(text);

	if (rtc.ws < 0 || !rtc.ws_open || length + 3 > sizeof(message))
		return;

	message[0] = (byte) type;
	message[1] = (byte) (brokerid & 0xff);
	message[2] = (byte) ((brokerid >> 8) & 0xff);
	memcpy(message + 3, text, length);
	rtcSendMessage(rtc.ws, (const char *) message, (int) (length + 3)); // binary
}

static void RTC_SendServerInfo (void)
{
	char info[MAX_SERVERINFO_STRING * 2];
	const char *gamedir;
	int i, clients = 0;

	rtc.nextinfo = curtime + RTC_INFO_TIME;

	for (i = 0; i < MAX_CLIENTS; i++)
		if ((svs.clients[i].state == cs_connected || svs.clients[i].state == cs_spawned) && !svs.clients[i].spectator)
			clients++;

	// what FTE's and SoftWorld's servers tell it
	strlcpy(info, svs.info, sizeof(info));
	gamedir = Info_ValueForKey(svs.info, "*gamedir");
	Info_SetValueForKey(info, "clients", va("%d", clients), sizeof(info));
	Info_SetValueForKey(info, "mapname", sv.mapname, sizeof(info));
	Info_SetValueForKey(info, "modname", *gamedir ? gamedir : "qw", sizeof(info));
	Info_SetValueForKey(info, "protocol", va("%d", PROTOCOL_VERSION), sizeof(info));
	RTC_BrokerSend(ICEMSG_SERVERINFO, -1, info);
}

// Candidates of a family the server has no address of (rtc.ipv4, rtc.ipv6)
// go neither way: libjuice would only fail to send to the client's, and the
// server's own would offer what the server does not listen on (its socket
// for any address of IPv6 takes IPv4 too). A name, such as an mDNS .local
// one, is left to libjuice.
static qbool RTC_CandidateUsable (const char *line)
{
	const char *s = strchr(line, ':'); // past "candidate:" or "a=candidate:"
	size_t length;
	int field;

	if (!s)
		return true;
	for (s++, field = 0; field < 4; field++)
	{
		// foundation, component, transport, priority; then the address
		s += strcspn(s, " \r\n");
		if (*s != ' ')
			return true;
		s += strspn(s, " ");
	}
	length = strcspn(s, " \r\n");
	if (memchr(s, ':', length))
		return rtc.ipv6;
	if (length && strspn(s, "0123456789.") == length)
		return rtc.ipv4;
	return true;
}

// The same for the candidates a description carries itself.
static void RTC_DropCandidates (char *sdp)
{
	char *in = sdp, *out = sdp, *end;
	size_t length;

	while (*in)
	{
		end = strchr(in, '\n');
		length = end ? (size_t) (end - in + 1) : strlen(in);
		if (strncmp(in, "a=candidate:", 12) || RTC_CandidateUsable(in))
		{
			memmove(out, in, length);
			out += length;
		}
		in += length;
	}
	*out = 0;
}

static void RTC_SendDescription (rtcpeer_t *p, const char *type, const char *sdp)
{
	static char json[RTC_MAX_MESSAGE], escaped[RTC_MAX_MESSAGE - 64], usable[RTC_MAX_MESSAGE - 64];

	// libdatachannel's descriptions are a few hundred bytes; never truncated
	if (p->brokerid < 0 || strlen(type) > 16 || strlcpy(usable, sdp, sizeof(usable)) >= sizeof(usable))
		return;
	RTC_DropCandidates(usable);
	if (!RTC_JsonEscape(usable, escaped, sizeof(escaped)))
		return;
	snprintf(json, sizeof(json), "{\"type\":\"%s\",\"sdp\":\"%s\"}", type, escaped);
	RTC_BrokerSend(ICEMSG_OFFER, p->brokerid, json);
}

static void RTC_SendCandidate (rtcpeer_t *p, const char *candidate, const char *mid)
{
	char json[1024 + 64 + 64], escaped[1024], escapedmid[64];

	if (p->brokerid < 0 || !RTC_CandidateUsable(candidate) || !RTC_JsonEscape(candidate, escaped, sizeof(escaped))
		|| !RTC_JsonEscape(mid && *mid ? mid : "0", escapedmid, sizeof(escapedmid)))
		return;
	snprintf(json, sizeof(json), "{\"candidate\":\"%s\",\"sdpMid\":\"%s\",\"sdpMLineIndex\":0}", escaped, escapedmid);
	RTC_BrokerSend(ICEMSG_CANDIDATE, p->brokerid, json);
}

static void RTC_Offer (int brokerid, const char *text)
{
	static char sdp[RTC_MAX_MESSAGE];
	rtcDataChannelInit init;
	rtcpeer_t *p = RTC_BrokerPeer(brokerid);
	int ret;

	if (!p || p->dc_local >= 0)
		return; // unknown, or answered already: no renegotiation

	if (*text == '{')
	{
		if (!RTC_JsonString(text, "sdp", sdp, sizeof(sdp)))
		{
			RTC_DropPeer(p, "its offer could not be read");
			return;
		}
	}
	else
		strlcpy(sdp, text, sizeof(sdp)); // FTE without JSON
	RTC_DropCandidates(sdp);

	if (sv_webrtc_debug.value >= 2)
		Con_Printf("WebRTC: %s offers:\n%s\n", RTC_PeerName(p), sdp);

	// libdatachannel answers by itself (RTC_OnDescription)
	if ((ret = rtcSetRemoteDescription(p->pc, sdp, "offer")) < 0)
	{
		RTC_DropPeer(p, va("its offer was refused (error %d)", ret));
		return;
	}

	// the server's own channel, opened only now so that it does not make an
	// offer of its own; FTE's native client reads this one
	memset(&init, 0, sizeof(init));
	init.reliability.unordered = true;
	init.reliability.unreliable = true;
	init.reliability.maxRetransmits = 0;
	if ((p->dc_local = rtcCreateDataChannelEx(p->pc, "quake", &init)) < 0)
	{
		p->dc_local = -1;
		RTC_DropPeer(p, "no data channel could be opened");
		return;
	}
	rtcSetUserPointer(p->dc_local, (void *) (intptr_t) (p - rtc.peers + 1));
	rtcSetAvailableCallback(p->dc_local, RTC_OnAvailable);
}

static void RTC_Candidate (int brokerid, const char *text)
{
	static char candidates[RTC_MAX_MESSAGE];
	rtcpeer_t *p = RTC_BrokerPeer(brokerid);
	char *line, *next;

	if (!p)
		return;

	if (*text == '{')
	{
		if (!RTC_JsonString(text, "candidate", candidates, sizeof(candidates)))
			return;
	}
	else
		strlcpy(candidates, text, sizeof(candidates)); // FTE without JSON: a=candidate lines

	for (line = candidates; line && *line; line = next)
	{
		if ((next = strchr(line, '\n')))
			*next++ = 0;
		line[strcspn(line, "\r")] = 0;
		if (!*line || !RTC_CandidateUsable(line))
			continue; // the end of candidates, or of no use
		// no mid: libdatachannel takes the one media section there is
		if (rtcAddRemoteCandidate(p->pc, line, NULL) < 0 && sv_webrtc_debug.value)
			Con_Printf("WebRTC: %s: candidate not used: %s\n", RTC_PeerName(p), line);
	}
}

static void RTC_BrokerClose (const char *why, double retry)
{
	int i;

	if (rtc.ws >= 0)
		rtcDeleteWebSocket(rtc.ws);
	if (why && (rtc.room[0] || rtc.failures == 0 || sv_webrtc_debug.value))
		Con_Printf("WebRTC: %s\n", why);
	if (!rtc.room[0])
		rtc.failures++;

	rtc.ws = -1;
	rtc.ws_open = false;
	rtc.room[0] = 0;
	rtc.retry = curtime + retry;

	// clients still connecting cannot finish without it; those in stay
	for (i = 0; i < MAX_RTCPEERS; i++)
	{
		if (!rtc.peers[i].inuse)
			continue;
		if (rtc.peers[i].adr.webrtc)
			rtc.peers[i].brokerid = -1;
		else
			RTC_DropPeer(&rtc.peers[i], "the broker went away");
	}
}

// "[ws|wss|rtc|rtcs|tcp|tls|ice|ices://]host[:port][/...]"; FTE's schemes, and
// like FTE, TLS without one. The host is a name or an IPv4 address:
// libdatachannel's URLs take no IPv6 literal, but a name of IPv6 addresses
// works.
static qbool RTC_ParseBroker (const char *url)
{
	const char *s = url, *scheme = strstr(url, "://");
	size_t length;

	rtc.tls = true;
	if (scheme)
	{
		length = scheme - url;
		if ((length == 2 && !strncmp(url, "ws", 2)) || (length == 3 && (!strncmp(url, "rtc", 3)
			|| !strncmp(url, "tcp", 3) || !strncmp(url, "ice", 3))))
			rtc.tls = false;
		else if (!((length == 3 && (!strncmp(url, "wss", 3) || !strncmp(url, "tls", 3)))
			|| (length == 4 && (!strncmp(url, "rtcs", 4) || !strncmp(url, "ices", 4)))))
			return false;
		s = scheme + 3;
	}

	length = strcspn(s, ":/");
	if (!length || length >= sizeof(rtc.host) || strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") < length)
		return false;
	memcpy(rtc.host, s, length);
	rtc.host[length] = 0;

	rtc.port = RTC_BROKER_PORT;
	if (s[length] == ':')
	{
		rtc.port = Q_atoi(s + length + 1);
		if (rtc.port <= 0 || rtc.port > 65535)
			return false;
	}
	return true;
}

static void RTC_BrokerOpen (const char *config)
{
	const char *protocols[1] = {"rtc_host"};
	const char *room = sv_webrtc_room.string;
	rtcWsConfiguration wsconfig;
	char url[512], about[MAX_OSPATH + 64];
	char *cas = NULL;

	strlcpy(rtc.ws_config, config, sizeof(rtc.ws_config));

	if (!RTC_ParseBroker(sv_webrtc_broker.string))
	{
		if (rtc.failures++ == 0)
			Con_Printf("WebRTC: sv_webrtc_broker \"%s\" is not a broker address%s\n", sv_webrtc_broker.string,
				strchr(sv_webrtc_broker.string, '[') ? " (libdatachannel takes no IPv6 address in brackets: give a name)" : "");
		rtc.retry = curtime + RTC_BROKER_RETRY;
		return;
	}
	if (strspn(room, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != strlen(room) || strlen(room) >= sizeof(rtc.room))
	{
		if (rtc.failures++ == 0)
			Con_Printf("WebRTC: sv_webrtc_room \"%s\" may only have letters, digits, '_', '.' and '-'\n", room);
		rtc.retry = curtime + RTC_BROKER_RETRY;
		return;
	}

	snprintf(url, sizeof(url), "%s://%s:%d/%s/%s", rtc.tls ? "wss" : "ws", rtc.host, rtc.port, RTC_BROKER_PROTOCOL, room);

	memset(&wsconfig, 0, sizeof(wsconfig));
	// Checking the broker's certificate keeps anyone between the server and
	// the broker from swapping the fingerprints in the offers and answers.
	// Mbed TLS cannot find the CA certificates the system trusts, so they are
	// passed in.
	wsconfig.disableTlsVerification = sv_webrtc_broker_insecure.value != 0;
	if (rtc.tls && !wsconfig.disableTlsVerification)
	{
		if (!(cas = SV_WebRTC_LoadCAs(sv_webrtc_broker_ca.string, about, sizeof(about))))
		{
			if (rtc.failures++ == 0)
				Con_Printf("WebRTC: cannot check the broker's certificate: %s; "
					"set sv_webrtc_broker_ca to a file of them, or sv_webrtc_broker_insecure 1 not to check it\n", about);
			rtc.retry = curtime + RTC_BROKER_RETRY;
			return;
		}
		if (sv_webrtc_debug.value)
			Con_Printf("WebRTC: checking the broker's certificate with %s\n", about);
		wsconfig.caCertificatePemFile = cas;
	}
	wsconfig.protocols = protocols;
	wsconfig.protocolsCount = 1;
	wsconfig.maxMessageSize = RTC_MAX_MESSAGE;

	rtc.ws = rtcCreateWebSocketEx(url, &wsconfig); // copies the CA certificates
	Q_free(cas);
	if (rtc.ws < 0)
	{
		rtc.ws = -1;
		RTC_BrokerClose(va("could not open %s", url), RTC_BROKER_RETRY);
		return;
	}
	rtcSetAvailableCallback(rtc.ws, RTC_OnAvailable);
	rtc.ws_open = false;
	rtc.ws_started = curtime;
	if (sv_webrtc_debug.value)
		Con_Printf("WebRTC: connecting to the broker, %s\n", url);
}

static void RTC_Greeting (const char *resource)
{
	const char *room = strrchr(resource, '/');

	strlcpy(rtc.room, room ? room + 1 : resource, sizeof(rtc.room));
	rtc.failures = 0;
	Con_Printf("WebRTC: hosting at %s\n", RTC_HostedURL());
	RTC_SendServerInfo();
}

static void RTC_BrokerMessage (const byte *data, int size)
{
	static char text[RTC_MAX_MESSAGE + 1];
	rtcpeer_t *p;
	size_t first;
	int type, brokerid = -1, length;

	if (size < 1)
		return;
	type = data[0];
	if (type == ICEMSG_NAMEINUSE)
	{
		data += 1;
		length = size - 1;
	}
	else
	{
		if (size < 3)
			return;
		brokerid = data[1] | (data[2] << 8);
		if (brokerid >= 0x8000)
			brokerid -= 0x10000; // -1 is the broker
		data += 3;
		length = size - 3;
	}
	memcpy(text, data, length);
	text[length] = 0;

	switch (type)
	{
	case ICEMSG_GREETING:
		RTC_Greeting(text);
		break;

	case ICEMSG_NEWPEER:
		if (brokerid >= 0)
		{
			first = strlen(text);
			RTC_NewPeer(brokerid, text, first < (size_t) length ? text + first + 1 : "");
		}
		break;

	case ICEMSG_OFFER:
		RTC_Offer(brokerid, text);
		break;

	case ICEMSG_CANDIDATE:
		RTC_Candidate(brokerid, text);
		break;

	case ICEMSG_PEERLOST:
		if (brokerid == -1)
			RTC_BrokerClose("the broker is going away", RTC_BROKER_RETRY);
		else if ((p = RTC_BrokerPeer(brokerid)))
		{
			// it only matters before the client is in
			if (p->adr.webrtc)
				p->brokerid = -1;
			else
				RTC_DropPeer(p, "it left the broker");
		}
		break;

	case ICEMSG_NAMEINUSE:
		if (sv_webrtc_room.string[0])
		{
			RTC_BrokerClose(va("room %s is taken at the broker; asking again in %d seconds",
				sv_webrtc_room.string, (int) RTC_NAMEINUSE_RETRY), RTC_NAMEINUSE_RETRY);
			rtc.failures = 1; // said once
		}
		else
			RTC_BrokerClose("the broker's room for this server was taken; asking for another", RTC_BROKER_RETRY);
		break;

	default:
		break;
	}
}

// Reads and drops a message too long for the buffer it was read into; size
// is what libdatachannel said it needs (negative for a text message).
static qbool RTC_Discard (int id, int size)
{
	static char scratch[RTC_MAX_MESSAGE + 16];
	char *buffer;
	qbool ok;

	size = abs(size);
	buffer = size <= (int) sizeof(scratch) ? scratch : malloc(size);
	if (!buffer)
		return false;
	ok = rtcReceiveMessage(id, buffer, &size) == RTC_ERR_SUCCESS;
	if (buffer != scratch)
		free(buffer);
	return ok;
}

static void RTC_BrokerRead (void)
{
	static char message[RTC_MAX_MESSAGE];
	int size, ret, count;

	for (count = 0; count < 64 && rtc.ws >= 0; count++)
	{
		size = sizeof(message);
		ret = rtcReceiveMessage(rtc.ws, message, &size);
		if (ret == RTC_ERR_TOO_SMALL)
		{
			if (!RTC_Discard(rtc.ws, size))
				break;
			continue;
		}
		if (ret != RTC_ERR_SUCCESS)
			break;
		RTC_BrokerMessage((const byte *) message, abs(size));
	}
}

static void RTC_Broker (void)
{
	char config[512];

	snprintf(config, sizeof(config), "%s\n%s\n%s\n%d", sv_webrtc_broker.string, sv_webrtc_room.string,
		sv_webrtc_broker_ca.string, sv_webrtc_broker_insecure.value != 0);
	if (strcmp(config, rtc.ws_config))
	{
		if (rtc.ws >= 0)
			RTC_BrokerClose("the broker's settings changed", 0);
		rtc.failures = 0;
		rtc.retry = 0;
		strlcpy(rtc.ws_config, config, sizeof(rtc.ws_config));
	}

	if (rtc.ws < 0)
	{
		if (sv_webrtc_broker.string[0] && curtime >= rtc.retry)
			RTC_BrokerOpen(config);
		return;
	}

	if (!rtc.ws_open)
	{
		if (rtcIsOpen(rtc.ws))
		{
			rtc.ws_open = true;
			rtc.ws_opened = curtime;
			if (sv_webrtc_debug.value)
				Con_Printf("WebRTC: connected to the broker\n");
		}
		else if (rtcIsClosed(rtc.ws) || curtime - rtc.ws_started > RTC_BROKER_TIMEOUT)
		{
			// it may have opened, refused the room (NAMEINUSE) and closed
			// between two frames
			RTC_BrokerRead();
			if (rtc.ws >= 0)
				RTC_BrokerClose(va("could not reach the broker at %s:%d; trying again every %d seconds",
					rtc.host, rtc.port, (int) RTC_BROKER_RETRY), RTC_BROKER_RETRY);
			return;
		}
		else
			return;
	}

	RTC_BrokerRead();

	if (rtc.ws >= 0 && rtcIsClosed(rtc.ws))
		RTC_BrokerClose(rtc.room[0] ? "lost the broker; trying again" : "the broker closed the connection",
			RTC_BROKER_RETRY);
	else if (rtc.ws >= 0 && !rtc.room[0] && curtime - rtc.ws_opened > RTC_BROKER_TIMEOUT)
		RTC_BrokerClose("the broker gave this server no room; trying again", RTC_BROKER_RETRY);
	else if (rtc.ws >= 0 && rtc.room[0] && curtime >= rtc.nextinfo)
		RTC_SendServerInfo();
}

/*
=============================================================================

Main thread

=============================================================================
*/

// Past a "<where><mark><digits>: " prefix, if s starts with one; <where> may
// have spaces in it only when spaces is true.
static const char *RTC_SkipLocation (const char *s, char mark, qbool spaces)
{
	const char *p, *digits;

	for (p = s; (p = strchr(p, mark)); p++)
	{
		if (!spaces && memchr(s, ' ', p - s))
			break;
		for (digits = p + 1; isdigit((unsigned char) *digits); digits++)
			;
		if (p > s && digits > p + 1 && digits[0] == ':' && digits[1] == ' ')
			return digits + 2;
	}
	return s;
}

// libdatachannel's log lines come formatted by plog as
// "rtc::impl::TcpTransport::processConnect@469: TCP connected" (a lambda's
// name has spaces in it), libjuice's inside them as
// "juice: agent.c:1253: Changing state to connected": just the text.
static const char *RTC_LogText (const char *s)
{
	s = RTC_SkipLocation(s, '@', true);
	if (!strncmp(s, "juice: ", 7))
		s = RTC_SkipLocation(s + 7, ':', false);
	return s;
}

static void RTC_ProcessEvents (void)
{
	rtcevent_t *ev;
	rtcpeer_t *p;

	while ((ev = RTC_PopEvent()))
	{
		p = NULL;
		if (ev->slot >= 0 && ev->slot < MAX_RTCPEERS && rtc.peers[ev->slot].inuse && rtc.peers[ev->slot].pc == ev->pc)
			p = &rtc.peers[ev->slot];

		switch (ev->kind)
		{
		case EV_LOG:
			if (ev->text)
				Con_Printf("WebRTC: %s\n", RTC_LogText(ev->text));
			break;

		case EV_DESCRIPTION:
			if (p && ev->text)
			{
				if (sv_webrtc_debug.value >= 2)
					Con_Printf("WebRTC: %s gets the %s:\n%s\n", RTC_PeerName(p), ev->extra ? ev->extra : "answer", ev->text);
				RTC_SendDescription(p, ev->extra ? ev->extra : "answer", ev->text);
			}
			break;

		case EV_CANDIDATE:
			if (p && ev->text)
				RTC_SendCandidate(p, ev->text, ev->extra);
			break;

		case EV_STATE:
			if (!p)
				break;
			p->state = ev->value;
			if (sv_webrtc_debug.value)
				Con_Printf("WebRTC: %s: %s\n", RTC_PeerName(p), RTC_StateName(p->state));
			if (p->state == RTC_FAILED || p->state == RTC_CLOSED)
				RTC_DropPeer(p, p->state == RTC_FAILED ? "the connection failed" : "the connection closed");
			break;

		case EV_CHANNEL:
			if (p && p->dc_remote < 0)
			{
				p->dc_remote = ev->value;
				rtcSetAvailableCallback(p->dc_remote, RTC_OnAvailable);
				RTC_SetWaiting(ev->slot, true); // what came before the callback
			}
			else
				rtcDeleteDataChannel(ev->value); // a second one, or its peer has gone
			break;
		}

		RTC_FreeEvent(ev);
	}
}

static void RTC_SetLogLevel (void)
{
	int debug = (int) sv_webrtc_debug.value;
	int level = debug >= 4 ? RTC_LOG_VERBOSE : debug == 3 ? RTC_LOG_DEBUG : debug == 2 ? RTC_LOG_INFO
		: debug == 1 ? RTC_LOG_WARNING : RTC_LOG_ERROR;

	if (level != rtc.loglevel)
	{
		rtcInitLogger((rtcLogLevel) level, RTC_OnLog);
		rtc.loglevel = level;
	}
}

static void RTC_Stop (void)
{
	rtcevent_t *ev;
	int i;

	for (i = 0; i < MAX_RTCPEERS; i++)
		RTC_DropPeer(&rtc.peers[i], NULL);
	if (rtc.ws >= 0)
		rtcDeleteWebSocket(rtc.ws);
	rtc.ws = -1;
	rtc.ws_open = false;
	rtc.room[0] = 0;
	rtc.ws_config[0] = 0;

	rtcCleanup(); // waits for libdatachannel's threads
	while ((ev = RTC_PopEvent()))
		RTC_FreeEvent(ev);
	rtc_waiting = 0;
	NET_WakeShutdown();

	Q_free(rtc.cert_pem);
	if (rtc.key_pem)
		memset(rtc.key_pem, 0, strlen(rtc.key_pem));
	Q_free(rtc.key_pem);
	rtc.running = false;
	Con_Printf("WebRTC: stopped\n");
}

// WebRTC has the address families the server has sockets for, bound as -ip
// and -ip6 bind them. libjuice binds one address: any address of both
// families, or one of a family; given one of each, it takes the IPv4 one.
static void RTC_Bind (void)
{
	const char *bind4 = NULL, *bind6 = NULL;
	int i;

	if ((i = COM_CheckParm("-ip")) != 0 && i + 1 < COM_Argc())
		bind4 = COM_Argv(i + 1);
	if ((i = COM_CheckParm("-ip6")) != 0 && i + 1 < COM_Argc())
		bind6 = COM_Argv(i + 1);

	rtc.ipv4 = svs.socketip != INVALID_SOCKET;
	rtc.ipv6 = svs.socketip6 != INVALID_SOCKET;
	rtc.bind[0] = 0;
	if (rtc.ipv4 && rtc.ipv6 && bind4 && bind6)
	{
		rtc.ipv6 = false;
		Con_Printf("WebRTC: IPv4 only, on %s: libdatachannel binds one address, and -ip and -ip6 give two\n", bind4);
	}
	if (rtc.ipv4 && !rtc.ipv6)
		strlcpy(rtc.bind, bind4 ? bind4 : "0.0.0.0", sizeof(rtc.bind));
	else if (rtc.ipv6 && !rtc.ipv4)
		strlcpy(rtc.bind, bind6 ? bind6 : "::", sizeof(rtc.bind));
}

static void RTC_Start (void)
{
	RTC_Bind();

	if (!SV_WebRTC_LoadCertificate(sv_webrtc_cert.string, sv_webrtc_key.string,
		&rtc.cert_pem, &rtc.key_pem, rtc.fingerprint, sizeof(rtc.fingerprint)))
	{
		Con_Printf("WebRTC: not started; set sv_webrtc 0 and 1 to try again\n");
		rtc.failed = true;
		return;
	}

	if (!NET_WakeInit())
		Con_Printf("WebRTC: no wakeup socket; packets wait for the next frame\n");
	RTC_SetLogLevel();
	rtcPreload(); // its threads now rather than with the first client

	rtc.running = true;
	rtc.failures = 0;
	rtc.retry = 0;
	rtc.ws_config[0] = 0;
	Con_Printf("WebRTC: started, %s\n", rtc.ipv4 && rtc.ipv6 ? "IPv4 and IPv6" : rtc.ipv6 ? "IPv6 only" : "IPv4 only");
}

void SV_WebRTC_Frame (void)
{
	char config[512];

	snprintf(config, sizeof(config), "%s\n%s\n%d", sv_webrtc_cert.string, sv_webrtc_key.string,
		(int) sv_webrtc_port.value);

	if (rtc.running && (!sv_webrtc.value || strcmp(config, rtc.config)))
		RTC_Stop();

	if (!sv_webrtc.value)
	{
		rtc.failed = false;
		return;
	}

	if (!rtc.running)
	{
		if (rtc.failed && !strcmp(config, rtc.config))
			return; // until something changes
		strlcpy(rtc.config, config, sizeof(rtc.config));
		rtc.failed = false;
		RTC_Start();
		if (!rtc.running)
			return;
	}

	RTC_SetLogLevel();
	RTC_ProcessEvents();
	RTC_Broker();
	RTC_CheckPeers();
}

// a packet from the channel, if one waits
static qbool RTC_Receive (int dc, sizebuf_t *message)
{
	int size, ret;

	if (dc < 0)
		return false;
	for (;;)
	{
		size = message->maxsize;
		ret = rtcReceiveMessage(dc, (char *) message->data, &size);
		if (ret == RTC_ERR_TOO_SMALL)
		{
			if (!RTC_Discard(dc, size))
				return false;
			continue;
		}
		if (ret != RTC_ERR_SUCCESS)
			return false;
		size = abs(size); // negative for a text message
		if (size <= 0 || size >= message->maxsize)
			continue; // as NET_GetUDPPacket: no room to spare, no packet
		message->cursize = size;
		return true;
	}
}

qbool SV_WebRTC_GetPacket (netadr_t *from, sizebuf_t *message)
{
	uint64_t waiting;
	rtcpeer_t *p;
	int n, i;

	// only the peers libdatachannel said have something
	if (!rtc.running || !(waiting = RTC_Waiting()))
		return false;

	for (n = 0; n < MAX_RTCPEERS; n++)
	{
		i = (rtc.nextpeer + n) % MAX_RTCPEERS;
		if (!(waiting & ((uint64_t) 1 << i)))
			continue;
		p = &rtc.peers[i];
		if (!p->inuse)
		{
			RTC_SetWaiting(i, false);
			continue;
		}
		if (!RTC_PeerReady(p))
			continue; // read once it is

		// cleared before reading, so that a packet arriving meanwhile sets it again
		RTC_SetWaiting(i, false);
		if (RTC_Receive(p->dc_remote, message) || RTC_Receive(p->dc_local, message))
		{
			RTC_SetWaiting(i, true); // there may be more
			*from = p->adr;
			p->lastrecv = curtime;
			rtc.nextpeer = (i + 1) % MAX_RTCPEERS; // the others first next time
			return true;
		}
	}
	return false;
}

void SV_WebRTC_SendPacket (int length, const void *data, netadr_t to)
{
	rtcpeer_t *p;
	int i;

	if (!rtc.running)
		return;

	for (i = 0; i < MAX_RTCPEERS; i++)
	{
		p = &rtc.peers[i];
		if (!p->inuse || !p->adr.webrtc || !NET_CompareAdr(p->adr, to))
			continue;
		// on the channel chosen each frame; as UDP, lost when it cannot go
		if (p->dc_send >= 0 && rtcGetBufferedAmount(p->dc_send) <= RTC_MAX_BUFFERED)
			rtcSendMessage(p->dc_send, (const char *) data, length);
		return;
	}
}

static void RTC_Status_f (void)
{
	rtcpeer_t *p;
	int i, count = 0;

	if (!rtc.running)
	{
		Con_Printf("WebRTC is off%s\n", !sv_webrtc.value ? " (sv_webrtc 0)" : rtc.failed ? ": it could not start" : "");
		return;
	}

	Con_Printf("certificate: %s, sha-256 %s\n", sv_webrtc_cert.string, rtc.fingerprint);
	if (rtc.ws < 0)
		Con_Printf("broker: not connected%s\n", sv_webrtc_broker.string[0] ? va(", next try in %.0f seconds",
			max(0, rtc.retry - curtime)) : " (sv_webrtc_broker is empty)");
	else if (!rtc.ws_open)
		Con_Printf("broker: connecting to %s:%d\n", rtc.host, rtc.port);
	else if (!rtc.room[0])
		Con_Printf("broker: connected to %s:%d, waiting for a room\n", rtc.host, rtc.port);
	else
		Con_Printf("broker: hosting at %s\n", RTC_HostedURL());

	for (i = 0; i < MAX_RTCPEERS; i++)
	{
		p = &rtc.peers[i];
		if (!p->inuse)
			continue;
		count++;
		Con_Printf("%2d %-21s %-12s %s%s%s, channels %s%s, %d bytes queued, last packet %.1fs ago\n", i,
			p->adr.webrtc ? NET_AdrToString(p->adr) : va("#%d %s", p->brokerid, p->from),
			RTC_StateName(p->state),
			!p->adr.webrtc ? "connecting" : p->joined ? "in the game" : "joining",
			p->relayed ? ", relayed" : "", p->banned ? ", banned" : "",
			RTC_ChannelOpen(p->dc_remote) ? "client's " : "", RTC_ChannelOpen(p->dc_local) ? "server's" : "",
			p->dc_send >= 0 ? rtcGetBufferedAmount(p->dc_send) : 0,
			p->adr.webrtc ? curtime - p->lastrecv : 0.0);
	}
	Con_Printf("%d WebRTC connection%s\n", count, count == 1 ? "" : "s");
}

void SV_WebRTC_Init (void)
{
	int i;

	Cvar_Register(&sv_webrtc);
	Cvar_Register(&sv_webrtc_broker);
	Cvar_Register(&sv_webrtc_broker_ca);
	Cvar_Register(&sv_webrtc_broker_insecure);
	Cvar_Register(&sv_webrtc_room);
	Cvar_Register(&sv_webrtc_port);
	Cvar_Register(&sv_webrtc_stun);
	Cvar_Register(&sv_webrtc_cert);
	Cvar_Register(&sv_webrtc_key);
	Cvar_Register(&sv_webrtc_mtu);
	Cvar_Register(&sv_webrtc_debug);
	Cmd_AddCommand("webrtc_status", RTC_Status_f);

	Sys_MutexInit(&rtc_lock);
	rtc.ws = -1;
	rtc.loglevel = -1;
	for (i = 0; i < MAX_RTCPEERS; i++)
		RTC_ClearPeer(&rtc.peers[i]);
}

void SV_WebRTC_Shutdown (void)
{
	double end;
	qbool queued, connected = false;
	int i;

	if (!rtc.running)
		return;

	// Let the last packets out (the server's final message) before the
	// channels close: until libdatachannel has passed them on, and a moment
	// more for SCTP to send them.
	for (end = Sys_DoubleTime() + RTC_FLUSH_TIME; ; Sys_Sleep(10))
	{
		queued = false;
		for (i = 0; i < MAX_RTCPEERS; i++)
		{
			if (!rtc.peers[i].inuse || rtc.peers[i].dc_send < 0)
				continue;
			connected = true;
			if (rtcGetBufferedAmount(rtc.peers[i].dc_send) > 0)
				queued = true;
		}
		if (!queued || Sys_DoubleTime() >= end)
			break;
	}
	if (connected)
		Sys_Sleep(50);

	RTC_Stop();
}

#endif // WEBRTC
