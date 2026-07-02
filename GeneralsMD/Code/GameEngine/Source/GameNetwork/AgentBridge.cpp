// TheSuperHackers @feature agentbridge external synchronous control server (experimental)
#include "PreRTS.h"
#if RTS_BUILD_AGENT_BRIDGE

#include "GameNetwork/AgentBridge.h"
#include "Common/GlobalData.h"
#include "GameLogic/GameLogic.h"
#include <winsock.h>

AgentBridge* TheAgentBridge = NULL;

static Bool s_winsockUp = FALSE;

AgentBridge::AgentBridge()
	: m_listenSock(~0u), m_clientSock(~0u), m_framesPerStep(5),
	  m_framesSinceStep(0), m_awaitingFirstStep(TRUE) {}

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

// M0 stubs — completed in M1/M2:
AsciiString AgentBridge::buildObservation() {
	AsciiString s;
	s.format("{\"schema\":0,\"frame\":%u,\"done\":false}",
		TheGameLogic ? TheGameLogic->getFrame() : 0);
	return s;
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
