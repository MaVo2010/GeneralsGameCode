// TheSuperHackers @feature agentbridge minimal JSON parser (see header)
#include "PreRTS.h"
#if RTS_BUILD_AGENT_BRIDGE

#include "GameNetwork/AgentBridgeJson.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <float.h>

class AgentJsonParser
{
public:
	AgentJsonParser(const char* text) : m_p(text) {}

	bool parseDocument(AgentJsonValue& out)
	{
		skipWs();
		if (!parseValue(out, 0)) return false;
		skipWs();
		return *m_p == '\0';
	}

private:
	enum { MAX_DEPTH = 16 };
	const char* m_p;

	void skipWs() { while (*m_p == ' ' || *m_p == '\t' || *m_p == '\r' || *m_p == '\n') ++m_p; }

	bool parseValue(AgentJsonValue& out, int depth)
	{
		if (depth > MAX_DEPTH) return false;
		skipWs();
		switch (*m_p)
		{
			case '{': return parseObject(out, depth);
			case '[': return parseArray(out, depth);
			case '"': out.m_type = AgentJsonValue::JSON_STRING; return parseString(out.m_string);
			case 't': if (strncmp(m_p, "true", 4) == 0)  { m_p += 4; out.m_type = AgentJsonValue::JSON_BOOL; out.m_bool = true;  return true; } return false;
			case 'f': if (strncmp(m_p, "false", 5) == 0) { m_p += 5; out.m_type = AgentJsonValue::JSON_BOOL; out.m_bool = false; return true; } return false;
			case 'n': if (strncmp(m_p, "null", 4) == 0)  { m_p += 4; out.m_type = AgentJsonValue::JSON_NULL; return true; } return false;
			default:  return parseNumber(out);
		}
	}

	bool parseObject(AgentJsonValue& out, int depth)
	{
		++m_p; // consume '{'
		out.m_type = AgentJsonValue::JSON_OBJECT;
		skipWs();
		if (*m_p == '}') { ++m_p; return true; }
		for (;;)
		{
			skipWs();
			if (*m_p != '"') return false;
			std::string key;
			if (!parseString(key)) return false;
			skipWs();
			if (*m_p != ':') return false;
			++m_p;
			AgentJsonValue val;
			if (!parseValue(val, depth + 1)) return false;
			out.m_object[key] = val;
			skipWs();
			if (*m_p == ',') { ++m_p; continue; }
			if (*m_p == '}') { ++m_p; return true; }
			return false;
		}
	}

	bool parseArray(AgentJsonValue& out, int depth)
	{
		++m_p; // consume '['
		out.m_type = AgentJsonValue::JSON_ARRAY;
		skipWs();
		if (*m_p == ']') { ++m_p; return true; }
		for (;;)
		{
			AgentJsonValue val;
			if (!parseValue(val, depth + 1)) return false;
			out.m_array.push_back(val);
			skipWs();
			if (*m_p == ',') { ++m_p; continue; }
			if (*m_p == ']') { ++m_p; return true; }
			return false;
		}
	}

	bool parseString(std::string& out)
	{
		++m_p; // consume opening '"'
		out.clear();
		while (*m_p && *m_p != '"')
		{
			if (*m_p == '\\')
			{
				++m_p;
				switch (*m_p)
				{
					case '"':  out += '"';  break;
					case '\\': out += '\\'; break;
					case '/':  out += '/';  break;
					case 'b':  out += '\b'; break;
					case 'f':  out += '\f'; break;
					case 'n':  out += '\n'; break;
					case 'r':  out += '\r'; break;
					case 't':  out += '\t'; break;
					case 'u':
					{
						for (int i = 0; i < 4; ++i)
						{
							++m_p;
							if (!isxdigit((unsigned char)*m_p)) return false;
						}
						out += '?';
						break;
					}
					default: return false;
				}
				++m_p;
			}
			else
			{
				out += *m_p;
				++m_p;
			}
		}
		if (*m_p != '"') return false;
		++m_p;
		return true;
	}

	bool parseNumber(AgentJsonValue& out)
	{
		char* end = NULL;
		double v = strtod(m_p, &end);
		if (end == m_p) return false;
		if (!(v == v) || v > DBL_MAX || v < -DBL_MAX) return false; // reject NaN/inf (strtod accepts non-JSON "nan"/"inf")
		m_p = end;
		out.m_type = AgentJsonValue::JSON_NUMBER;
		out.m_number = v;
		return true;
	}
};

bool AgentJsonValue::parse(const char* text, AgentJsonValue& out)
{
	out = AgentJsonValue();
	if (text == NULL) return false;
	AgentJsonParser parser(text);
	if (parser.parseDocument(out)) return true;
	out = AgentJsonValue();
	return false;
}

#endif // RTS_BUILD_AGENT_BRIDGE
