// TheSuperHackers @feature agentbridge external synchronous control server (experimental)
#include "PreRTS.h"
#if RTS_BUILD_AGENT_BRIDGE

#include "GameNetwork/AgentBridge.h"
#include "Common/GlobalData.h"
#include "GameLogic/GameLogic.h"
// TheSuperHackers @feature agentbridge TheNetwork, to restrict the bridge to offline games
#include "GameNetwork/NetworkDefs.h"
// TheSuperHackers @feature agentbridge observation serializer includes (M1)
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/Money.h"
#include "Common/Energy.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/AIUpdate.h"
#include "Common/ThingTemplate.h"
// TheSuperHackers @feature agentbridge action-batch JSON parsing + MessageStream injection (M3)
#include "GameNetwork/AgentBridgeJson.h"
#include "Common/MessageStream.h"
#include "GameLogic/TerrainLogic.h"
// TheSuperHackers @feature agentbridge done flag from TheVictoryConditions (M3)
#include "GameLogic/VictoryConditions.h"
// TheSuperHackers @feature agentbridge M7 v2 own-unit economy extras: construction/production/build_options
#include "GameClient/ControlBar.h"
#include "Common/BuildAssistant.h"
#include "GameLogic/Module/ProductionUpdate.h"
// TheSuperHackers @feature agentbridge M7 v2 build/train verbs: resolve kind name -> ThingTemplate
#include "Common/ThingFactory.h"
// TheSuperHackers @feature agentbridge M13 schema 3: shroud raster, supply sources,
// upgrades, sciences and special-power readiness.
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "Common/Upgrade.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/SpecialPowerMaskType.h"
#include <winsock.h>

AgentBridge* TheAgentBridge = NULL;

static Bool s_winsockUp = FALSE;

AgentBridge::AgentBridge()
	: m_listenSock(~0u), m_clientSock(~0u), m_framesPerStep(5), m_controlling(FALSE),
	  m_framesSinceStep(0), m_awaitingFirstStep(TRUE),
	  // TheSuperHackers @feature agentbridge protocol v1 handshake (M3)
	  m_awaitingHello(TRUE),
	  m_agentPlayerIndex(-1), m_lastApplied(0),
	  // TheSuperHackers @feature agentbridge M13 observer mode + schema 3
	  m_observerMode(FALSE), m_schema(2), m_terrainSent(FALSE) {}

AgentBridge::~AgentBridge() { closeClient();
	if (m_listenSock != ~0u) { closesocket((SOCKET)m_listenSock); m_listenSock = ~0u; }
	if (s_winsockUp) { WSACleanup(); s_winsockUp = FALSE; } }

void AgentBridge::init()
{
	if (!TheGlobalData->m_agentBridge) return;

	// TheSuperHackers @feature agentbridge M13: observer configuration is startup config,
	// not per-session state, so it is read once here and never touched by rearmSessionState().
	m_observerMode = TheGlobalData->m_agentBridgeObserver;
	if (m_observerMode)
		m_agentPlayerIndex = TheGlobalData->m_agentBridgeObserverPlayer;

	if (!s_winsockUp) {
		WSADATA wsa; WORD ver = MAKEWORD(2,2);
		if (WSAStartup(ver, &wsa) != 0) { DEBUG_LOG(("AgentBridge: WSAStartup failed")); return; }
		s_winsockUp = TRUE;
	}
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) { DEBUG_LOG(("AgentBridge: socket() failed")); return; }
	BOOL yes = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
	sockaddr_in addr; memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
	addr.sin_port = htons((u_short)TheGlobalData->m_agentBridgePort);
	if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 1) != 0) {
		DEBUG_LOG(("AgentBridge: bind/listen failed on port %d", TheGlobalData->m_agentBridgePort));
		closesocket(s); return;
	}
	m_listenSock = (unsigned int)s;
	DEBUG_LOG(("AgentBridge: listening on 127.0.0.1:%d", TheGlobalData->m_agentBridgePort));
}

// TheSuperHackers @feature agentbridge M13: single definition of "a session starts now".
// Called from reset() (engine/game reset) AND acceptClientIfWaiting() (a new client on a
// still-running engine) — the two paths that used to carry duplicate, drift-prone lists.
void AgentBridge::rearmSessionState() {
	m_framesSinceStep = 0; m_awaitingFirstStep = TRUE; m_awaitingHello = TRUE;
	// TheSuperHackers @feature agentbridge fresh sessions report a clean last_action (M4)
	m_lastApplied = 0;
	m_lastRejected.clear();
	// TheSuperHackers @feature agentbridge M13: a new client renegotiates from scratch, and
	// owes a fresh terrain block — leaking either across clients desynchronises the decoder.
	m_schema = 2;
	m_terrainSent = FALSE;
}

// TheSuperHackers @feature agentbridge re-arm the v1 hello whenever the connection state resets (M3)
void AgentBridge::reset() {
	rearmSessionState();
	// TheSuperHackers @feature agentbridge M6C: a stale controlling flag must never
	// survive an engine reset into the frame-pacer gate.
	m_controlling = FALSE;
}
void AgentBridge::update() { /* driven via preLogicSync() from GameEngine::update() */ }

void AgentBridge::closeClient() {
	if (m_clientSock != ~0u) { closesocket((SOCKET)m_clientSock); m_clientSock = ~0u; }
	// TheSuperHackers @feature agentbridge M6: pacer must resume immediately on disconnect,
	// even if no further preLogicSync() runs before the next frame's pacer gate.
	m_controlling = FALSE;
}

Bool AgentBridge::acceptClientIfWaiting() {
	if (m_listenSock == ~0u || m_clientSock != ~0u) return (m_clientSock != ~0u);
	fd_set fds; FD_ZERO(&fds); FD_SET((SOCKET)m_listenSock, &fds);
	timeval tv; tv.tv_sec = 0; tv.tv_usec = 0; // non-blocking poll
	if (select(0, &fds, NULL, NULL, &tv) > 0) {
		SOCKET c = accept((SOCKET)m_listenSock, NULL, NULL);
		if (c != INVALID_SOCKET) { m_clientSock = (unsigned int)c;
			// TheSuperHackers @feature agentbridge new connection must re-send the v1 hello (M3)
			rearmSessionState();
			DEBUG_LOG(("AgentBridge: client connected")); return TRUE; }
	}
	return FALSE;
}

static Bool recvAll(SOCKET s, char* buf, int len) {
	int got = 0; while (got < len) { int n = recv(s, buf+got, len-got, 0);
		if (n <= 0) return FALSE; got += n; } return TRUE;
}
static Bool sendAll(SOCKET s, const char* buf, int len) {
	int sent = 0; while (sent < len) { int n = send(s, buf+sent, len-sent, 0);
		if (n <= 0) return FALSE; sent += n; } return TRUE;
}

