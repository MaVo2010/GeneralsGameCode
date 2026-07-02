#pragma once
// TheSuperHackers @feature agentbridge external synchronous control server (experimental)
#if RTS_BUILD_AGENT_BRIDGE

#include "Common/SubsystemInterface.h"
#include "Common/AsciiString.h"
// TheSuperHackers @feature agentbridge std::string accumulator for buildObservation() (unbounded, unlike AsciiString)
#include <string>

class AgentBridge : public SubsystemInterface
{
public:
	AgentBridge();
	virtual ~AgentBridge();

	virtual void init();
	virtual void reset();
	virtual void update();   // ticked manually from GameEngine::update()

	Bool isActive() const { return m_listenSock != ~0u; }

	// Called from the GameEngine::update() hook once per frame; drives the
	// synchronous step protocol (see AgentBridge.cpp). Returns TRUE if the bridge
	// is controlling the clock this frame (caller then forces one logic frame,
	// bypassing the frame pacer); FALSE means "not controlling — pace normally".
	Bool preLogicSync();

private:
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

	unsigned int m_listenSock;   // SOCKET (unsigned) or ~0u if none
	unsigned int m_clientSock;   // connected client or ~0u
	Int m_framesPerStep;         // logic frames advanced per step()
	Int m_framesSinceStep;       // counter toward m_framesPerStep
	Bool m_awaitingFirstStep;    // true until the client's first step/reset
	Int m_agentPlayerIndex;      // TheSuperHackers @feature agentbridge which player the agent controls; -1 = local player
};

extern AgentBridge* TheAgentBridge;

#endif // RTS_BUILD_AGENT_BRIDGE
