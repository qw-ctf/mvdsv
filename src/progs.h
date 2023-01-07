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

#ifndef __PROGS_H__
#define __PROGS_H__

#include "pr_comp.h" // defs shared with qcc
#include "progdefs.h" // generated by program cdefs

typedef union eval_s
{
	string_t	string;
	float		_float;
	float		vector[3];
	func_t		function;
	int		_int;
	int		edict;
} eval_t;

struct edict_s; // forward referecnce for link_t

typedef struct link_s
{
	struct edict_s *ed;

	struct link_s	*prev, *next;
} link_t;

#define	EDICT_FROM_AREA(l)	((l)->ed)

#define	MAX_ENT_LEAFS	16

typedef struct sv_edict_s
{
	qbool		free;
	link_t		area;			// linked to a division node or leaf

	int         entnum;

	int			num_leafs;
	short		leafnums[MAX_ENT_LEAFS];

	entity_state_t	baseline;

	float		freetime;		// sv.time when the object was freed
	double		lastruntime;	// sv.time when SV_RunEntity was last called for this edict (Tonik)
} sv_edict_t;

typedef struct
{
	float	alpha;
} ext_entvars_t;

typedef struct edict_s
{
	sv_edict_t	e;			// server side part of the edict_t
	entvars_t	*v;			// C exported fields from progs
	ext_entvars_t xv;
} edict_t;

//============================================================================

extern	dprograms_t	*progs;
extern	dfunction_t	*pr_functions;
extern	char		*pr_strings;
extern	ddef_t		*pr_globaldefs;
extern	ddef_t		*pr_fielddefs;
extern	dstatement_t	*pr_statements;
extern	globalvars_t	*pr_global_struct;
extern	float		*pr_globals;	// same as pr_global_struct

extern	int         pr_edict_size;	// in bytes
extern	cvar_t      sv_progsname; 
#ifdef WITH_NQPROGS
extern	cvar_t      sv_forcenqprogs;
#endif

//============================================================================

#ifdef WITH_NQPROGS

extern	qbool			pr_nqprogs;

extern	int pr_fieldoffsetpatch[106];
extern	int pr_globaloffsetpatch[62];

#define PR_FIELDOFS(i) ((unsigned int)(i) > 105 ? (i) : pr_fieldoffsetpatch[i])
#define PR_GLOBAL(field) (((globalvars_t *)((byte *)pr_global_struct + \
	pr_globaloffsetpatch[((int *)&((globalvars_t *)0)->field - (int *)0) - 28]))->field)

void NQP_Reset (void);

#else	// !WITH_NQPROGS

#define pr_nqprogs 0
#define PR_FIELDOFS(i) (i)
#define PR_GLOBAL(field) pr_global_struct->field
#define NQP_Reset()

#endif

//============================================================================

void PR_Init (void);

void PR_ExecuteProgram (func_t fnum);
void PR_InitPatchTables (void);	// NQ progs support

void PR_Profile_f (void);

void ED_ClearEdict (edict_t *e);
edict_t *ED_Alloc (void);
void ED_Free (edict_t *ed);

char *ED_NewString (char *string);
// returns a copy of the string allocated from the server's string heap

void ED_Print (edict_t *ed);
void ED_Write (FILE *f, edict_t *ed);
const char *ED_ParseEdict (const char *data, edict_t *ent);

void ED_WriteGlobals (FILE *f);
void ED_ParseGlobals (const char *data);

void ED_LoadFromFile (const char *data);

edict_t *EDICT_NUM(int n);
int NUM_FOR_EDICT(edict_t *e);

#define	NEXT_EDICT(e) ((edict_t *)((byte *)(e) + sizeof(edict_t)))

#define	EDICT_TO_PROG(e) ((byte *)(e)->v - (byte *)sv.game_edicts)
#define PROG_TO_EDICT(e) (&sv.edicts[(e)/pr_edict_size])

//============================================================================

#define	G_FLOAT(o) (pr_globals[o])
#define	G_INT(o) (*(int *)&pr_globals[o])
#define	G_EDICT(o) (&sv.edicts[(*(int *)&pr_globals[o])/pr_edict_size])
#define G_EDICTNUM(o) NUM_FOR_EDICT(G_EDICT(o))
#define	G_VECTOR(o) (&pr_globals[o])
#define	G_STRING(o) (PR1_GetString(*(string_t *)&pr_globals[o]))
#define	G_FUNCTION(o) (*(func_t *)&pr_globals[o])

#define	E_FLOAT(e,o) (((float*)e->v)[o])
#define	E_INT(e,o) (*(int *)&((float*)e->v)[o])
#define	E_VECTOR(e,o) (&((float*)e->v)[o])
#define	E_STRING(e,o) (PR1_GetString(*(string_t *)&((float*)e->v)[PR_FIELDOFS(o)]))

typedef void		(*builtin_t) (void);
extern	builtin_t	*pr_builtins;
extern	int		pr_numbuiltins;

extern	int		pr_argc;

extern	qbool	pr_trace;
extern	dfunction_t	*pr_xfunction;
extern	int		pr_xstatement;