Bool AgentBridge::recvJson(AsciiString& out) {
	if (m_clientSock == ~0u) return FALSE;
	unsigned char hdr[4];
	if (!recvAll((SOCKET)m_clientSock, (char*)hdr, 4)) { closeClient(); return FALSE; }
	unsigned int len = (hdr[0]<<24)|(hdr[1]<<16)|(hdr[2]<<8)|hdr[3];
	// TheSuperHackers @feature agentbridge cap at an AsciiString-safe size: the payload
	// sinks into an AsciiString below; AsciiString asserts above 32766 in debug; 32000
	// leaves margin. Action batches are far smaller than this.
	if (len == 0 || len > 32000u) { closeClient(); return FALSE; }
	char* buf = (char*)malloc(len+1);
	if (!buf) { closeClient(); return FALSE; }
	if (!recvAll((SOCKET)m_clientSock, buf, (int)len)) { free(buf); closeClient(); return FALSE; }
	buf[len] = 0; out = buf; free(buf); return TRUE;
}

// TheSuperHackers @feature agentbridge raw length-prefixed send; both AsciiString and the
// std::string observation buffer (unbounded, unlike AsciiString) funnel through here.
Bool AgentBridge::sendJson(const char* data, unsigned int len) {
	if (m_clientSock == ~0u) return FALSE;
	unsigned char hdr[4] = { (unsigned char)(len>>24),(unsigned char)(len>>16),
	                         (unsigned char)(len>>8),(unsigned char)(len) };
	if (!sendAll((SOCKET)m_clientSock, (char*)hdr, 4)) { closeClient(); return FALSE; }
	// TheSuperHackers @feature agentbridge close on payload-send failure too (was a latent trap: only the header path did)
	if (!sendAll((SOCKET)m_clientSock, data, (int)len)) { closeClient(); return FALSE; }
	return TRUE;
}

Bool AgentBridge::sendJson(const AsciiString& json) {
	return sendJson(json.str(), (unsigned int)json.getLength());
}

// TheSuperHackers @feature agentbridge observation serializer (M1)
// Carries userData (output buffer + which player's view to shroud-filter against,
// plus the owning player's index for enemy entries) through Player::iterateObjects()'s
// C-style callback. `out` is std::string: AsciiString is capped at MAX_LEN (32767,
// asserted) and wraps structurally past 64K (unsigned short m_numCharsAllocated) —
// large armies would overflow it, so the accumulator must be unbounded.
struct ObsBuilder { std::string* out; Int viewerIndex; Bool wantEnemies; Bool first; Int ownerIndex; Int schema; Bool observerMode; };

// TheSuperHackers @feature agentbridge (M13) raster resolution for terrain and shroud.
// Deliberately coarse: terrain travels once per session, but the shroud raster goes out
// with EVERY observation, so this number is a bandwidth decision as much as a fidelity one.
static const Int AGENT_GRID_N = 32;

// TheSuperHackers @feature agentbridge (M13) world position of a raster cell's centre.
// Map extents start at the origin (W3DTerrainLogic::getExtent sets lo = 0), so the world
// span is [0, getWidth()) x [0, getHeight()) and no offset is needed.
static void agentCellCentre(Int ix, Int iy, Real& wx, Real& wy)
{
	const Real w = TheGameLogic ? TheGameLogic->getWidth() : 0.0f;
	const Real h = TheGameLogic ? TheGameLogic->getHeight() : 0.0f;
	wx = ((Real)ix + 0.5f) * w / (Real)AGENT_GRID_N;
	wy = ((Real)iy + 0.5f) * h / (Real)AGENT_GRID_N;
}

// TheSuperHackers @feature agentbridge (M13) static terrain: height raster plus a
// passability string ('0' open, '1' cliff, '2' water), row-major, cell (ix,iy) at iy*N+ix.
// Sent once per session — 1024 heights and 1024 characters are far too heavy per step, and
// none of it ever changes during a game.
static void appendTerrainJson(std::string& out)
{
	AsciiString piece;
	out.append("\"terrain\":{\"n\":");
	piece.format("%d", AGENT_GRID_N);
	out.append(piece.str());
	out.append(",\"height\":[");
	std::string passable;
	passable.reserve(AGENT_GRID_N * AGENT_GRID_N);
	for (Int iy = 0; iy < AGENT_GRID_N; ++iy)
	{
		for (Int ix = 0; ix < AGENT_GRID_N; ++ix)
		{
			Real wx, wy;
			agentCellCentre(ix, iy, wx, wy);
			Int z = 0;
			char pass = '0';
			if (TheTerrainLogic)
			{
				z = (Int)(TheTerrainLogic->getGroundHeight(wx, wy) + 0.5f);
				pass = TheTerrainLogic->isUnderwater(wx, wy) ? '2'
					: (TheTerrainLogic->isCliffCell(wx, wy) ? '1' : '0');
			}
			piece.format("%s%d", (ix || iy) ? "," : "", z);
			out.append(piece.str());
			passable += pass;
		}
	}
	out.append("],\"passable\":\"");
	out.append(passable);
	out.append("\"}");
}

// TheSuperHackers @feature agentbridge (M13) per-perspective shroud raster:
// '0' clear, '1' fogged (seen before, not currently visible), '2' never explored.
// PartitionCell indexes its per-player shroud array without an upper bound check, so the
// player index is validated here rather than trusted.
static void appendShroudJson(std::string& out, Int playerIndex)
{
	std::string cells;
	cells.reserve(AGENT_GRID_N * AGENT_GRID_N);
	const Bool canQuery = ThePartitionManager && playerIndex >= 0 && playerIndex < MAX_PLAYER_COUNT;
	for (Int iy = 0; iy < AGENT_GRID_N; ++iy)
	{
		for (Int ix = 0; ix < AGENT_GRID_N; ++ix)
		{
			char c = '2';
			if (canQuery)
			{
				Real wx, wy;
				agentCellCentre(ix, iy, wx, wy);
				Coord3D loc;
				loc.set(wx, wy, 0.0f);
				const CellShroudStatus s = ThePartitionManager->getShroudStatusForPlayer(playerIndex, &loc);
				c = (s == CELLSHROUD_CLEAR) ? '0' : ((s == CELLSHROUD_FOGGED) ? '1' : '2');
			}
			cells += c;
		}
	}
	out.append("\"shroud\":\"");
	out.append(cells);
	out.append("\"");
}

