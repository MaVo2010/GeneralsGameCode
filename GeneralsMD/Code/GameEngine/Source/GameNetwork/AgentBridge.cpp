// TheSuperHackers @feature agentbridge external synchronous control server (experimental)
#include "PreRTS.h"
#if RTS_BUILD_AGENT_BRIDGE

#include "GameNetwork/AgentBridge.h"
#include "Common/GlobalData.h"
#include "GameLogic/GameLogic.h"
// TheSuperHackers @feature agentbridge observation serializer includes (M1)
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/Money.h"
#include "Common/Energy.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/AIUpdate.h"
#include "Common/ThingTemplate.h"
#include <winsock.h>

AgentBridge* TheAgentBridge = NULL;

static Bool s_winsockUp = FALSE;

AgentBridge::AgentBridge()
	: m_listenSock(~0u), m_clientSock(~0u), m_framesPerStep(5),
	  m_framesSinceStep(0), m_awaitingFirstStep(TRUE),
	  m_agentPlayerIndex(-1) {}

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
	if (len == 0 || len > 8u*1024u*1024u) { closeClient(); return FALSE; }
	char* buf = (char*)malloc(len+1);
	if (!buf) { closeClient(); return FALSE; }
	if (!recvAll((SOCKET)m_clientSock, buf, (int)len)) { free(buf); closeClient(); return FALSE; }
	buf[len] = 0; out = buf; free(buf); return TRUE;
}

Bool AgentBridge::sendJson(const AsciiString& json) {
	if (m_clientSock == ~0u) return FALSE;
	unsigned int len = (unsigned int)json.getLength();
	unsigned char hdr[4] = { (unsigned char)(len>>24),(unsigned char)(len>>16),
	                         (unsigned char)(len>>8),(unsigned char)(len) };
	if (!sendAll((SOCKET)m_clientSock, (char*)hdr, 4)) { closeClient(); return FALSE; }
	return sendAll((SOCKET)m_clientSock, json.str(), (int)len);
}

// TheSuperHackers @feature agentbridge observation serializer (M1)
// Carries userData (output buffer + which player's view to shroud-filter against)
// through Player::iterateObjects()'s C-style callback.
struct ObsBuilder { AsciiString* out; Int viewerIndex; Bool wantEnemies; Bool first; };

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
	entry.format("%s{\"id\":%u,\"kind\":\"%s\",\"pos\":[%.1f,%.1f,%.1f],\"hp\":%.0f,\"maxhp\":%.0f,\"moving\":%s}",
		b->first ? "" : ",", (unsigned int)obj->getID(), kind, p->x, p->y, p->z, hp, maxhp,
		(ai && ai->isMoving()) ? "true" : "false");
	b->out->concat(entry);
	b->first = FALSE;
}

// M1: full observation — agent player's units, shroud-filtered enemy units, resources.
AsciiString AgentBridge::buildObservation() {
	Player* agent = (m_agentPlayerIndex < 0)
		? ThePlayerList->getLocalPlayer()
		: ThePlayerList->getNthPlayer(m_agentPlayerIndex);
	Int viewer = agent ? agent->getPlayerIndex() : 0;

	AsciiString out;
	AsciiString head;
	head.format("{\"schema\":0,\"frame\":%u,\"player\":{\"id\":%d,\"money\":%u,\"power_produced\":%d,\"power_used\":%d},",
		TheGameLogic ? TheGameLogic->getFrame() : 0, viewer,
		agent ? agent->getMoney()->countMoney() : 0,
		agent ? agent->getEnergy()->getProduction() : 0,
		agent ? agent->getEnergy()->getConsumption() : 0);
	out.concat(head);

	out.concat("\"self_units\":[");
	if (agent) { ObsBuilder sb = { &out, viewer, FALSE, TRUE }; agent->iterateObjects(appendUnitJson, &sb); }
	out.concat("],\"visible_enemies\":[");
	Bool firstEnemy = TRUE;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i) {
		Player* p = ThePlayerList->getNthPlayer(i);
		if (!p || p == agent) continue;
		ObsBuilder eb = { &out, viewer, TRUE, firstEnemy };
		p->iterateObjects(appendUnitJson, &eb);
		firstEnemy = eb.first; // preserve comma state across players
	}
	AsciiString tail;
	tail.format("],\"map\":{\"width\":%.0f,\"height\":%.0f},\"last_action\":{\"accepted\":true,\"reason\":\"\"},\"done\":false}",
		TheGameLogic ? TheGameLogic->getWidth() : 0.0f, TheGameLogic ? TheGameLogic->getHeight() : 0.0f);
	out.concat(tail);
	return out;
}
void AgentBridge::applyActions(const AsciiString& /*actionsJson*/) { /* M2 */ }

// Returns TRUE only while the bridge is CONTROLLING the clock: a client is
// connected AND a game is running. Otherwise returns FALSE so the engine paces
// normally — menus stay responsive and you can start a skirmish before/while a
// client is attached. When controlling, it forces one logic frame per call and
// blocks at every Nth frame to exchange observation/action with the client.
Bool AgentBridge::preLogicSync() {
	if (m_listenSock == ~0u) return FALSE;                       // bridge not listening
	acceptClientIfWaiting();                                     // opportunistic, non-blocking
	if (m_clientSock == ~0u) return FALSE;                       // no client → pace normally
	if (!TheGameLogic || !TheGameLogic->isInGame()) return FALSE;// not in game → pace normally

	m_framesSinceStep++;
	if (m_awaitingFirstStep || m_framesSinceStep >= m_framesPerStep) {
		// Step boundary: report state, block for the next command, apply it.
		if (!sendJson(buildObservation())) { closeClient(); return FALSE; }
		AsciiString cmd;
		if (!recvJson(cmd)) { closeClient(); return FALSE; }
		applyActions(cmd);                                      // M2 injects orders here
		m_framesSinceStep = 0;
		m_awaitingFirstStep = FALSE;
	}
	return TRUE;   // force exactly one logic frame this iteration (bypass pacer)
}

#endif // RTS_BUILD_AGENT_BRIDGE
