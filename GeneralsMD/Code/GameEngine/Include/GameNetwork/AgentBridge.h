#pragma once
// TheSuperHackers @feature agentbridge external synchronous control server (experimental)
#if RTS_BUILD_AGENT_BRIDGE

#include "Common/SubsystemInterface.h"
#include "Common/AsciiString.h"
// TheSuperHackers @feature agentbridge std::string accumulator for buildObservation() (unbounded, unlike AsciiString)
#include <string>
// TheSuperHackers @feature agentbridge m_lastRejected batch results (M3)
#include <vector>

class AgentBridge : public SubsystemInterface
{
public:
	AgentBridge();
	virtual ~AgentBridge();

	virtual void init();
	virtual void reset();
	virtual void update();   // ticked manually from GameEngine::update()

	// TheSuperHackers @feature agentbridge M7: game-teardown notification (bye + result)
	void onGameEnding();

	Bool isActive() const { return m_listenSock != ~0u; }

	// TheSuperHackers @feature agentbridge TRUE while a connected client drives the
	// lockstep (last preLogicSync result). Read by the frame-pacer gate in
	// GameEngine::execute() to skip wall-clock pacing during agent-driven frames.
	Bool isControllingClock() const { return m_controlling; }

	// Called from the GameEngine::update() hook once per frame; drives the
	// synchronous step protocol (see AgentBridge.cpp). Returns TRUE if the bridge
	// is controlling the clock this frame (caller then forces one logic frame,
	// bypassing the frame pacer); FALSE means "not controlling — pace normally".
	Bool preLogicSync();

private:
	// TheSuperHackers @feature agentbridge M6: actual preLogicSync() body, wrapped by
	// preLogicSync() so every return path records m_controlling in one place.
	Bool preLogicSyncInternal();
	Bool acceptClientIfWaiting();     // non-blocking accept
	Bool recvJson(AsciiString& out);  // blocking, length-prefixed
	Bool sendJson(const AsciiString& json);
	// TheSuperHackers @feature agentbridge raw length-prefixed send; sendJson(AsciiString) delegates to this
	Bool sendJson(const char* data, unsigned int len);
	void closeClient();

	// TheSuperHackers @feature agentbridge std::string accumulator: AsciiString is capped at MAX_LEN (32767, asserted)
	// and wraps structurally past 64K (unsigned short m_numCharsAllocated) — large armies would overflow it.
	std::string buildObservation();   // filled out in M1
	void applyActions(const AsciiString& actionsJson); // filled out in M2
	// TheSuperHackers @feature agentbridge M13: the player an observation/action batch
	// speaks for. Shared by buildObservation() and applyActions() so both can never disagree.
	class Player* resolveAgentPlayer();

	// TheSuperHackers @feature agentbridge per-batch action results for last_action (M3)
	struct RejectedAction { Int index; const char* reason; };  // reason = static string
	const char* applyOneAction(const class AgentJsonValue& action, class Player* agent);
	// TheSuperHackers @feature agentbridge protocol v1 handshake (M3, DESIGN §8.1)
	Bool processHello(const AsciiString& helloJson); // validate + apply hello, FALSE = reject

	// TheSuperHackers @feature agentbridge M13: one place for the per-session state that
	// reset() and acceptClientIfWaiting() both have to re-arm. Keeping two hand-maintained
	// copies of that list in sync was the standing trap; a latch missed in the accept path
	// means the second client of a session silently gets a different protocol than it asked for.
	void rearmSessionState();

	unsigned int m_listenSock;   // SOCKET (unsigned) or ~0u if none
	unsigned int m_clientSock;   // connected client or ~0u
	Int m_framesPerStep;         // logic frames advanced per step()
	Bool m_controlling; ///< last preLogicSync() result (client owns the clock this frame)
	Int m_framesSinceStep;       // counter toward m_framesPerStep
	Bool m_awaitingFirstStep;    // true until the client's first step/reset
	// TheSuperHackers @feature agentbridge protocol v1 handshake (M3)
	Bool m_awaitingHello;    ///< true until the client's hello is validated
	Int m_agentPlayerIndex;      // TheSuperHackers @feature agentbridge which player the agent controls; -1 = local player

	// TheSuperHackers @feature agentbridge M13 observer mode + schema 3
	Bool m_observerMode;   ///< observe only: applyActions rejects everything with "observer_mode"
	Int  m_schema;         ///< schema negotiated in the hello (2 or 3); 2 stays byte-identical to M7
	Bool m_terrainSent;    ///< static terrain raster travels once per session, not once per step

	Int m_lastApplied;                        ///< actions injected by the last batch
	std::vector<RejectedAction> m_lastRejected; ///< rejected actions of the last batch
};

extern AgentBridge* TheAgentBridge;

#endif // RTS_BUILD_AGENT_BRIDGE