// M7: own-unit economy extras (v2). Appends nothing when not applicable.
static void appendOwnUnitExtrasJson(Object* obj, std::string* out)
{
	AsciiString piece;
	if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
	{
		piece.format(",\"under_construction\":true,\"construction_pct\":%.1f",
			obj->getConstructionPercent());
		out->append(piece.str());
	}
	ProductionUpdateInterface* pu = obj->getProductionUpdateInterface();
	if (pu && pu->getProductionCount() > 0)
	{
		out->append(",\"production\":[");
		Bool first = TRUE;
		for (const ProductionEntry* e = pu->firstProduction(); e; e = pu->nextProduction(e))
		{
			if (!e->getProductionObject()) continue;
			piece.format("%s{\"kind\":\"%s\",\"pct\":%.1f}", first ? "" : ",",
				e->getProductionObject()->getName().str(), e->getPercentComplete());
			out->append(piece.str());
			first = FALSE;
		}
		out->append("]");
	}
	if (TheControlBar && TheBuildAssistant && !obj->getCommandSetString().isEmpty())
	{
		const CommandSet* cs = TheControlBar->findCommandSet(obj->getCommandSetString());
		if (cs)
		{
			std::string opts;
			Bool any = FALSE;
			for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
			{
				const CommandButton* btn = cs->getCommandButton(i);
				if (!btn) continue;
				if (btn->getCommandType() != GUI_COMMAND_UNIT_BUILD
						&& btn->getCommandType() != GUI_COMMAND_DOZER_CONSTRUCT) continue;
				const ThingTemplate* tt = btn->getThingTemplate();
				if (!tt) continue;
				Bool ok = (TheBuildAssistant->canMakeUnit(obj, tt) == CANMAKE_OK);
				piece.format("%s{\"kind\":\"%s\",\"cost\":%d,\"ok\":%s}", any ? "," : "",
					tt->getName().str(), tt->calcCostToBuild(obj->getControllingPlayer()),
					ok ? "true" : "false");
				opts.append(piece.str());
				any = TRUE;
			}
			if (any)
			{
				out->append(",\"build_options\":[");
				out->append(opts);
				out->append("]");
			}
		}
	}
}

// TheSuperHackers @feature agentbridge (M13, schema 3) extras that make sense for ANY unit,
// friendly or hostile — unlike the economy extras above, which only exist for owned objects.
// Under schema 2 this is never called, so those entries stay byte-identical.
static void appendCommonUnitExtrasJson(Object* obj, std::string* out)
{
	// getLargestWeaponRange() reports -1 for objects with no weapons at all; clamp so the
	// wire carries "no reach" as 0 rather than a magic negative.
	Real weaponRange = obj->getLargestWeaponRange();
	if (weaponRange < 0.0f)
		weaponRange = 0.0f;
	UnsignedInt victimId = 0;
	const AIUpdateInterface* ai = obj->getAIUpdateInterface();
	if (ai)
	{
		// Two nulls to clear: the victim ID may be unset, and it may name an object that
		// has since died — findObjectByID then returns NULL.
		const Object* victim = ai->getCurrentVictim();
		if (victim)
			victimId = (UnsignedInt)victim->getID();
	}
	AsciiString piece;
	// "structure" comes straight from KINDOF rather than being guessed client-side from the
	// kind name. The existing rl-side substring rule only ever knew six unit types from M6/M7
	// and would classify almost every real building (power plants, war factories, tunnels,
	// defensive structures) as a mobile unit.
	piece.format(",\"veterancy\":%d,\"vision_range\":%.0f,\"weapon_range\":%.0f,\"target_id\":%u,\"structure\":%s",
		(Int)obj->getVeterancyLevel(), obj->getVisionRange(), weaponRange, victimId,
		obj->isKindOf(KINDOF_STRUCTURE) ? "true" : "false");
	out->append(piece.str());
}

static void appendUnitJson(Object* obj, void* ud) {
	ObsBuilder* b = (ObsBuilder*)ud;
	if (obj == NULL) return;
	if (b->wantEnemies) {
		// TheSuperHackers @feature agentbridge (M13) Object::getShroudedStatus() is declared
		// const but forwards to PartitionData::getShroudedStatus(), which is NOT const: it
		// writes m_shroudedness[playerIndex], m_everSeenByPlayer[playerIndex] and ghost-object
		// snapshots. For the local player the simulation performs that query anyway, so the
		// pre-M13 call site is harmless. The observer, however, asks on behalf of players the
		// simulation never asks about, which populates memo state for indices that would
		// otherwise stay untouched — and PartitionManager::unRegisterObject branches on
		// wasSeenByAnyPlayers(), so an attached observer would change how object teardown is
		// handled. That would break exactly the read-only property the replay gate rests on.
		// The cell query below is genuinely const and costs one grid lookup.
		if (b->observerMode) {
			CellShroudStatus s = CELLSHROUD_SHROUDED;
			if (ThePartitionManager && b->viewerIndex >= 0 && b->viewerIndex < MAX_PLAYER_COUNT)
				s = ThePartitionManager->getShroudStatusForPlayer(b->viewerIndex, obj->getPosition());
			if (s != CELLSHROUD_CLEAR) return;
		} else {
			ObjectShroudStatus s = obj->getShroudedStatus(b->viewerIndex);
			if (s != OBJECTSHROUD_CLEAR && s != OBJECTSHROUD_PARTIAL_CLEAR) return;
		}
	}
	const Coord3D* p = obj->getPosition();
	Real hp = 0.0f, maxhp = 0.0f;
	if (obj->getBodyModule()) { hp = obj->getBodyModule()->getHealth(); maxhp = obj->getBodyModule()->getMaxHealth(); }
	AIUpdateInterface* ai = obj->getAIUpdateInterface();
	const char* kind = obj->getTemplate() ? obj->getTemplate()->getName().str() : "";
	AsciiString entry;
	if (b->wantEnemies) {
		// TheSuperHackers @feature agentbridge owner field on visible_enemies entries (schema v0 review decision)
		// M13: the brace moved out of the format string so schema-3 extras can precede it.
		// Under schema 2 nothing is inserted, so the emitted bytes are unchanged.
		entry.format("%s{\"id\":%u,\"kind\":\"%s\",\"pos\":[%.1f,%.1f,%.1f],\"hp\":%.0f,\"maxhp\":%.0f,\"moving\":%s,\"player\":%d",
			b->first ? "" : ",", (unsigned int)obj->getID(), kind, p->x, p->y, p->z, hp, maxhp,
			(ai && ai->isMoving()) ? "true" : "false", b->ownerIndex);
	} else {
		// M7 v2: own units omit the closing brace here so appendOwnUnitExtrasJson can add
		// economy fields before it.
		entry.format("%s{\"id\":%u,\"kind\":\"%s\",\"pos\":[%.1f,%.1f,%.1f],\"hp\":%.0f,\"maxhp\":%.0f,\"moving\":%s",
			b->first ? "" : ",", (unsigned int)obj->getID(), kind, p->x, p->y, p->z, hp, maxhp,
			(ai && ai->isMoving()) ? "true" : "false");
	}
	b->out->append(entry.str());
	// TheSuperHackers @feature agentbridge (M13) enemies finally get extras too — until now
	// they carried nothing beyond position and health, which is why the agent could see an
	// approaching force but not judge it.
	if (b->schema >= 3)
		appendCommonUnitExtrasJson(obj, b->out);
	if (!b->wantEnemies)
		appendOwnUnitExtrasJson(obj, b->out);
	b->out->append("}");
	b->first = FALSE;
}

