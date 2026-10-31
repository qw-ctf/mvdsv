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

// net.h -- quake's interface to the networking layer

#ifndef __NET_H__
#define __NET_H__

#include <errno.h>

#define MAX_DUPLICATE_PACKETS (3)

#ifdef _WIN32

//
// OS specific includes.
//

#include <winsock2.h>
#include <ws2tcpip.h>

//
// OS specific types definition.
//

typedef int socklen_t;
typedef SOCKET socket_t;

//
// OS specific definitions.
//

#ifdef EWOULDBLOCK
#undef EWOULDBLOCK
#endif
#ifdef EMSGSIZE
#undef EMSGSIZE
#endif
#ifdef ECONNRESET
#undef ECONNRESET
#endif
#ifdef ECONNABORTED
#undef ECONNABORTED
#endif
#ifdef ECONNREFUSED
#undef ECONNREFUSED
#endif
#ifdef EADDRNOTAVAIL
#undef EADDRNOTAVAIL
#endif
#ifdef EAFNOSUPPORT
#undef EAFNOSUPPORT
#endif

#define EWOULDBLOCK		WSAEWOULDBLOCK
#define EMSGSIZE		WSAEMSGSIZE
#define ECONNRESET		WSAECONNRESET
#define ECONNABORTED	WSAECONNABORTED
#define ECONNREFUSED	WSAECONNREFUSED
#define EADDRNOTAVAIL	WSAEADDRNOTAVAIL
#define EAFNOSUPPORT	WSAEAFNOSUPPORT
#define qerrno 			WSAGetLastError()
#else //_WIN32

//
// OS specific includes.
//

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __sun__
#include <sys/filio.h>
#endif //__sun__

#ifdef NeXT
#include <libc.h>
#endif //NeXT

//
// OS specific types definition.
//

typedef int socket_t;

//
// OS specific definitions.
//

#define qerrno 			errno

#define closesocket close
#define ioctlsocket ioctl

#endif //_WIN32

//
// common definitions.
//

#ifndef INVALID_SOCKET
#define INVALID_SOCKET	-1
#endif

#ifndef SOCKET_ERROR
#define SOCKET_ERROR -1
#endif

#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 27 // value from the Winsock ABI, POSIX headers always define it
#endif

#define PORT_ANY ((unsigned short int)0xFFFF)

typedef enum {NA_INVALID, NA_LOOPBACK, NA_IP, NA_IPV6} netadrtype_t;

typedef enum {NS_CLIENT, NS_SERVER} netsrc_t;

typedef struct
{
	netadrtype_t	type;

	byte			ip[4];		// valid when type == NA_IP
	byte			ip6[16];	// valid when type == NA_IPV6
	unsigned int	scope_id;	// NA_IPV6 only: sin6_scope_id of link-local addresses, otherwise 0

	unsigned short	port;		// network byte order

	// A client on a WebRTC data channel (sv_webrtc.c): the type, address and
	// port are its real ones, but packets to it go through its channel, never UDP.
	qbool			webrtc;
} netadr_t;

extern	netadr_t	net_local_sv_ipadr;		// IPv4 server address, NA_INVALID if IPv4 is not enabled
extern	netadr_t	net_local_sv_ip6adr;	// IPv6 server address, NA_INVALID if IPv6 is not enabled
extern	netadr_t	net_local_cl_ipadr;

extern	netadr_t	net_from; // address of who sent the packet
extern	sizebuf_t	net_message;

// convert netadr_t to sockaddr_storage, returns the length of the sockaddr, or 0 for non IP address types and WebRTC addresses.
socklen_t	NetadrToSockadr (const netadr_t *a, struct sockaddr_storage *s);
// convert sockaddr_storage to netadr_t, returns false (and sets NA_INVALID) for unsupported address families.
// IPv4-mapped IPv6 addresses (::ffff:a.b.c.d) are converted to NA_IP.
qbool	SockadrToNetadr (const struct sockaddr_storage *s, netadr_t *a);

// compare netadr_t. Addresses of different types never match, NA_INVALID addresses never match anything,
// and a WebRTC address never matches a UDP one.
qbool	NET_CompareAdr (const netadr_t a, const netadr_t b);
// compare netadr_t, ignore port.
qbool	NET_CompareBaseAdr (const netadr_t a, const netadr_t b);
// print netadr_t as string, a.b.c.d:port or [v6]:port notation.
char	*NET_AdrToString (const netadr_t a);
// print netadr_t as string, port skipped, a.b.c.d or v6 (without brackets) notation.
char	*NET_BaseAdrToString (const netadr_t a);
// convert/resolve IP/DNS to netadr_t. Accepts host, host:port, [v6], [v6]:port and bare v6 (no port).
// When a name resolves to both families, the family of an open server socket is preferred, IPv4 first.
qbool	NET_StringToAdr (const char *s, netadr_t *a);
// same syntax as NET_StringToAdr, returns every address the name resolves to (both families, no duplicates).
int		NET_StringToAdrList (const char *s, netadr_t *out, int maxout);
// same syntax as NET_StringToAdr, but only numeric addresses are accepted, never does DNS lookups.
qbool	NET_StringToAdrNumeric (const char *s, netadr_t *a);

