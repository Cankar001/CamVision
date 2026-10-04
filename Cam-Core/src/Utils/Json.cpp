#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Core
{
	namespace
	{
		const Json &NullValue()
		{
			static const Json null_value;
			return null_value;
		}

		void AppendUtf8(std::string &out, uint32 code)
		{
			if (code < 0x80)
			{
				out.push_back((char)code);
			}
			else if (code < 0x800)
			{
				out.push_back((char)(0xC0 | (code >> 6)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}
			else if (code < 0x10000)
			{
				out.push_back((char)(0xE0 | (code >> 12)));
				out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}
			else
			{
				out.push_back((char)(0xF0 | (code >> 18)));
				out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
				out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
				out.push_back((char)(0x80 | (code & 0x3F)));
			}
		}

		void DumpString(std::string &out, const std::string &text)
		{
			out.push_back('"');
			for (unsigned char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					case '\b': out += "\\b"; break;
					case '\f': out += "\\f"; break;
					default:
						if (c < 0x20)
						{
							char escape[8];
							snprintf(escape, sizeof(escape), "\\u%04x", c);
							out += escape;
						}
						else
						{
							out.push_back((char)c);
						}
						break;
				}
			}

			out.push_back('"');
		}

		class Parser
		{
		public:

			Parser(const std::string &text)
				: m_Text(text)
			{
			}

			bool ParseDocument(Json *out, std::string *error)
			{
				SkipSpace();
				if (!ParseValue(out, 0))
				{
					*error = m_Error + " (at position " + std::to_string(m_Position) + ")";
					return false;
				}

				SkipSpace();
				if (m_Position != m_Text.size())
				{
					*error = "unexpected text after the value (at position " + std::to_string(m_Position) + ")";
					return false;
				}

				return true;
			}

		private:

			bool Fail(const char *message)
			{
				m_Error = message;
				return false;
			}

			void SkipSpace()
			{
				while (m_Position < m_Text.size() && (m_Text[m_Position] == ' ' || m_Text[m_Position] == '\t' || m_Text[m_Position] == '\n' || m_Text[m_Position] == '\r'))
				{
					++m_Position;
				}
			}

			bool Match(const char *word)
			{
				size_t length = strlen(word);
				if (m_Text.compare(m_Position, length, word) == 0)
				{
					m_Position += length;
					return true;
				}

				return false;
			}

			bool ParseValue(Json *out, int depth)
			{
				if (depth > 32)
				{
					return Fail("nested too deep");
				}

				if (m_Position >= m_Text.size())
				{
					return Fail("the text ends too early");
				}

				char c = m_Text[m_Position];
				if (c == '{')
				{
					return ParseObject(out, depth);
				}

				if (c == '[')
				{
					return ParseArray(out, depth);
				}

				if (c == '"')
				{
					std::string text;
					if (!ParseString(&text))
					{
						return false;
					}

					*out = Json(text);
					return true;
				}

				if (Match("true"))
				{
					*out = Json(true);
					return true;
				}

				if (Match("false"))
				{
					*out = Json(false);
					return true;
				}

				if (Match("null"))
				{
					*out = Json();
					return true;
				}

				return ParseNumber(out);
			}

			bool ParseNumber(Json *out)
			{
				size_t start = m_Position;
				if (m_Position < m_Text.size() && m_Text[m_Position] == '-')
				{
					++m_Position;
				}

				size_t digits = 0;
				while (m_Position < m_Text.size() && ((m_Text[m_Position] >= '0' && m_Text[m_Position] <= '9') || m_Text[m_Position] == '.' || m_Text[m_Position] == 'e' || m_Text[m_Position] == 'E' || m_Text[m_Position] == '+' || m_Text[m_Position] == '-'))
				{
					++digits;
					++m_Position;
				}

				if (digits == 0)
				{
					m_Position = start;
					return Fail("a value was expected");
				}

				std::string number = m_Text.substr(start, m_Position - start);
				char *end = nullptr;
				double value = strtod(number.c_str(), &end);
				if (*end != 0 || !std::isfinite(value))
				{
					m_Position = start;
					return Fail("not a valid number");
				}

				*out = Json(value);
				return true;
			}

			bool ParseHex4(uint32 *out)
			{
				if (m_Position + 4 > m_Text.size())
				{
					return Fail("a \\u escape is cut off");
				}

				uint32 value = 0;
				for (int i = 0; i < 4; ++i)
				{
					char c = m_Text[m_Position + i];
					value <<= 4;
					if (c >= '0' && c <= '9') value |= (uint32)(c - '0');
					else if (c >= 'a' && c <= 'f') value |= (uint32)(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F') value |= (uint32)(c - 'A' + 10);
					else return Fail("a \\u escape needs 4 hex digits");
				}

				m_Position += 4;
				*out = value;
				return true;
			}

			bool ParseString(std::string *out)
			{
				++m_Position;		// the opening quote
				out->clear();
				while (m_Position < m_Text.size())
				{
					unsigned char c = (unsigned char)m_Text[m_Position++];
					if (c == '"')
					{
						return true;
					}

					if (c < 0x20)
					{
						return Fail("a control character in a text (it must be escaped)");
					}

					if (c != '\\')
					{
						out->push_back((char)c);
						continue;
					}

					if (m_Position >= m_Text.size())
					{
						break;
					}

					char escape = m_Text[m_Position++];
					switch (escape)
					{
						case '"': out->push_back('"'); break;
						case '\\': out->push_back('\\'); break;
						case '/': out->push_back('/'); break;
						case 'b': out->push_back('\b'); break;
						case 'f': out->push_back('\f'); break;
						case 'n': out->push_back('\n'); break;
						case 'r': out->push_back('\r'); break;
						case 't': out->push_back('\t'); break;
						case 'u':
						{
							uint32 code = 0;
							if (!ParseHex4(&code))
							{
								return false;
							}

							if (code >= 0xD800 && code < 0xDC00)
							{
								// The first half of a pair: the second half has to follow.
								uint32 low = 0;
								if (m_Position + 2 > m_Text.size() || m_Text[m_Position] != '\\' || m_Text[m_Position + 1] != 'u')
								{
									return Fail("half of a surrogate pair");
								}

								m_Position += 2;
								if (!ParseHex4(&low))
								{
									return false;
								}

								if (low < 0xDC00 || low > 0xDFFF)
								{
									return Fail("wrong second half of a surrogate pair");
								}

								code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
							}
							else if (code >= 0xDC00 && code <= 0xDFFF)
							{
								return Fail("half of a surrogate pair");
							}

							AppendUtf8(*out, code);
							break;
						}
						default:
							return Fail("an unknown escape in a text");
					}
				}

				return Fail("a text is not closed");
			}

			bool ParseArray(Json *out, int depth)
			{
				++m_Position;
				*out = Json::Array();
				SkipSpace();
				if (m_Position < m_Text.size() && m_Text[m_Position] == ']')
				{
					++m_Position;
					return true;
				}

				for (;;)
				{
					SkipSpace();
					Json item;
					if (!ParseValue(&item, depth + 1))
					{
						return false;
					}

					out->Push(item);
					SkipSpace();
					if (m_Position >= m_Text.size())
					{
						return Fail("a list is not closed");
					}

					char c = m_Text[m_Position++];
					if (c == ']')
					{
						return true;
					}

					if (c != ',')
					{
						--m_Position;
						return Fail("',' or ']' was expected");
					}
				}
			}

			bool ParseObject(Json *out, int depth)
			{
				++m_Position;
				*out = Json::Object();
				SkipSpace();
				if (m_Position < m_Text.size() && m_Text[m_Position] == '}')
				{
					++m_Position;
					return true;
				}

				for (;;)
				{
					SkipSpace();
					if (m_Position >= m_Text.size() || m_Text[m_Position] != '"')
					{
						return Fail("a name in quotes was expected");
					}

					std::string key;
					if (!ParseString(&key))
					{
						return false;
					}

					SkipSpace();
					if (m_Position >= m_Text.size() || m_Text[m_Position] != ':')
					{
						return Fail("':' was expected");
					}

					++m_Position;
					SkipSpace();
					Json value;
					if (!ParseValue(&value, depth + 1))
					{
						return false;
					}

					(*out)[key] = value;
					SkipSpace();
					if (m_Position >= m_Text.size())
					{
						return Fail("an object is not closed");
					}

					char c = m_Text[m_Position++];
					if (c == '}')
					{
						return true;
					}

					if (c != ',')
					{
						--m_Position;
						return Fail("',' or '}' was expected");
					}
				}
			}

			const std::string &m_Text;
			size_t m_Position = 0;
			std::string m_Error;
		};
	}

	Json Json::Array()
	{
		Json json;
		json.m_Type = Type::Array;
		return json;
	}

	Json Json::Object()
	{
		Json json;
		json.m_Type = Type::Object;
		return json;
	}

	bool Json::Has(const std::string &key) const
	{
		return Find(key) != nullptr;
	}

	const Json *Json::Find(const std::string &key) const
	{
		if (m_Type != Type::Object)
		{
			return nullptr;
		}

		for (const auto &member : m_Members)
		{
			if (member.first == key)
			{
				return &member.second;
			}
		}

		return nullptr;
	}

	Json &Json::operator[](const std::string &key)
	{
		if (m_Type != Type::Object)
		{
			*this = Json::Object();
		}

		for (auto &member : m_Members)
		{
			if (member.first == key)
			{
				return member.second;
			}
		}

		m_Members.emplace_back(key, Json());
		return m_Members.back().second;
	}

	const Json &Json::Get(const std::string &key) const
	{
		const Json *found = Find(key);
		return found ? *found : NullValue();
	}

	void Json::Push(const Json &value)
	{
		if (m_Type != Type::Array)
		{
			*this = Json::Array();
		}

		m_Items.push_back(value);
	}

	size_t Json::Size() const
	{
		if (m_Type == Type::Array)
		{
			return m_Items.size();
		}

		return m_Type == Type::Object ? m_Members.size() : 0;
	}

	const Json &Json::At(size_t index) const
	{
		return m_Type == Type::Array && index < m_Items.size() ? m_Items[index] : NullValue();
	}

	std::string Json::Dump() const
	{
		std::string out;
		DumpTo(out);
		return out;
	}

	void Json::DumpTo(std::string &out) const
	{
		switch (m_Type)
		{
			case Type::Null:
				out += "null";
				break;

			case Type::Bool:
				out += m_Bool ? "true" : "false";
				break;

			case Type::Number:
			{
				char number[40];
				if (std::isfinite(m_Number) && m_Number == std::floor(m_Number) && std::fabs(m_Number) < 9007199254740992.0)
				{
					snprintf(number, sizeof(number), "%lld", (long long)m_Number);
				}
				else if (std::isfinite(m_Number))
				{
					snprintf(number, sizeof(number), "%.15g", m_Number);
				}
				else
				{
					// JSON has no infinity and no NaN.
					snprintf(number, sizeof(number), "null");
				}

				out += number;
				break;
			}

			case Type::String:
				DumpString(out, m_String);
				break;

			case Type::Array:
			{
				out.push_back('[');
				for (size_t i = 0; i < m_Items.size(); ++i)
				{
					if (i > 0)
					{
						out.push_back(',');
					}

					m_Items[i].DumpTo(out);
				}

				out.push_back(']');
				break;
			}

			case Type::Object:
			{
				out.push_back('{');
				for (size_t i = 0; i < m_Members.size(); ++i)
				{
					if (i > 0)
					{
						out.push_back(',');
					}

					DumpString(out, m_Members[i].first);
					out.push_back(':');
					m_Members[i].second.DumpTo(out);
				}

				out.push_back('}');
				break;
			}
		}
	}

	bool Json::Parse(const std::string &text, Json *out, std::string *error)
	{
		std::string ignored;
		Parser parser(text);
		Json result;
		if (!parser.ParseDocument(&result, error ? error : &ignored))
		{
			return false;
		}

		*out = result;
		return true;
	}
}