// M7: "win"/"lose" once TheVictoryConditions has decided, NULL while undecided.
// Draw edge (both defeated same frame): reads as "lose" by design (SPEC-M7).
static const char* victoryResultString(Player* agent)
{
	if (!agent || !TheVictoryConditions)
		return NULL;
	if (TheVictoryConditions->hasAchievedVictory(agent))
		return "win";
	if (TheVictoryConditions->hasBeenDefeated(agent))
		return "lose";
	return NULL;
}

// TheSuperHackers @feature agentbridge (M13) science names, built once. ScienceType is not a
// dense enum — the values are NameKeys minted while parsing INI — so ownership can only be
// probed name by name. friend_getScienceNames() allocates a vector per call, hence the latch.
static const std::vector<AsciiString>& agentScienceNames()
{
	static std::vector<AsciiString> s_names;
	static Bool s_filled = FALSE;
	if (!s_filled && TheScienceStore)
	{
		s_names = TheScienceStore->friend_getScienceNames();
		s_filled = TRUE;
	}
	return s_names;
}

// TheSuperHackers @feature agentbridge (M13) collects a player's special powers across all
// their objects, deduplicated by type.
struct PowerScan { std::string* out; Bool first; Bool seen[SPECIALPOWER_COUNT]; };

static void scanPowersJson(Object* obj, void* ud)
{
	PowerScan* s = (PowerScan*)ud;
	if (obj == NULL || !obj->hasAnySpecialPower()) return;
	for (Int t = 1; t < SPECIALPOWER_COUNT; ++t)
	{
		if (s->seen[t]) continue;
		const SpecialPowerType type = (SpecialPowerType)t;
		if (!obj->hasSpecialPower(type)) continue;
		SpecialPowerModuleInterface* mod = obj->findSpecialPowerModuleInterface(type);
		if (!mod) continue;
		s->seen[t] = TRUE;

		// TheSuperHackers @feature agentbridge (M13) NEVER call SpecialPowerModule::isReady() or
		// getReadyFrame() on a shared-and-synced power: for those they delegate to
		// Player::getOrStartSpecialPowerReadyFrame(), which APPENDS a timer entry when none
		// exists yet — a write hiding behind a const accessor, and a timer created on the wrong
		// frame diverges the CRC. Shared powers are read through peekSpecialPowerReadyFrame()
		// instead, which reports "no timer yet" rather than creating one; readiness is then null.
		const SpecialPowerTemplate* tmpl = mod->getSpecialPowerTemplate();
		const Bool shared = tmpl && tmpl->isSharedNSync();
		const UnsignedInt now = TheGameLogic ? TheGameLogic->getFrame() : 0;
		Bool known = TRUE;
		Bool ready = FALSE;
		Int readyFrame = -1;
		if (shared)
		{
			UnsignedInt frame = 0;
			Player* owner = obj->getControllingPlayer();
			known = owner && owner->peekSpecialPowerReadyFrame(tmpl, &frame);
			if (known)
			{
				readyFrame = (Int)frame;
				ready = (now >= frame);
			}
		}
		else
		{
			ready = mod->isReady();
			readyFrame = (Int)mod->getReadyFrame();
		}
		const char* name = SpecialPowerMaskType::getNameFromSingleBit(t);
		AsciiString piece;
		piece.format("%s{\"type\":\"%s\",\"ready\":%s,\"ready_frame\":%d}",
			s->first ? "" : ",", name ? name : "",
			known ? (ready ? "true" : "false") : "null", readyFrame);
		s->out->append(piece.str());
		s->first = FALSE;
	}
}

// TheSuperHackers @feature agentbridge (M13) everything about a player that is not a unit:
// economy, rank, upgrades, sciences, powers. Emitted for both sides in observer mode; for the
// agent alone in normal play, so a playing agent never gains a view its opponent could not have.
static void appendPlayerStateJson(std::string& out, Player* p)
{
	AsciiString piece;
	const Money* money = p->getMoney();
	const Energy* energy = p->getEnergy();
	ScoreKeeper* score = p->getScoreKeeper();
	const char* result = victoryResultString(p);
	piece.format("{\"index\":%d,\"type\":%d,\"is_ai\":%s,\"money\":%u,\"cash_per_minute\":%u,"
		"\"power_produced\":%d,\"power_used\":%d,\"earned\":%d,\"spent\":%d,"
		"\"defeated\":%s,\"result\":%s%s%s,"
		"\"rank\":%d,\"skill_points\":%d,\"science_points\":%d",
		p->getPlayerIndex(), (Int)p->getPlayerType(),
		p->isSkirmishAIPlayer() ? "true" : "false",
		money ? money->countMoney() : 0, money ? money->getCashPerMinute() : 0,
		energy ? energy->getProduction() : 0, energy ? energy->getConsumption() : 0,
		score ? score->getTotalMoneyEarned() : 0, score ? score->getTotalMoneySpent() : 0,
		p->isPlayerDead() ? "true" : "false",
		result ? "\"" : "", result ? result : "null", result ? "\"" : "",
		p->getRankLevel(), p->getSkillPoints(), p->getSciencePurchasePoints());
	out.append(piece.str());

	// One walk over the ~86 upgrade templates fills both lists.
	out.append(",\"upgrades_done\":[");
	std::string pending;
	Bool firstDone = TRUE, firstPending = TRUE;
	for (UpgradeTemplate* u = TheUpgradeCenter ? TheUpgradeCenter->firstUpgradeTemplate() : NULL;
			u; u = u->friend_getNext())
	{
		if (p->hasUpgradeComplete(u))
		{
			piece.format("%s\"%s\"", firstDone ? "" : ",", u->getUpgradeName().str());
			out.append(piece.str());
			firstDone = FALSE;
		}
		else if (p->hasUpgradeInProduction(u))     // note: not const, hence the non-const Player*
		{
			piece.format("%s\"%s\"", firstPending ? "" : ",", u->getUpgradeName().str());
			pending.append(piece.str());
			firstPending = FALSE;
		}
	}
	out.append("],\"upgrades_pending\":[");
	out.append(pending);
	out.append("],\"sciences\":[");
	if (TheScienceStore)
	{
		const std::vector<AsciiString>& names = agentScienceNames();
		Bool firstScience = TRUE;
		for (size_t i = 0; i < names.size(); ++i)
		{
			const ScienceType st = TheScienceStore->getScienceFromInternalName(names[i]);
			if (st == SCIENCE_INVALID || !p->hasScience(st)) continue;
			piece.format("%s\"%s\"", firstScience ? "" : ",", names[i].str());
			out.append(piece.str());
			firstScience = FALSE;
		}
	}
	out.append("],\"powers\":[");
	PowerScan ps;
	ps.out = &out;
	ps.first = TRUE;
	for (Int i = 0; i < SPECIALPOWER_COUNT; ++i) ps.seen[i] = FALSE;
	p->iterateObjects(scanPowersJson, &ps);
	out.append("]}");
}

