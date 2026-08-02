# KillhausMonitor

Самодостаточный Metamod:Source (CS2 / Source2) плагин. Регистрирует консольную
команду **`mm_getinfo`**, которая печатает JSON с данными матча — его по RCON
читает бэкенд killhausremake и показывает счёт CT/T + составы команд в модалке
`/servers`.

В отличие от оригинального `PlayersInfo` (Pisex), **не зависит** от плагинов
Utils/Players/LR: entity system и globals резолвятся сами, статы читаются напрямую
через schema (SchemaEntity). Достаточно поставить один этот плагин.

## Формат вывода `mm_getinfo`

```json
{
  "time": 1785657813,
  "current_map": "de_mirage",
  "score_ct": 7,
  "score_t": 5,
  "players": [
    { "userid": 0, "name": "player", "team": 3, "steamid": "7656119...",
      "kills": 14, "death": 9, "headshots": 6, "ping": 24, "playtime": 612 }
  ]
}
```
`team`: 3 = CT, 2 = T. Схема совпадает с парсером `server/internal/store/monitor_rcon.go`.

## Сборка

Собирать нужно под **актуальный** hl2sdk-cs2 — иначе будет `undefined symbol`
(именно из-за рассинхрона версий не грузился прежний .so).

### Вариант A — GitHub Actions (проще всего)
1. Залить эту папку как отдельный репозиторий на GitHub.
2. Actions → workflow **Build KillhausMonitor** соберёт под свежий hl2sdk-cs2 +
   metamod master и выложит артефакт `KillhausMonitor.zip` (см. `.github/workflows/build.yml`).
3. Скачать артефакт — внутри `addons/`.

### Вариант B — локально (Linux/WSL)
```bash
# зависимости: clang, python3, ambuild
git clone https://github.com/alliedmodders/ambuild && pip3 install ./ambuild
mkdir -p external && cd external
git clone --recursive -b master https://github.com/alliedmodders/metamod-source.git
git clone --recursive -b cs2 https://github.com/alliedmodders/hl2sdk.git hl2sdk-cs2
cd ..
git clone https://github.com/Pisex/SchemaEntity.git ../SchemaEntity   # рядом с этой папкой
mkdir build && cd build
python3 ../configure.py -s cs2 --targets x86_64 --enable-optimize \
  --hl2sdk-manifests=../hl2sdk-manifests --mms_path=../external/metamod-source \
  --hl2sdk-root=../external
ambuild
```
Готовый addon — в `build/package/addons/`.

## Установка на сервер
Скопировать содержимое `addons/` в `csgo/addons/`:
- `addons/metamod/KillhausMonitor.vdf`
- `addons/KillhausMonitor/KillhausMonitor.so`

Затем сменить карту или перезапустить сервер (Metamod грузит плагины при старте).
Проверить: RCON `meta list` → должен быть `KillhausMonitor`; `mm_getinfo` → JSON.

## Включение в бэкенде
В `server/.env`:
```
EXTENDED_CMD=mm_getinfo
RCON_1=<пароль> …   # уже прописаны
```
Перезапустить Go-сервер → модалка `/servers` начнёт показывать счёт CT/T и команды.

## Заметки по первой сборке
Код написан по рабочему исходнику `PlayersInfo` и хедерам SchemaEntity, но собрать
локально в этом окружении было нельзя. На первой сборке возможны мелкие правки:
- **Смещение GameEntitySystem** в `ResolveGameEntitySystem()` — `WIN_LINUX(0x58, 0x50)`.
  Если игроки/сущности не находятся, свериться с текущим значением (в CS2Fixes и др.).
- Дубликат/отсутствие символа `g_pSchemaSystem`/`g_pEntitySystem` — снять/добавить
  соответствующее определение в начале `killhaus_monitor.cpp`.
- Точные имена интерфейсов/хедеров (`ISource2Server`, `IGameResourceServiceServer`,
  `INetworkServerService`) — при ошибке компиляции поправить include.

Присылай лог сборки — быстро поправлю.
