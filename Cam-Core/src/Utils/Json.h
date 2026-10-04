#pragma once

#include "Core/Core.h"

#include <string>
#include <utility>
#include <vector>

namespace Core
{
	/// <summary>
	/// A JSON value (null, true/false, number, text, list, object): small, with no dependencies, for the commands of the WebSocket. Objects keep the order of
	/// their members.
	///
	///   Json answer = Json::Object();
	///   answer["ok"] = true;
	///   answer["files"] = Json::Array();
	///   answer["files"].Push("a.avi");
	///   std::string text = answer.Dump();
	///
	///   Json request; std::string error;
	///   if (Json::Parse(text, &request, &error)) { request["cmd"].AsString(); }
	/// </summary>
	class Json
	{
	public:

		enum class Type { Null, Bool, Number, String, Array, Object };

		Json() = default;
		Json(bool value) : m_Type(Type::Bool), m_Bool(value) {}
		Json(int value) : m_Type(Type::Number), m_Number((double)value) {}
		Json(uint32 value) : m_Type(Type::Number), m_Number((double)value) {}
		Json(int64 value) : m_Type(Type::Number), m_Number((double)value) {}
		Json(uint64 value) : m_Type(Type::Number), m_Number((double)value) {}
		Json(double value) : m_Type(Type::Number), m_Number(value) {}
		Json(const char *value) : m_Type(Type::String), m_String(value) {}
		Json(const std::string &value) : m_Type(Type::String), m_String(value) {}

		static Json Array();
		static Json Object();

		Type GetType() const { return m_Type; }
		bool IsNull() const { return m_Type == Type::Null; }
		bool IsBool() const { return m_Type == Type::Bool; }
		bool IsNumber() const { return m_Type == Type::Number; }
		bool IsString() const { return m_Type == Type::String; }
		bool IsArray() const { return m_Type == Type::Array; }
		bool IsObject() const { return m_Type == Type::Object; }

		// The value, or the fallback, if it is something else.
		bool AsBool(bool fallback = false) const { return IsBool() ? m_Bool : fallback; }
		double AsNumber(double fallback = 0.0) const { return IsNumber() ? m_Number : fallback; }
		int64 AsInt(int64 fallback = 0) const { return IsNumber() ? (int64)m_Number : fallback; }
		std::string AsString(const std::string &fallback = "") const { return IsString() ? m_String : fallback; }

		// ---- object
		bool Has(const std::string &key) const;

		/// <summary>
		/// The member with this key, or nullptr.
		/// </summary>
		const Json *Find(const std::string &key) const;

		/// <summary>
		/// The member with this key (it is added as null, if there is none; this turns a null value into an object).
		/// </summary>
		Json &operator[](const std::string &key);

		/// <summary>
		/// The member with this key, or null, if there is none (nothing is added).
		/// </summary>
		const Json &Get(const std::string &key) const;

		const std::vector<std::pair<std::string, Json>> &Members() const { return m_Members; }

		// ---- array
		void Push(const Json &value);
		size_t Size() const;
		const Json &At(size_t index) const;
		const std::vector<Json> &Items() const { return m_Items; }

		/// <summary>
		/// The value as JSON text on one line.
		/// </summary>
		std::string Dump() const;

		/// <summary>
		/// Reads JSON text. The whole text must be one value. Nesting deeper than 32 levels is refused.
		/// </summary>
		static bool Parse(const std::string &text, Json *out, std::string *error);

	private:

		void DumpTo(std::string &out) const;

		Type m_Type = Type::Null;
		bool m_Bool = false;
		double m_Number = 0.0;
		std::string m_String;
		std::vector<Json> m_Items;
		std::vector<std::pair<std::string, Json>> m_Members;
	};
}