// 127.0.0.0/8 and ::1.
qbool	NET_IsLoopbackAdr (const netadr_t a);
// IPv4: 10/8, 172.16/12, 192.168/16 and multicast. IPv6: link-local, unique local and multicast.
qbool	NET_IsPrivateAdr (const netadr_t a);
// raw address bytes of the active family (4 or 16), NULL and *len 0 for non IP addresses.
const byte	*NET_AdrBytes (const netadr_t *a, int *len);

void	NET_Init (void);
void	NET_Shutdown (void);
void	NET_InitClient (void);
void	NET_CloseClient (void);
void	NET_InitServer (void);
void	NET_CloseServer (void);
qbool	NET_GetPacket (netsrc_t sock);
void	NET_SendPacket (netsrc_t sock, int length, void *data, netadr_t to);

void	NET_GetLocalAddress (socket_t sock, netadr_t *out);

void	NET_ClearLoopback (void);
qbool	NET_Sleep(int msec, qbool stdinissocket);
#ifdef WEBRTC
// A loopback socket NET_Sleep also waits on, so other threads can end its wait.
qbool	NET_WakeInit (void);
void	NET_WakeShutdown (void);
void	NET_Wake (void);	// from any thread
#endif

// GETER: return port of UDP server socket.
int		NET_UDPSVPort (void);

// number of server TCP listen sockets, index 0 is IPv4, index 1 is IPv6.
#define NET_TCP_LISTENERS 2
// GETER: return server TCP listen socket by index, INVALID_SOCKET if it is not open.
socket_t	NET_GetTCPListenSocket (int index);
// GETER: true if any server TCP listen socket is open.
qbool	NET_HasTCPListenSocket (void);
// GETER: true if an address family with a server UDP socket has no server TCP listen socket.
qbool	NET_TCPListenSocketMissing (void);

// open server TCP sockets, one per enabled address family.
void	NET_InitServer_TCP(unsigned short int port);
// open server TCP sockets which are not open yet, keeping the open ones.
void	NET_OpenServer_TCP(unsigned short int port);

#ifndef CLIENTONLY
// allocate server side TCP connection, may link it in, if requested.
struct svtcpstream_s	*sv_tcp_connection_new (socket_t sock, netadr_t from, char *buf, int buf_len, qbool link);
int		sv_tcp_connection_count (void);
#endif

// UTILITY: set KEEPALIVE option on TCP socket (useful for faster timeout detection).
qbool 	TCP_Set_KEEPALIVE(socket_t sock);
// UTILITY: open TCP socket for remove address (useful for client connection).
socket_t	TCP_OpenStream (netadr_t remoteaddr);
// UTILITY: open TCP listen socket (useful for server). family is AF_INET or AF_INET6, bindaddr NULL means any address.
socket_t	TCP_OpenListenSocket (int family, const char *bindaddr, unsigned short int port);
// UTILITY: open UDP listen socket (useful for server). family is AF_INET or AF_INET6, bindaddr NULL means any address.
socket_t	UDP_OpenSocket (int family, const char *bindaddr, unsigned short int port);
//============================================================================

//
// netchan related.
//

#define	OLD_AVG		0.99		// total = oldtotal*OLD_AVG + new*(1-OLD_AVG)

#define	MAX_LATENT	32

typedef struct
{
	netsrc_t	sock;

	qbool		fatal_error;

	int			dropped;			// between last packet and previous

	float		last_received;		// for timeouts

	// the statistics are cleared at each client begin, because
	// the server connecting process gives a bogus picture of the data
	float		frame_latency;		// rolling average
	float		frame_rate;

	int			drop_count;			// dropped packets, cleared each level
	int			good_count;			// cleared each level

	netadr_t	remote_address;
	int			qport;

	// bandwidth estimator
	double		cleartime;			// if curtime > nc->cleartime, free to go
	double		rate;				// seconds / byte

	int         dupe;               // duplicate packets to send (0 = no duplicates, as normal)

	// sequencing variables
	int			incoming_sequence;
	int			incoming_acknowledged;
	int			incoming_reliable_acknowledged; // single bit

	int			incoming_reliable_sequence; // single bit, maintained local

	int			outgoing_sequence;
	int			reliable_sequence;	// single bit
	int			last_reliable_sequence; // sequence number of last send

	// reliable staging and holding areas
	sizebuf_t	message;			// writing buffer to send to server
	byte		message_buf[MAX_MSGLEN];

	int			reliable_length;
	byte		reliable_buf[MAX_MSGLEN]; // unacked reliable message

	// time and size data to calculate bandwidth
	int			outgoing_size[MAX_LATENT];
	double		outgoing_time[MAX_LATENT];
} netchan_t;

void Netchan_Init (void);
void Netchan_Transmit (netchan_t *chan, int length, byte *data);
void Netchan_OutOfBand (netsrc_t sock, netadr_t adr, int length, byte *data);
void Netchan_OutOfBandPrint (netsrc_t sock, netadr_t adr, char *format, ...);
qbool Netchan_Process (netchan_t *chan);
void Netchan_Setup (netsrc_t sock, netchan_t *chan, netadr_t adr, int qport, int mtu);

qbool Netchan_CanPacket (netchan_t *chan);
qbool Netchan_CanReliable (netchan_t *chan);

#endif /* !__NET_H__ */