// TheSuperHackers @feature agentbridge (M13) supply sources with their remaining boxes.
// The cheap KINDOF test comes first so the module lookup only runs for the handful of real
// warehouses rather than every object in the world (idiom from AIPlayer.cpp:2217-2225).
// A source without a warehouse module reports boxes -1 instead of being dropped — its
// position alone is already information.
static void appendSupplySourcesJson(std::string& out)
{
	out.append("\"supply_sources\":[");
	static const NameKeyType key_warehouseUpdate = NAMEKEY("SupplyWarehouseDockUpdate");
	Bool first = TRUE;
	AsciiString piece;
	for (Object* obj = TheGameLogic ? TheGameLogic->getFirstObject() : NULL;
			obj; obj = obj->getNextObject())
	{
		if (!obj->isKindOf(KINDOF_SUPPLY_SOURCE)) continue;
		Int boxes = -1;
		SupplyWarehouseDockUpdate* wh =
			(SupplyWarehouseDockUpdate*)obj->findUpdateModule(key_warehouseUpdate);
		if (wh) boxes = wh->getBoxesStored();
		const Coord3D* pos = obj->getPosition();
		piece.format("%s{\"id\":%u,\"pos\":[%.1f,%.1f],\"boxes\":%d}", first ? "" : ",",
			(unsigned int)obj->getID(), pos->x, pos->y, boxes);
		out.append(piece.str());
		first = FALSE;
	}
	out.append("]");
}

// TheSuperHackers @feature agentbridge (M13) the combatants of the running game.
// ThePlayerList is NOT just the players you see in the lobby: index 0 is the neutral
// player (PlayerList.cpp:232-243), a FactionCivilian side survives prepareForMP_or_Skirmish
// (SidesList.cpp:495-506), and a "ReplayObserver" side is appended unconditionally
// (GameLogic.cpp:1524-1547). So indices are never safe to assume — isPlayableSide() is the
// filter that leaves exactly the factions that are actually playing. It deliberately keeps
// DEFEATED players (isPlayerActive() would drop them the moment they lose, which is precisely
// when we still need to record who lost).
static Bool isCombatant(Player* p)
{
	return p && p->isPlayableSide();
}

// TheSuperHackers @feature agentbridge (M13) the player an observation speaks for.
// Normal mode is unchanged (local player). Observer mode takes the requested index, or
// falls back to the first live combatant so that the top-level result/done stay meaningful.
Player* AgentBridge::resolveAgentPlayer()
{
	if (!ThePlayerList)
		return NULL;
	if (m_agentPlayerIndex >= 0)
	{
		Player* wanted = ThePlayerList->getNthPlayer(m_agentPlayerIndex);
		// In observer mode an index that is not a combatant (index 0 is the neutral player,
		// and a mistyped argument lands there) would otherwise yield empty perspectives and a
		// result that never resolves — the episode then runs to its time cap and is discarded
		// with nothing to show for it. Fall back loudly instead of failing silently.
		if (!m_observerMode || isCombatant(wanted))
			return wanted;
		DEBUG_LOG(("AgentBridge: observer player index %d is not a combatant, using the first one",
			m_agentPlayerIndex));
	}
	if (!m_observerMode)
		return ThePlayerList->getLocalPlayer();
	// TheSuperHackers @feature agentbridge (M13) deliberately NOT "the first ACTIVE combatant".
	// isPlayerActive() goes false the moment a player is defeated, so preferring active players
	// would silently hand the top-level player/result/done block to the other side exactly at
	// the decisive moment — every decided game would then report result "win" no matter who won.
	// The first combatant in list order is stable for the whole game, which is what a caller
	// reading the top-level block across steps needs.
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player* p = ThePlayerList->getNthPlayer(i);
		if (isCombatant(p))
			return p;
	}
	return NULL;
}

// TheSuperHackers @feature agentbridge (M13) one player's view of the world: their own
// objects plus the hostile objects their shroud currently permits. Factored out of
// buildObservation so the observer can emit it once per perspective.
void AgentBridge::appendPlayerViewJson(std::string& out, Player* p)
{
	const Int viewer = p ? p->getPlayerIndex() : 0;
	out.append("\"self_units\":[");
	if (p) { ObsBuilder sb = { &out, viewer, FALSE, TRUE, -1, m_schema, m_observerMode }; p->iterateObjects(appendUnitJson, &sb); }
	out.append("],\"visible_enemies\":[");
	if (p && ThePlayerList) {
		Bool firstEnemy = TRUE;
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
			Player* q = ThePlayerList->getNthPlayer(i);
			if (!q || q == p) continue;
			// TheSuperHackers @feature agentbridge enemies-only filter (schema v0 review decision):
			// visible_enemies must contain only objects of players in an ENEMIES relationship with
			// the agent player. API: Player::getRelationship(const Team*) against the candidate
			// player's default team (same idiom as AISkirmishPlayer::acquireEnemy / WeaponSet / etc).
			if (p->getRelationship(q->getDefaultTeam()) != ENEMIES) continue;
			ObsBuilder eb = { &out, viewer, TRUE, firstEnemy, q->getPlayerIndex(), m_schema, m_observerMode };
			q->iterateObjects(appendUnitJson, &eb);
			firstEnemy = eb.first; // preserve comma state across players
		}
	}
	out.append("]");
}

