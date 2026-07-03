#pragma once
// TheSuperHackers @feature agentbridge minimal JSON parser for the control protocol (experimental)
#if RTS_BUILD_AGENT_BRIDGE

#include <string>
#include <vector>
#include <map>

// Minimal JSON value + recursive-descent parser, tailored to the AgentBridge
// protocol. Zero dependencies. Numbers are doubles (non-finite values are rejected); duplicate object keys:
// last one wins; \uXXXX escapes are consumed and replaced with '?' (the
// protocol never sends them). Nesting is depth-capped.
class AgentJsonValue
{
public:
	enum Type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

	AgentJsonValue() : m_type(JSON_NULL), m_bool(false), m_number(0.0) {}

	Type getType() const { return m_type; }
	bool isObject() const { return m_type == JSON_OBJECT; }
	bool isArray()  const { return m_type == JSON_ARRAY; }
	bool isNumber() const { return m_type == JSON_NUMBER; }
	bool isString() const { return m_type == JSON_STRING; }
	bool isBool()   const { return m_type == JSON_BOOL; }

	double asNumber() const { return m_number; }
	bool asBool() const { return m_bool; }
	const std::string& asString() const { return m_string; }

	size_t size() const { return m_array.size(); }
	const AgentJsonValue* at(size_t i) const { return i < m_array.size() ? &m_array[i] : NULL; }
	const AgentJsonValue* getMember(const char* key) const
	{
		std::map<std::string, AgentJsonValue>::const_iterator it = m_object.find(key);
		return it != m_object.end() ? &it->second : NULL;
	}

	// Parses NUL-terminated text into out. Returns false on any syntax error
	// or trailing non-whitespace; out is reset to JSON_NULL on failure.
	static bool parse(const char* text, AgentJsonValue& out);

private:
	friend class AgentJsonParser;
	Type m_type;
	bool m_bool;
	double m_number;
	std::string m_string;
	std::vector<AgentJsonValue> m_array;
	std::map<std::string, AgentJsonValue> m_object;
};

#endif // RTS_BUILD_AGENT_BRIDGE
