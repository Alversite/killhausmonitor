/**
 * KillhausMonitor — mm_getinfo для CS2 без зависимостей от Utils/Players/LR.
 * Логика повторяет Pisex/PlayersInfo, но entity system и globals резолвятся сами.
 * GPLv3.
 */
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "killhaus_monitor.h"

#include "schemasystem/schemasystem.h"
#include <entity2/entitysystem.h>
#include <eiface.h>
#include <iserver.h> // INetworkServerService, INetworkGameServer, CNetworkGameServerBase

// SchemaEntity (../SchemaEntity) — schema-обёртки и глобалы g_pEntitySystem.
#include "utils.hpp"
#include "CBaseEntity.h"
#include "CTeam.h"
#include "CCSPlayerController.h"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

// В hl2sdk-cs2 нет отдельного хедера GameResourceService — объявляем forward и строки
// версий сами (значения из public/interfaces/interfaces.h; guarded — если SDK объявит
// их сам, дубля не будет). ВНИМАНИЕ: у GameResourceService строка с 'V'.
class IGameResourceService;
#ifndef GAMERESOURCESERVICESERVER_INTERFACE_VERSION
#define GAMERESOURCESERVICESERVER_INTERFACE_VERSION "GameResourceServiceServerV001"
#endif
#ifndef NETWORKSERVERSERVICE_INTERFACE_VERSION
#define NETWORKSERVERSERVICE_INTERFACE_VERSION "NetworkServerService_001"
#endif
#ifndef SOURCE2SERVER_INTERFACE_VERSION
#define SOURCE2SERVER_INTERFACE_VERSION "Source2Server001"
#endif
#ifndef SOURCE2ENGINETOSERVER_INTERFACE_VERSION
#define SOURCE2ENGINETOSERVER_INTERFACE_VERSION "Source2EngineToServer001"
#endif

// ── Глобалы плагина ─────────────────────────────────────────────────
KillhausMonitor g_KillhausMonitor;
PLUGIN_EXPOSE(KillhausMonitor, g_KillhausMonitor);

IVEngineServer2 *engine = nullptr;
ISource2Server *server = nullptr;
INetworkServerService *g_pNetworkServerService = nullptr;
IGameResourceServiceServer *g_pGameResourceServiceServer = nullptr;

// Требуются SchemaEntity (schemasystem.cpp/CBaseEntity.h) как внешние — определяем тут.
// Если SDK объявит их сам и линкер ругнётся на дубликат — убрать соответствующую строку.
ISchemaSystem *g_pSchemaSystem = nullptr;
CEntitySystem *g_pEntitySystem = nullptr;
CGameEntitySystem *g_pGameEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

// Время подключения игроков (для playtime). Заполняется в GameFrame-хуке.
static int g_iConnectTime[64] = {0};
static double g_flLastPoll = 0.0;

SH_DECL_HOOK3_void(ISource2Server, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);

