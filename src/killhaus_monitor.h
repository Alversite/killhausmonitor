/**
 * KillhausMonitor — самодостаточный Metamod:Source (Source2/CS2) плагин.
 *
 * Регистрирует консольную команду `mm_getinfo`, которая печатает JSON с данными
 * матча (карта, счёт CT/T, игроки с командой/K-D/HS/пингом/временем) — формат
 * совместим с бэкендом killhausremake (server/internal/store/monitor_rcon.go).
 *
 * В отличие от оригинального PlayersInfo (Pisex), НЕ зависит от плагинов
 * Utils/Players/LR: entity system и globals резолвятся самостоятельно, статы
 * читаются напрямую через schema (SchemaEntity).
 *
 * GPLv3 (унаследовано от исходников PlayersInfo/SchemaEntity).
 */
#ifndef _INCLUDE_KILLHAUS_MONITOR_H_
#define _INCLUDE_KILLHAUS_MONITOR_H_

#include <ISmmPlugin.h>
#include <sh_vector.h>
#include <iserver.h>
#include <playerslot.h> // CPlayerSlot

class KillhausMonitor final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);

public: // Хук подключения клиента — учёт времени на сервере (playtime).
	void Hook_ClientPutInServer(CPlayerSlot slot, char const *name, int type, uint64 xuid);

public: // ISmmPlugin
	const char *GetAuthor();
	const char *GetName();
	const char *GetDescription();
	const char *GetURL();
	const char *GetLicense();
	const char *GetVersion();
	const char *GetDate();
	const char *GetLogTag();
};

extern KillhausMonitor g_KillhausMonitor;

#endif // _INCLUDE_KILLHAUS_MONITOR_H_