extern func_t mod_ConsoleCmd, mod_UserCmd;
extern func_t mod_UserInfo_Changed, mod_localinfoChanged;
extern func_t mod_ChatMessage;
extern func_t mod_SpectatorConnect, mod_SpectatorDisconnect, mod_SpectatorThink;
extern func_t GE_ClientCommand, GE_PausedTic, GE_ShouldPause;

extern int fofs_items2; // ZQ_ITEMS2 extension
extern int fofs_vw_index;	// ZQ_VWEP
extern int fofs_movement;
extern int fofs_gravity, fofs_maxspeed;
extern int fofs_hideentity;
extern int fofs_trackent;
extern int fofs_visibility;
extern int fofs_hide_players;
extern int fofs_teleported;

#define EdictFieldFloat(ed, fieldoffset) ((eval_t *)((byte *)(ed)->v + (fieldoffset)))->_float
#define EdictFieldVector(ed, fieldoffset) ((eval_t *)((byte *)(ed)->v + (fieldoffset)))->vector

void PR_RunError (char *error, ...);

void ED_PrintEdicts (void);
void ED_PrintNum (int ent);

eval_t *PR1_GetEdictFieldValue(edict_t *ed, char *field);

int ED1_FindFieldOffset (char *field);

//
// PR Strings stuff
//
#define MAX_PRSTR 1024

extern char *pr_strtbl[MAX_PRSTR];
extern char *pr_newstrtbl[MAX_PRSTR];
extern int num_prstr;

char *PR1_GetString(int num);
void PR1_SetString(string_t* address, char* s);
void PR_SetTmpString(string_t* address, const char *s);

void PR1_LoadProgs (void);
void PR1_InitProg(void);
void PR1_Init(void);

#define PR1_GameShutDown()	// PR1 does not really have it.
void PR1_UnLoadProgs(void);

void PR1_GameClientDisconnect(int spec);
void PR1_GameClientConnect(int spec);
void PR1_GamePutClientInServer(int spec);
void PR1_GameClientPreThink(int spec);
void PR1_GameClientPostThink(int spec);
qbool PR1_ClientSay(int isTeamSay, char *message);
void PR1_PausedTic(float duration);
qbool PR1_ClientCmd(void);

#define PR1_GameSetChangeParms() PR_ExecuteProgram(PR_GLOBAL(SetChangeParms))
#define PR1_GameSetNewParms() PR_ExecuteProgram(PR_GLOBAL(SetNewParms))
#define PR1_GameStartFrame() PR_ExecuteProgram (PR_GLOBAL(StartFrame))
#define PR1_ClientKill() PR_ExecuteProgram (PR_GLOBAL(ClientKill))
#define PR1_UserInfoChanged(after) (0) // PR1 does not really have it,
                                  // we have mod_UserInfo_Changed but it is slightly different.
#define PR1_LoadEnts ED_LoadFromFile
#define PR1_EdictThink PR_ExecuteProgram
#define PR1_EdictTouch PR_ExecuteProgram
#define PR1_EdictBlocked PR_ExecuteProgram

#ifndef USE_PR2
	#define PR_LoadProgs PR1_LoadProgs
	#define PR_InitProg PR1_InitProg
	#define PR_GameShutDown PR1_GameShutDown
	#define PR_UnLoadProgs PR1_UnLoadProgs

	#define PR_Init PR1_Init
	//#define PR_GetString PR1_GetString
	//#define PR_SetString PR1_SetString
	#define PR_GetEntityString PR1_GetString
	#define PR_SetEntityString(ent, target, value) PR1_SetString(&target, value)
	#define PR_SetGlobalString(target, value) PR1_SetString(&target, value)
	#define ED_FindFieldOffset ED1_FindFieldOffset
	#define PR_GetEdictFieldValue PR1_GetEdictFieldValue

	#define PR_GameClientDisconnect PR1_GameClientDisconnect
	#define PR_GameClientConnect PR1_GameClientConnect
	#define PR_GamePutClientInServer PR1_GamePutClientInServer
	#define PR_GameClientPreThink PR1_GameClientPreThink
	#define PR_GameClientPostThink PR1_GameClientPostThink
	#define PR_ClientSay PR1_ClientSay
	#define PR_PausedTic PR1_PausedTic
	#define PR_ClientCmd PR1_ClientCmd

	#define PR_GameSetChangeParms PR1_GameSetChangeParms
	#define PR_GameSetNewParms PR1_GameSetNewParms
	#define PR_GameStartFrame(isBotFrame) { if (!isBotFrame) { PR1_GameStartFrame(); } }
	#define PR_ClientKill PR1_ClientKill
	#define PR_UserInfoChanged PR1_UserInfoChanged
	#define PR_LoadEnts PR1_LoadEnts
	#define PR_EdictThink PR1_EdictThink
	#define PR_EdictTouch PR1_EdictTouch
	#define PR_EdictBlocked PR1_EdictBlocked

	#define PR_ClearEdict(ent)
#endif

// pr_cmds.c
void PR_InitBuiltins (void);

#endif /* !__PROGS_H__ */