// ── Резолв движковых указателей (то, что раньше давал Utils) ─────────
static CGameEntitySystem *ResolveGameEntitySystem()
{
	if (!g_pGameResourceServiceServer)
		return nullptr;
	// Известное смещение указателя на GameEntitySystem внутри GameResourceService.
	// При обновлении игры может измениться — тогда поправить здесь (0x58 win / 0x50 linux).
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

// ── Сбор данных матча ───────────────────────────────────────────────
static void GetTeamScore(int &ctScore, int &tScore)
{
	ctScore = 0;
	tScore = 0;
	std::vector<CEntityInstance *> teams = UTIL_FindEntityByClassnameAll("cs_team_manager");
	for (size_t i = 0; i < teams.size(); i++)
	{
		CTeam *pTeam = reinterpret_cast<CTeam *>(teams[i]);
		if (!pTeam)
			continue;
		if (pTeam->m_iTeamNum() == 3)
			ctScore = pTeam->m_iScore();
		else if (pTeam->m_iTeamNum() == 2)
			tScore = pTeam->m_iScore();
	}
}

static json BuildServerInfo()
{
	json jdata;
	jdata["time"] = (long long)std::time(nullptr);
	jdata["current_map"] = (gpGlobals && gpGlobals->mapname.ToCStr()) ? gpGlobals->mapname.ToCStr() : "";

	int ctScore = 0, tScore = 0;
	GetTeamScore(ctScore, tScore);
	jdata["score_ct"] = ctScore;
	jdata["score_t"] = tScore;

	json jPlayers = json::array();
	for (int i = 0; i < 64; i++)
	{
		CCSPlayerController *pController = CCSPlayerController::FromSlot(i);
		if (!pController)
			continue;
		if (!pController->IsConnected())
			continue;

		json jp;
		const char *szName = pController->GetPlayerName();
		jp["userid"] = i;
		jp["name"] = (szName && szName[0] != '\0') ? szName : "Unknown";
		jp["team"] = pController->GetTeam();

		char szSteam[32];
		std::snprintf(szSteam, sizeof(szSteam), "%llu", (unsigned long long)pController->m_steamID());
		jp["steamid"] = szSteam;

		CCSPlayerController_ActionTrackingServices *ats = pController->m_pActionTrackingServices();
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
		jp["ping"] = (int)pController->m_iPing();
		jp["playtime"] = (g_iConnectTime[i] > 0) ? (int)(std::time(nullptr) - g_iConnectTime[i]) : 0;

		jPlayers.push_back(jp);
	}
	jdata["players"] = jPlayers;
	return jdata;
}

CON_COMMAND_F(mm_getinfo, "Печатает JSON с данными матча (для веб-мониторинга)", FCVAR_GAMEDLL | FCVAR_SERVER_CAN_EXECUTE)
{
	json jdata = BuildServerInfo();
	std::string dump = jdata.dump();
	META_CONPRINTF("%s\n", dump.c_str());
}

// ── GameFrame: ленивый резолв указателей + учёт времени игроков ──────
void KillhausMonitor::Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	if (!g_pEntitySystem)
	{
		g_pGameEntitySystem = ResolveGameEntitySystem();
		g_pEntitySystem = reinterpret_cast<CEntitySystem *>(g_pGameEntitySystem);
	}
	if (!gpGlobals)
		gpGlobals = ResolveGlobals();

	// Раз в секунду обновляем таблицу времени подключения по факту присутствия.
	double now = (double)std::time(nullptr);
	if (now - g_flLastPoll < 1.0)
		RETURN_META(MRES_IGNORED);
	g_flLastPoll = now;

	if (!g_pEntitySystem)
		RETURN_META(MRES_IGNORED);

	for (int i = 0; i < 64; i++)
	{
		CCSPlayerController *pController = CCSPlayerController::FromSlot(i);
		bool connected = pController && pController->IsConnected();
		if (connected)
		{
			if (g_iConnectTime[i] == 0)
				g_iConnectTime[i] = (int)std::time(nullptr);
		}
		else
		{
			g_iConnectTime[i] = 0;
		}
	}
	RETURN_META(MRES_IGNORED);
}

// ── ISmmPlugin ──────────────────────────────────────────────────────
bool KillhausMonitor::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceServiceServer, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);

	SH_ADD_HOOK_MEMFUNC(ISource2Server, GameFrame, server, &g_KillhausMonitor, &KillhausMonitor::Hook_GameFrame, true);

	ConVar_Register(FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);
	return true;
}

bool KillhausMonitor::Unload(char *error, size_t maxlen)
{
	if (server)
		SH_REMOVE_HOOK_MEMFUNC(ISource2Server, GameFrame, server, &g_KillhausMonitor, &KillhausMonitor::Hook_GameFrame, true);
	ConVar_Unregister();
	return true;
}

// ── Метаданные ──────────────────────────────────────────────────────
const char *KillhausMonitor::GetLicense() { return "GPLv3"; }
const char *KillhausMonitor::GetVersion() { return "1.0.0"; }
const char *KillhausMonitor::GetDate() { return __DATE__; }
const char *KillhausMonitor::GetLogTag() { return "KillhausMonitor"; }
const char *KillhausMonitor::GetAuthor() { return "KILLHAUS"; }
const char *KillhausMonitor::GetDescription() { return "mm_getinfo match data for web monitoring"; }
const char *KillhausMonitor::GetName() { return "KillhausMonitor"; }
const char *KillhausMonitor::GetURL() { return "https://killhaus.su"; }