// M1: full observation — agent player's units, shroud-filtered enemy units, resources.
std::string AgentBridge::buildObservation() {
	// TheSuperHackers @feature agentbridge guard ThePlayerList: NULL -> agent stays NULL,
	// self_units/visible_enemies both come out empty instead of a crash.
	Player* agent = resolveAgentPlayer();
	Int viewer = agent ? agent->getPlayerIndex() : 0;
	// TheSuperHackers @feature agentbridge (M13) under schema 3 an observer puts the per-player
	// unit lists into "perspectives" instead, so the flat pair would only be a duplicate of the
	// first perspective — and unit lists are the bulk of an observation. They stay present but
	// empty to keep one single shape for the decoder.
	const Bool multiView = (m_schema >= 3) && m_observerMode;

	std::string out;
	AsciiString head;
	// TheSuperHackers @feature agentbridge M7 schema 2: adds top-level result (win/lose/null) in the tail
	head.format("{\"schema\":%d,\"frame\":%u,\"player\":{\"id\":%d,\"money\":%u,\"power_produced\":%d,\"power_used\":%d},",
		m_schema, TheGameLogic ? TheGameLogic->getFrame() : 0, viewer,
		agent ? agent->getMoney()->countMoney() : 0,
		agent ? agent->getEnergy()->getProduction() : 0,
		agent ? agent->getEnergy()->getConsumption() : 0);
	out.append(head.str());

	if (multiView)
		out.append("\"self_units\":[],\"visible_enemies\":[]");
	else
		appendPlayerViewJson(out, agent);

	// TheSuperHackers @feature agentbridge v1 tail: real last_action results + done from TheVictoryConditions (M3)
	// M13: the map block is written in two pieces so the terrain raster can slot in. Under
	// schema 2 nothing slots in and the concatenation is byte-identical to the old format string.
	AsciiString tail;
	tail.format(",\"map\":{\"width\":%.0f,\"height\":%.0f",
		TheGameLogic ? TheGameLogic->getWidth() : 0.0f,
		TheGameLogic ? TheGameLogic->getHeight() : 0.0f);
	out.append(tail.str());
	if (m_schema >= 3 && !m_terrainSent)
	{
		out.append(",");
		appendTerrainJson(out);
		m_terrainSent = TRUE;
	}
	tail.format("},\"last_action\":{\"applied\":%d,\"rejected\":[", m_lastApplied);
	out.append(tail.str());
	for (size_t i = 0; i < m_lastRejected.size(); ++i)
	{
		AsciiString rej;
		rej.format("%s{\"index\":%d,\"reason\":\"%s\"}",
			i ? "," : "", m_lastRejected[i].index, m_lastRejected[i].reason);
		out.append(rej.str());
	}
	Bool done = FALSE;
	if (agent && TheVictoryConditions)
		done = TheVictoryConditions->hasAchievedVictory(agent) || TheVictoryConditions->hasBeenDefeated(agent);
	// TheSuperHackers @feature agentbridge M7 v2 tail: result (win/lose/null) precedes done
	const char* result = victoryResultString(agent);
	tail.format(",\"result\":%s%s%s,\"done\":%s}",
		result ? "\"" : "", result ? result : "null", result ? "\"" : "",
		done ? "true" : "false");
	out.append("]}");                 // close rejected array + last_action object

	// TheSuperHackers @feature agentbridge (M13) schema-3 blocks live here, between the close
	// of last_action and the result/done closer. This is the one seam that needs no change to
	// any pre-existing format string, so schema 2 stays byte-identical by construction.
	if (m_schema >= 3)
	{
		AsciiString piece;
		// Wall-clock is forbidden in GameLogic; this is derived from the logic frame, which is
		// exactly what makes it reproducible.
		piece.format(",\"time_s\":%.1f",
			TheGameLogic ? (Real)TheGameLogic->getFrame() / (Real)LOGICFRAMES_PER_SECOND : 0.0f);
		out.append(piece.str());

		out.append(",\"players\":[");
		if (m_observerMode && ThePlayerList)
		{
			Bool firstPlayer = TRUE;
			for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
			{
				Player* p = ThePlayerList->getNthPlayer(i);
				if (!isCombatant(p)) continue;
				if (!firstPlayer) out.append(",");
				appendPlayerStateJson(out, p);
				firstPlayer = FALSE;
			}
		}
		else if (agent)
		{
			// No cheat channel: a playing agent sees its own economy and nobody else's.
			appendPlayerStateJson(out, agent);
		}
		out.append("],");
		appendSupplySourcesJson(out);

		if (multiView && ThePlayerList)
		{
			out.append(",\"perspectives\":[");
			Bool firstView = TRUE;
			for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
			{
				Player* p = ThePlayerList->getNthPlayer(i);
				if (!isCombatant(p)) continue;
				// A single requested perspective is pinned to the player resolveAgentPlayer()
				// settled on, so a bad index cannot leave this array empty.
				if (m_agentPlayerIndex >= 0 && p != agent) continue;
				piece.format("%s{\"index\":%d,", firstView ? "" : ",", p->getPlayerIndex());
				out.append(piece.str());
				appendPlayerViewJson(out, p);
				out.append(",");
				appendShroudJson(out, p->getPlayerIndex());
				out.append("}");
				firstView = FALSE;
			}
			out.append("]");
		}
	}

	out.append(tail.str());
	return out;
}

// TheSuperHackers @feature agentbridge M7: teardown notification (bye + win/lose result).
void AgentBridge::onGameEnding()
{
	// M7: fires from GameEngine::reset() BEFORE subsystems reset — frame and
	// victory state are still readable here (VictoryConditions resets later).
	if (m_clientSock == ~0u)
		return;                                   // no client attached
	if (!TheGameLogic || !TheGameLogic->isInGame())
		return;                                   // save/load or non-game reset
	// TheSuperHackers @feature agentbridge (M13) same player the observations spoke for. Using
	// getLocalPlayer() here meant the farewell always carried result null in observer mode,
	// because with -selfai the local player is the non-combatant ReplayObserver, which
	// VictoryConditions excludes from its player list entirely.
	Player* agent = resolveAgentPlayer();
	const char* result = victoryResultString(agent);
	AsciiString bye;
	// M13: echoes the negotiated schema; for a schema-2 client this is the M7 message verbatim.
	bye.format("{\"schema\":%d,\"frame\":%u,\"done\":true,\"result\":%s%s%s}",
		m_schema, TheGameLogic->getFrame(),
		result ? "\"" : "", result ? result : "null", result ? "\"" : "");
	sendJson(bye);
	closeClient();
}

