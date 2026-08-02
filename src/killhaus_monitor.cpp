/**
 * KillhausMonitor — mm_getinfo для CS2 без зависимостей от Utils/Players/LR.
 * Данные читаются ТОЛЬКО по запросу команды mm_getinfo (RCON): без per-tick хуков,
 * без слепого перебора слотов — только реальные сущности cs_player_controller,
 * под null-проверками. Это безопасно для живого сервера. GPLv3.
 */
#include <cstdio>
#include <ctime>
#include <string>

#include "killhaus_monitor.h"

#include "schemasystem/schemasystem.h"
#include <entity2/entitysystem.h>
#include <eiface.h>
#include <iserver.h> // INetworkServerService, INetworkGameServer, CNetworkGameServerBase

// SchemaEntity — schema-обёртки и глобал g_pEntitySystem.
#include "utils.hpp"
#include "CBaseEntity.h"
#include "CTeam.h"
#include "CCSPlayerController.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

// В hl2sdk-cs2 нет отдельного хедера GameResourceService — forward + строки версий
// (значения из public/interfaces/interfaces.h; guarded).
class IGameResourceService;
#ifndef GAMERESOURCESERVICESERVER_INTERFACE_VERSION
#define GAMERESOURCESERVICESERVER_INTERFACE_VERSION "GameResourceServiceServerV001"
#endif
#ifndef NETWORKSERVERSERVICE_INTERFACE_VERSION
#define NETWORKSERVERSERVICE_INTERFACE_VERSION "NetworkServerService_001"
#endif
#ifndef SOURCE2ENGINETOSERVER_INTERFACE_VERSION
#define SOURCE2ENGINETOSERVER_INTERFACE_VERSION "Source2EngineToServer001"
#endif

// ── Глобалы плагина ─────────────────────────────────────────────────
KillhausMonitor g_KillhausMonitor;
PLUGIN_EXPOSE(KillhausMonitor, g_KillhausMonitor);

IVEngineServer2 *engine = nullptr;

// Эти три уже определены в SDK (interfaces.a) — только extern, не определять.
extern INetworkServerService *g_pNetworkServerService;
extern IGameResourceService *g_pGameResourceServiceServer;
extern ISchemaSystem *g_pSchemaSystem;

// Эти в SDK нет — их ждёт SchemaEntity, определяем сами.
CEntitySystem *g_pEntitySystem = nullptr;
CGameEntitySystem *g_pGameEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

// GameEntitySystem() — имя строго так: объявлено extern в SDK (entity2/entitysystem.h),
// SDK-код (entitysystem.cpp) линкуется на неё. Offset 0x58/0x50 подтверждён по
// актуальному gamedata CS2Fixes.
CGameEntitySystem *GameEntitySystem()
{
	if (!g_pGameResourceServiceServer)
		return nullptr;
	return *reinterpret_cast<CGameEntitySystem **>(
		reinterpret_cast<uintptr_t>(g_pGameResourceServiceServer) + WIN_LINUX(0x58, 0x50));
}

static CGlobalVars *ResolveGlobals()
{
	if (!g_pNetworkServerService)
		return nullptr;
	INetworkGameServer *gs = g_pNetworkServerService->GetIGameServer();
	return gs ? gs->GetGlobals() : nullptr;
}

static void GetTeamScore(int &ctScore, int &tScore)
{
	ctScore = 0;
	tScore = 0;
	for (CEntityInstance *e : UTIL_FindEntityByClassnameAll("cs_team_manager"))
	{
		CTeam *pTeam = reinterpret_cast<CTeam *>(e);
		if (!pTeam)
			continue;
		int tn = pTeam->m_iTeamNum();
		if (tn == 3)
			ctScore = pTeam->m_iScore();
		else if (tn == 2)
			tScore = pTeam->m_iScore();
	}
}

// Сбор данных матча — вызывается только из mm_getinfo. Указатели резолвятся лениво.
static json BuildServerInfo()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = reinterpret_cast<CEntitySystem *>(g_pGameEntitySystem);
	gpGlobals = ResolveGlobals();

	json jdata;
	jdata["time"] = (long long)std::time(nullptr);
	jdata["current_map"] = (gpGlobals && gpGlobals->mapname.ToCStr()) ? gpGlobals->mapname.ToCStr() : "";

	int ctScore = 0, tScore = 0;
	if (g_pEntitySystem)
		GetTeamScore(ctScore, tScore);
	jdata["score_ct"] = ctScore;
	jdata["score_t"] = tScore;

	json jPlayers = json::array();
	if (g_pEntitySystem)
	{
		// Только реальные контроллеры игроков — без слепого перебора слотов.
		for (CEntityInstance *e : UTIL_FindEntityByClassnameAll("cs_player_controller"))
		{
			CCSPlayerController *pc = reinterpret_cast<CCSPlayerController *>(e);
			if (!pc || !pc->IsConnected())
				continue;

			json jp;
			const char *name = pc->GetPlayerName();
			jp["userid"] = pc->GetPlayerSlot();
			jp["name"] = (name && name[0] != '\0') ? name : "Unknown";
			jp["team"] = pc->GetTeam();

			char sid[32];
			std::snprintf(sid, sizeof(sid), "%llu", (unsigned long long)pc->m_steamID());
			jp["steamid"] = sid;

			CCSPlayerController_ActionTrackingServices *ats = pc->m_pActionTrackingServices();
			if (ats)
			{
				jp["kills"] = ats->m_matchStats().m_iKills();
				jp["death"] = ats->m_matchStats().m_iDeaths();
				jp["headshots"] = ats->m_matchStats().m_iHeadShotKills();
			}
			else
			{
				jp["kills"] = 0;
				jp["death"] = 0;
				jp["headshots"] = 0;
			}
			jp["ping"] = (int)pc->m_iPing();
			jp["playtime"] = 0; // без per-tick хука время не трекаем (безопасность)
			jPlayers.push_back(jp);
		}
	}
	jdata["players"] = jPlayers;
	return jdata;
}

CON_COMMAND_F(mm_getinfo, "Печатает JSON с данными матча (веб-мониторинг)", FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE)
{
	json jdata = BuildServerInfo();
	std::string dump = jdata.dump();
	META_CONPRINTF("%s\n", dump.c_str());
}

// ── ISmmPlugin ──────────────────────────────────────────────────────
bool KillhausMonitor::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);

	ConVar_Register(FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);
	return true;
}

bool KillhausMonitor::Unload(char *error, size_t maxlen)
{
	ConVar_Unregister();
	return true;
}

// ── Метаданные ──────────────────────────────────────────────────────
const char *KillhausMonitor::GetLicense() { return "GPLv3"; }
const char *KillhausMonitor::GetVersion() { return "1.0.1"; }
const char *KillhausMonitor::GetDate() { return __DATE__; }
const char *KillhausMonitor::GetLogTag() { return "KillhausMonitor"; }
const char *KillhausMonitor::GetAuthor() { return "KILLHAUS"; }
const char *KillhausMonitor::GetDescription() { return "mm_getinfo match data for web monitoring"; }
const char *KillhausMonitor::GetName() { return "KillhausMonitor"; }
const char *KillhausMonitor::GetURL() { return "https://killhaus.su"; }
