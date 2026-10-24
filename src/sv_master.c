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
// sv_master.c - send heartbeats to master server

#ifndef CLIENTONLY
#include "qwsvdef.h"

#define HEARTBEAT_SECONDS 300

netadr_t master_adr[MAX_MASTERS]; // address of group servers

/*
====================
SV_SetMaster_f

Make a master server current
====================
*/
// true if server has a socket of the address family of adr.
static qbool SV_MasterFamilyEnabled (const netadr_t *adr)
{
	if (adr->type == NA_IP)
		return svs.socketip != INVALID_SOCKET;
	if (adr->type == NA_IPV6)
		return svs.socketip6 != INVALID_SOCKET;
	return false;
}

void SV_SetMaster_f (void)
{
	char data[2];
	netadr_t list[MAX_MASTERS];
	int i, j, k, n, count = 0;

	memset (&master_adr, 0, sizeof(master_adr));

	for (i=1 ; i<Cmd_Argc() ; i++)
	{
		if (!strcmp(Cmd_Argv(i), "none"))
		{
			memset (&master_adr, 0, sizeof(master_adr));
			Con_Printf ("Setting nomaster mode.\n");
			return;
		}

		// a name may resolve to IPv4 and IPv6 addresses, heartbeats are sent to all of them.
		n = NET_StringToAdrList (Cmd_Argv(i), list, MAX_MASTERS);
		if (n <= 0)
		{
			Con_Printf ("Couldn't resolve master server %s, skipping.\n", Cmd_Argv(i));
			continue;
		}

		for (j = 0; j < n; j++)
		{
			if (list[j].port == 0)
				list[j].port = BigShort (27000);

			if (!SV_MasterFamilyEnabled (&list[j]))
			{
				Con_Printf ("Master server at %s skipped, %s is not enabled.\n",
					NET_AdrToString (list[j]), list[j].type == NA_IPV6 ? "IPv6" : "IPv4");
				continue;
			}

			for (k = 0; k < count; k++)
			{
				if (NET_CompareAdr (master_adr[k], list[j]))
					break;
			}

			if (k < count)
				continue; // same master given twice

			if (count == MAX_MASTERS)
			{
				Con_Printf ("Too many master servers, %s ignored.\n", NET_AdrToString (list[j]));
				continue;
			}

			master_adr[count++] = list[j];

			Con_Printf ("Master server at %s\n", NET_AdrToString (list[j]));

			Con_Printf ("Sending a ping.\n");

			data[0] = A2A_PING;
			data[1] = 0;
			NET_SendPacket (NS_SERVER, 2, data, list[j]);
		}
	}

	svs.last_heartbeat = -99999;
}

/*
==================
SV_Heartbeat_f
==================
*/
void SV_Heartbeat_f (void)
{
	svs.last_heartbeat = -99999;
}

/*
================
Master_Heartbeat

Send a message to the master every few minutes to
let it know we are alive, and log information
================
*/
void Master_Heartbeat (void)
{
	char string[2048];
	int active;
	int i;

	if (sv.state != ss_active)
		return;

	if (realtime - svs.last_heartbeat < HEARTBEAT_SECONDS)
		return; // not time to send yet

	svs.last_heartbeat = realtime;

	//
	// count active users
	//
	active = 0;
	for (i=0 ; i<MAX_CLIENTS ; i++)
		if (svs.clients[i].state == cs_connected ||
				svs.clients[i].state == cs_spawned )
			active++;

	svs.heartbeat_sequence++;
	snprintf (string, sizeof(string), "%c\n%i\n%i\n", S2M_HEARTBEAT,
		  svs.heartbeat_sequence, active);


	// send to group master
	for (i=0 ; i<MAX_MASTERS ; i++)
		if (master_adr[i].type != NA_INVALID)
		{
			Con_Printf ("Sending heartbeat to %s\n", NET_AdrToString (master_adr[i]));
			NET_SendPacket (NS_SERVER, strlen(string), string, master_adr[i]);
		}
}

/*
=================
Master_Shutdown

Informs all masters that this server is going down
=================
*/
void Master_Shutdown (void)
{
	char string[2048];
	int i;

	snprintf (string, sizeof(string), "%c\n", S2M_SHUTDOWN);

	// send to group master
	for (i=0 ; i<MAX_MASTERS ; i++)
		if (master_adr[i].type != NA_INVALID)
		{
			Con_Printf ("Sending heartbeat to %s\n", NET_AdrToString (master_adr[i]));
			NET_SendPacket (NS_SERVER, strlen(string), string, master_adr[i]);
		}
}

#endif // !CLIENTONLY