// TheSuperHackers @feature agentbridge validate one action and inject it via the
// player message path. Returns NULL when injected, else a static reason string.
const char* AgentBridge::applyOneAction(const AgentJsonValue& action, Player* agent)
{
	if (!action.isObject()) return "bad_args";
	const AgentJsonValue* type = action.getMember("type");
	if (!type || !type->isString()) return "bad_args";

	const std::string& t = type->asString();
	Bool isMove = (t == "move");
	Bool isAttack = (t == "attack");
	Bool isAttackMove = (t == "attack_move");
	Bool isStop = (t == "stop");
	// TheSuperHackers @feature agentbridge M7 v2 verbs: dozer build + producer train
	Bool isBuild = (t == "build");
	Bool isTrain = (t == "train");
	if (!isMove && !isAttack && !isAttackMove && !isStop && !isBuild && !isTrain) return "unknown_type";

	const AgentJsonValue* unitVal = action.getMember("unit");
	if (!unitVal || !unitVal->isNumber()) return "bad_args";
	double unitNum = unitVal->asNumber();
	if (unitNum < 1.0 || unitNum > 4294967295.0) return "no_such_unit";
	if (unitNum != (double)(UnsignedInt)unitNum) return "bad_args"; // TheSuperHackers @feature agentbridge fractional ids are client bugs, not truncation targets (M4)
	ObjectID unitId = (ObjectID)(UnsignedInt)unitNum;

	Object* obj = TheGameLogic->findObjectByID(unitId);
	if (!obj) return "no_such_unit";
	if (obj->getControllingPlayer() != agent) return "not_owned";

	// TheSuperHackers @feature agentbridge M7 v2 build/train: resolve kind->template and inject
	// select-then-act BEFORE the no_ai check — buildings have no AIUpdateInterface, so the no_ai
	// reject below only applies to move/attack/attack_move/stop. Injection is message-only
	// (MSG_CREATE_SELECTED_GROUP + network-range order), CRC-safe like the v1 verbs.
	if (isBuild || isTrain)
	{
		const AgentJsonValue* kindV = action.getMember("kind");
		if (!kindV || !kindV->isString()) return "bad_args";
		// check=FALSE: a bad name must NOT trip findTemplate's DEBUG_CRASH in debug builds.
		const ThingTemplate* tt = TheThingFactory
			? TheThingFactory->findTemplate(AsciiString(kindV->asString().c_str()), FALSE)
			: NULL;
		if (!tt) return "unknown_kind";

		if (isTrain)
		{
			// Reject a missing PU interface BEFORE injecting: onQueueUnitCreate DEBUG_CRASHes
			// if the producer lacks one.
			ProductionUpdateInterface* pu = obj->getProductionUpdateInterface();
			if (!pu) return "no_production";
			if (!TheBuildAssistant || TheBuildAssistant->canMakeUnit(obj, tt) != CANMAKE_OK)
				return "cannot_build";
			ProductionID pid = pu->requestUniqueUnitID(); // in-process, UI-identical -> replay-safe
			GameMessage* sel = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
			sel->appendBooleanArgument(TRUE);
			sel->appendObjectIDArgument(obj->getID());
			GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UNIT_CREATE);
			msg->appendIntegerArgument(tt->getTemplateID());
			msg->appendIntegerArgument((Int)pid);
			return NULL;
		}

		// build: only a dozer constructs structures.
		if (!obj->isKindOf(KINDOF_DOZER)) return "cannot_build";
		// pos parse + terrain-extent reject — mirrors the move-verb block (incl. NaN guard).
		const AgentJsonValue* pos = action.getMember("pos");
		if (!pos || !pos->isArray() || pos->size() < 2) return "bad_args";
		const AgentJsonValue* px = pos->at(0);
		const AgentJsonValue* py = pos->at(1);
		if (!px || !py || !px->isNumber() || !py->isNumber()) return "bad_args";
		Real bx = (Real)px->asNumber();
		Real by = (Real)py->asNumber();
		if (!(bx == bx) || !(by == by)) return "bad_args"; // NaN guard
		Region3D bextent;
		TheTerrainLogic->getExtent(&bextent);
		if (bx < bextent.lo.x || bx > bextent.hi.x || by < bextent.lo.y || by > bextent.hi.y)
			return "bad_target";
		Coord3D loc;
		loc.set(bx, by, 0.0f);
		if (!TheBuildAssistant || TheBuildAssistant->canMakeUnit(obj, tt) != CANMAKE_OK)
			return "cannot_build";
		// Exact flag set from PlaceEventTranslator.cpp:230-236 (the UI placement click).
		UnsignedInt lb = BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::TERRAIN_RESTRICTIONS
			| BuildAssistant::CLEAR_PATH | BuildAssistant::NO_OBJECT_OVERLAP
			| BuildAssistant::SHROUD_REVEALED | BuildAssistant::IGNORE_STEALTHED
			| BuildAssistant::FAIL_STEALTHED_WITHOUT_FEEDBACK;
		if (TheBuildAssistant->isLocationLegalToBuild(&loc, tt, 0.0f, lb, obj, NULL) != LBC_OK)
			return "bad_target";
		GameMessage* sel = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
		sel->appendBooleanArgument(TRUE);
		sel->appendObjectIDArgument(obj->getID());
		GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT);
		msg->appendIntegerArgument(tt->getTemplateID());
		msg->appendLocationArgument(loc);
		msg->appendRealArgument(0.0f); // angle fixed 0 (SPEC-M7; not on the wire)
		return NULL;
	}

	if (!obj->getAIUpdateInterface()) return "no_ai";

	Coord3D dest;
	dest.x = dest.y = dest.z = 0.0f;
	Object* victim = NULL;
	if (isMove || isAttackMove)
	{
		const AgentJsonValue* pos = action.getMember("pos");
		if (!pos || !pos->isArray() || pos->size() < 2) return "bad_args";
		const AgentJsonValue* px = pos->at(0);
		const AgentJsonValue* py = pos->at(1);
		if (!px || !py || !px->isNumber() || !py->isNumber()) return "bad_args";
		Real x = (Real)px->asNumber();
		Real y = (Real)py->asNumber();
		if (!(x == x) || !(y == y)) return "bad_args"; // NaN guard
		Region3D extent;
		TheTerrainLogic->getExtent(&extent);
		if (x < extent.lo.x || x > extent.hi.x || y < extent.lo.y || y > extent.hi.y) return "bad_target";
		dest.set(x, y, 0.0f);
	}
	else if (isAttack)
	{
		const AgentJsonValue* target = action.getMember("target");
		if (!target || !target->isNumber()) return "bad_args";
		double targetNum = target->asNumber();
		if (targetNum < 1.0 || targetNum > 4294967295.0) return "bad_target";
		if (targetNum != (double)(UnsignedInt)targetNum) return "bad_args"; // TheSuperHackers @feature agentbridge (M4)
		victim = TheGameLogic->findObjectByID((ObjectID)(UnsignedInt)targetNum);
		if (!victim) return "bad_target";
		if (agent->getRelationship(victim->getTeam()) != ENEMIES) return "bad_target";
	}

	// TheSuperHackers @feature agentbridge select-then-act on TheMessageStream —
	// identical to a player order: deterministic, replay-recorded, CRC-safe.
	GameMessage* sel = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
	sel->appendBooleanArgument(TRUE); // start a fresh selection
	sel->appendObjectIDArgument(unitId);

	if (isMove)
	{
		GameMessage* m = TheMessageStream->appendMessage(GameMessage::MSG_DO_MOVETO);
		m->appendLocationArgument(dest);
	}
	else if (isAttackMove)
	{
		GameMessage* m = TheMessageStream->appendMessage(GameMessage::MSG_DO_ATTACKMOVETO);
		m->appendLocationArgument(dest);
	}
	else if (isAttack)
	{
		GameMessage* m = TheMessageStream->appendMessage(GameMessage::MSG_DO_ATTACK_OBJECT);
		m->appendObjectIDArgument(victim->getID());
	}
	else // stop
	{
		TheMessageStream->appendMessage(GameMessage::MSG_DO_STOP);
	}

	return NULL;
}

