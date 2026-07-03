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
#include <winsock.h>

AgentBridge* TheAgentBridge = NULL;

static Bool s_winsockUp = FALSE;

AgentBridge::AgentBridge()
	: m_listenSock(~0u), m_clientSock(~0u), m_framesPerStep(5),
	  m_framesSinceStep(0), m_awaitingFirstStep(TRUE),
	  m_agentPlayerIndex(-1), m_lastApplied(0) {}

AgentBridge::~AgentBridge() { closeClient();
	if (m_listenSock != ~0u) { closesocket((SOCKET)m_listenSock); m_listenSock = ~0u; }
	if (s_winsockUp) { WSACleanup(); s_winsockUp = FALSE; } }

void AgentBridge::init()
{
	if (!TheGlobalData->m_agentBridge) return;

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

void AgentBridge::reset() { m_framesSinceStep = 0; m_awaitingFirstStep = TRUE; }
void AgentBridge::update() { /* driven via preLogicSync() from GameEngine::update() */ }

void AgentBridge::closeClient() {
	if (m_clientSock != ~0u) { closesocket((SOCKET)m_clientSock); m_clientSock = ~0u; }
}

Bool AgentBridge::acceptClientIfWaiting() {
	if (m_listenSock == ~0u || m_clientSock != ~0u) return (m_clientSock != ~0u);
	fd_set fds; FD_ZERO(&fds); FD_SET((SOCKET)m_listenSock, &fds);
	timeval tv; tv.tv_sec = 0; tv.tv_usec = 0; // non-blocking poll
	if (select(0, &fds, NULL, NULL, &tv) > 0) {
		SOCKET c = accept((SOCKET)m_listenSock, NULL, NULL);
		if (c != INVALID_SOCKET) { m_clientSock = (unsigned int)c;
			m_awaitingFirstStep = TRUE; m_framesSinceStep = 0;
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
	// sinks into an AsciiString below, which asserts past MAX_LEN (32767, debug-only) and
	// structurally corrupts past 64K (m_numCharsAllocated is unsigned short); action
	// batches are far smaller than this.
	if (len == 0 || len > 32u*1024u) { closeClient(); return FALSE; }
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
struct ObsBuilder { std::string* out; Int viewerIndex; Bool wantEnemies; Bool first; Int ownerIndex; };

static void appendUnitJson(Object* obj, void* ud) {
	ObsBuilder* b = (ObsBuilder*)ud;
	if (obj == NULL) return;
	if (b->wantEnemies) {
		ObjectShroudStatus s = obj->getShroudedStatus(b->viewerIndex);
		if (s != OBJECTSHROUD_CLEAR && s != OBJECTSHROUD_PARTIAL_CLEAR) return;
	}
	const Coord3D* p = obj->getPosition();
	Real hp = 0.0f, maxhp = 0.0f;
	if (obj->getBodyModule()) { hp = obj->getBodyModule()->getHealth(); maxhp = obj->getBodyModule()->getMaxHealth(); }
	AIUpdateInterface* ai = obj->getAIUpdateInterface();
	const char* kind = obj->getTemplate() ? obj->getTemplate()->getName().str() : "";
	AsciiString entry;
	if (b->wantEnemies) {
		// TheSuperHackers @feature agentbridge owner field on visible_enemies entries (schema v0 review decision)
		entry.format("%s{\"id\":%u,\"kind\":\"%s\",\"pos\":[%.1f,%.1f,%.1f],\"hp\":%.0f,\"maxhp\":%.0f,\"moving\":%s,\"player\":%d}",
			b->first ? "" : ",", (unsigned int)obj->getID(), kind, p->x, p->y, p->z, hp, maxhp,
			(ai && ai->isMoving()) ? "true" : "false", b->ownerIndex);
	} else {
		entry.format("%s{\"id\":%u,\"kind\":\"%s\",\"pos\":[%.1f,%.1f,%.1f],\"hp\":%.0f,\"maxhp\":%.0f,\"moving\":%s}",
			b->first ? "" : ",", (unsigned int)obj->getID(), kind, p->x, p->y, p->z, hp, maxhp,
			(ai && ai->isMoving()) ? "true" : "false");
	}
	b->out->append(entry.str());
	b->first = FALSE;
}

// M1: full observation — agent player's units, shroud-filtered enemy units, resources.
std::string AgentBridge::buildObservation() {
	// TheSuperHackers @feature agentbridge guard ThePlayerList: NULL -> agent stays NULL,
	// self_units/visible_enemies both come out empty instead of a crash.
	Player* agent = ThePlayerList
		? ((m_agentPlayerIndex < 0) ? ThePlayerList->getLocalPlayer() : ThePlayerList->getNthPlayer(m_agentPlayerIndex))
		: NULL;
	Int viewer = agent ? agent->getPlayerIndex() : 0;

	std::string out;
	AsciiString head;
	head.format("{\"schema\":0,\"frame\":%u,\"player\":{\"id\":%d,\"money\":%u,\"power_produced\":%d,\"power_used\":%d},",
		TheGameLogic ? TheGameLogic->getFrame() : 0, viewer,
		agent ? agent->getMoney()->countMoney() : 0,
		agent ? agent->getEnergy()->getProduction() : 0,
		agent ? agent->getEnergy()->getConsumption() : 0);
	out.append(head.str());

	out.append("\"self_units\":[");
	if (agent) { ObsBuilder sb = { &out, viewer, FALSE, TRUE, -1 }; agent->iterateObjects(appendUnitJson, &sb); }
	out.append("],\"visible_enemies\":[");
	if (agent && ThePlayerList) {
		Bool firstEnemy = TRUE;
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
			Player* p = ThePlayerList->getNthPlayer(i);
			if (!p || p == agent) continue;
			// TheSuperHackers @feature agentbridge enemies-only filter (schema v0 review decision):
			// visible_enemies must contain only objects of players in an ENEMIES relationship with
			// the agent player. API: Player::getRelationship(const Team*) against the candidate
			// player's default team (same idiom as AISkirmishPlayer::acquireEnemy / WeaponSet / etc).
			if (agent->getRelationship(p->getDefaultTeam()) != ENEMIES) continue;
			ObsBuilder eb = { &out, viewer, TRUE, firstEnemy, p->getPlayerIndex() };
			p->iterateObjects(appendUnitJson, &eb);
			firstEnemy = eb.first; // preserve comma state across players
		}
	}
	AsciiString tail;
	tail.format("],\"map\":{\"width\":%.0f,\"height\":%.0f},\"last_action\":{\"accepted\":true,\"reason\":\"\"},\"done\":false}",
		TheGameLogic ? TheGameLogic->getWidth() : 0.0f, TheGameLogic ? TheGameLogic->getHeight() : 0.0f);
	out.append(tail.str());
	return out;
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
	if (!isMove && !isAttack && !isAttackMove && !isStop) return "unknown_type";

	const AgentJsonValue* unitVal = action.getMember("unit");
	if (!unitVal || !unitVal->isNumber()) return "bad_args";
	double unitNum = unitVal->asNumber();
	if (unitNum < 1.0 || unitNum > 4294967295.0) return "no_such_unit";
	ObjectID unitId = (ObjectID)(UnsignedInt)unitNum;

	Object* obj = TheGameLogic->findObjectByID(unitId);
	if (!obj) return "no_such_unit";
	if (obj->getControllingPlayer() != agent) return "not_owned";
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

	Player* agent = NULL;
	if (ThePlayerList)
		agent = (m_agentPlayerIndex < 0) ? ThePlayerList->getLocalPlayer()
		                                 : ThePlayerList->getNthPlayer(m_agentPlayerIndex);
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

// Returns TRUE only while the bridge is CONTROLLING the clock: a client is
// connected AND a game is running. Otherwise returns FALSE so the engine paces
// normally — menus stay responsive and you can start a skirmish before/while a
// client is attached. When controlling, it forces one logic frame per call and
// blocks at every Nth frame to exchange observation/action with the client.
Bool AgentBridge::preLogicSync() {
	if (m_listenSock == ~0u) return FALSE;                       // bridge not listening
	acceptClientIfWaiting();                                     // opportunistic, non-blocking
	if (m_clientSock == ~0u) return FALSE;                       // no client → pace normally
	// TheSuperHackers @feature agentbridge envelope: interactive offline game only.
	// isInInteractiveGame() (unlike isInGame()) excludes GAME_NONE/GAME_SHELL, so a
	// connecting client can no longer seize the shell/menu game; TheNetwork == NULL keeps
	// this offline-only, since forcing logic frames in a network game would bypass
	// canUpdateNetworkGameLogic() and guarantee an out-of-sync.
	if (!TheGameLogic || !TheGameLogic->isInInteractiveGame() || TheNetwork != NULL) return FALSE;

	m_framesSinceStep++;
	if (m_awaitingFirstStep || m_framesSinceStep >= m_framesPerStep) {
		// Step boundary: report state, block for the next command, apply it.
		// TheSuperHackers @feature agentbridge send the std::string observation via the raw
		// length-prefixed overload (buildObservation() is unbounded, unlike AsciiString).
		std::string obs = buildObservation();
		if (!sendJson(obs.data(), (unsigned int)obs.size())) { closeClient(); return FALSE; }
		AsciiString cmd;
		if (!recvJson(cmd)) { closeClient(); return FALSE; }
		applyActions(cmd);                                      // M2 injects orders here
		m_framesSinceStep = 0;
		m_awaitingFirstStep = FALSE;
	}
	return TRUE;   // force exactly one logic frame this iteration (bypass pacer)
}

#endif // RTS_BUILD_AGENT_BRIDGE