void AgentBridge::applyActions(const AsciiString& actionsJson)
{
	m_lastApplied = 0;
	m_lastRejected.clear();

	// TheSuperHackers @feature agentbridge (M13) observer mode: no action ever reaches the
	// message stream. This is not a policy but the load-bearing safety property — it is what
	// lets the frame gate below admit replay playback at all. Rejections are still reported
	// per action so the client sees WHY nothing happened, in the unchanged last_action shape.
	if (m_observerMode)
	{
		AgentJsonValue observed;
		const AgentJsonValue* list = NULL;
		if (AgentJsonValue::parse(actionsJson.str(), observed) && observed.isObject())
			list = observed.getMember("actions");
		const size_t n = (list && list->isArray()) ? list->size() : 0;
		for (size_t i = 0; i < n; ++i)
		{
			RejectedAction r = { (Int)i, "observer_mode" };
			m_lastRejected.push_back(r);
		}
		return;
	}

	Player* agent = resolveAgentPlayer();
	AgentJsonValue root;
	if (!agent || !TheMessageStream || !AgentJsonValue::parse(actionsJson.str(), root) || !root.isObject())
	{
		RejectedAction r = { -1, "parse_error" };
		m_lastRejected.push_back(r);
		return;
	}
	const AgentJsonValue* actions = root.getMember("actions");
	if (!actions || !actions->isArray())
	{
		RejectedAction r = { -1, "parse_error" };
		m_lastRejected.push_back(r);
		return;
	}
	for (size_t i = 0; i < actions->size(); ++i)
	{
		const char* reason = applyOneAction(*actions->at(i), agent);
		if (reason == NULL)
			m_lastApplied++;
		else
		{
			RejectedAction r = { (Int)i, reason };
			m_lastRejected.push_back(r);
		}
	}
}

// TheSuperHackers @feature agentbridge protocol v1 handshake (DESIGN §8.1)
Bool AgentBridge::processHello(const AsciiString& helloJson)
{
	AgentJsonValue root;
	if (!AgentJsonValue::parse(helloJson.str(), root) || !root.isObject()) return FALSE;
	const AgentJsonValue* cmd = root.getMember("cmd");
	if (!cmd || !cmd->isString() || cmd->asString() != "hello") return FALSE;
	// TheSuperHackers @feature agentbridge (M13) accept 2 or 3. A client asking for 2 gets
	// exactly the M7 wire format back; the schema-3 blocks are all additive and gated.
	const AgentJsonValue* schema = root.getMember("schema");
	if (!schema || !schema->isNumber()) return FALSE;
	const double wanted = schema->asNumber();
	if (wanted != 2.0 && wanted != 3.0) return FALSE;
	const AgentJsonValue* fps = root.getMember("frames_per_step");
	if (!fps || !fps->isNumber()) return FALSE;
	double n = fps->asNumber();
	if (n < 1.0 || n > 60.0 || n != (double)(Int)n) return FALSE;
	m_framesPerStep = (Int)n;
	m_schema = (Int)wanted;
	return TRUE;
}

// Returns TRUE only while the bridge is CONTROLLING the clock: a client is
// connected AND a game is running. Otherwise returns FALSE so the engine paces
// normally — menus stay responsive and you can start a skirmish before/while a
// client is attached. When controlling, it forces one logic frame per call and
// blocks at every Nth frame to exchange observation/action with the client.
Bool AgentBridge::preLogicSyncInternal()
{
	if (m_listenSock == ~0u) return FALSE;                       // bridge not listening
	acceptClientIfWaiting();                                     // opportunistic, non-blocking
	if (m_clientSock == ~0u) return FALSE;

	// TheSuperHackers @feature agentbridge envelope: interactive offline game,
	// never during replay playback (would diverge the CRC we verify against).
	//
	// TheSuperHackers @feature agentbridge (M13) observer mode lifts the replay exclusion.
	// The original reason only ever covered INJECTION: applyActions() above returns without
	// touching TheMessageStream while m_observerMode is set, so playback has nothing to
	// diverge from. Reading is CRC-neutral by construction, and the -jobs replay check is
	// the standing proof. Network games stay excluded — self-play is not in scope.
	//
	// Scope note (verified, M13): this only opens the WINDOWED replay path. Headless replay
	// (-headless -replay) runs ReplaySimulation::simulateReplays() in Core/, which drives
	// TheGameLogic->UPDATE() directly and never calls GameEngine::update() — so preLogicSync()
	// is not reached there at all, and -jobs workers are not passed -agentbridge either.
	// Observing a replay therefore means running it windowed.
	const Bool replayOk = m_observerMode || (TheGameLogic && !TheGameLogic->isInReplayGame());
	Bool gateOpen = TheGameLogic && TheGameLogic->isInInteractiveGame()
		&& replayOk && TheNetwork == NULL;
	if (!gateOpen)
		return FALSE;      // M7: teardown bye now fires from onGameEnding()

	m_framesSinceStep++;
	if (m_awaitingFirstStep || m_framesSinceStep >= m_framesPerStep)
	{
		if (m_awaitingHello)
		{
			AsciiString hello;
			if (!recvJson(hello)) { closeClient(); return FALSE; }
			if (!processHello(hello))
			{
				sendJson(AsciiString("{\"error\":\"bad_hello\",\"expect_schema\":[2,3]}"));
				closeClient();
				return FALSE;
			}
			m_awaitingHello = FALSE;
		}
		std::string obs = buildObservation();
		if (!sendJson(obs.data(), (unsigned int)obs.size())) { closeClient(); return FALSE; }
		AsciiString cmd;
		if (!recvJson(cmd)) { closeClient(); return FALSE; }
		applyActions(cmd);
		m_framesSinceStep = 0;
		m_awaitingFirstStep = FALSE;
	}
	return TRUE;   // force exactly one logic frame this iteration (bypass pacer)
}

Bool AgentBridge::preLogicSync()
{
	// TheSuperHackers @feature agentbridge record the controlling state for the
	// frame-pacer gate in GameEngine::execute().
	m_controlling = preLogicSyncInternal();
	return m_controlling;
}

#endif // RTS_BUILD_AGENT_BRIDGE
