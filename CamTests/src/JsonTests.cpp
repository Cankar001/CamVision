#include "CamTest.h"

#include "Utils/Json.h"

using Core::Json;

namespace
{
	Json Parse(const std::string &text)
	{
		Json json;
		std::string error;
		REQUIRE(Json::Parse(text, &json, &error));
		return json;
	}

	bool Fails(const std::string &text)
	{
		Json json;
		std::string error;
		bool ok = Json::Parse(text, &json, &error);
		return !ok && !error.empty();
	}
}

TEST(Json, ParsesAllTypes)
{
	Json json = Parse("{\"text\":\"hi\",\"number\":-12.5,\"yes\":true,\"no\":false,\"nothing\":null,\"list\":[1,2,3],\"inner\":{\"a\":1}}");
	REQUIRE(json.IsObject());
	CHECK_EQ(json.Get("text").AsString(), "hi");
	CHECK(json.Get("number").AsNumber() == -12.5);
	CHECK(json.Get("yes").AsBool());
	CHECK(!json.Get("no").AsBool(true));
	CHECK(json.Get("nothing").IsNull());
	CHECK_EQ(json.Get("list").Size(), (size_t)3);
	CHECK_EQ(json.Get("list").At(2).AsInt(), 3);
	CHECK_EQ(json.Get("inner").Get("a").AsInt(), 1);
}

TEST(Json, MissingValuesAreNullAndFallbacksApply)
{
	Json json = Parse("{\"a\":1}");
	CHECK(json.Get("missing").IsNull());
	CHECK(!json.Has("missing"));
	CHECK(json.Has("a"));
	CHECK_EQ(json.Get("a").AsString("fallback"), "fallback");		// a number is no text
	CHECK_EQ(json.Get("missing").AsInt(7), 7);
	CHECK(json.Get("list").At(5).IsNull());
}

TEST(Json, WhitespaceIsFine)
{
	Json json = Parse("  \n\t{ \"a\" : [ 1 , 2 ] ,\r\n \"b\" : \"x\" }  ");
	CHECK_EQ(json.Get("a").Size(), (size_t)2);
	CHECK_EQ(json.Get("b").AsString(), "x");
}

TEST(Json, DumpAndParseRoundTrip)
{
	Json json = Json::Object();
	json["name"] = "Front door";
	json["frames"] = 1500;
	json["seconds"] = 12.25;
	json["ok"] = true;
	json["nothing"] = Json();
	json["list"] = Json::Array();
	json["list"].Push(1);
	json["list"].Push("two");
	json["list"].Push(Json::Object());

	std::string text = json.Dump();
	CHECK_EQ(text, "{\"name\":\"Front door\",\"frames\":1500,\"seconds\":12.25,\"ok\":true,\"nothing\":null,\"list\":[1,\"two\",{}]}");

	Json again = Parse(text);
	CHECK_EQ(again.Dump(), text);
}

TEST(Json, ObjectsKeepTheOrderOfTheirMembers)
{
	Json json = Json::Object();
	json["z"] = 1;
	json["a"] = 2;
	json["m"] = 3;
	json["a"] = 4;		// replaces, keeps the place
	CHECK_EQ(json.Dump(), "{\"z\":1,\"a\":4,\"m\":3}");
}

TEST(Json, WholeNumbersHaveNoDecimals)
{
	CHECK_EQ(Json(5).Dump(), "5");
	CHECK_EQ(Json(-5).Dump(), "-5");
	CHECK_EQ(Json((int64)1234567890123).Dump(), "1234567890123");
	CHECK_EQ(Json(0.5).Dump(), "0.5");
	CHECK_EQ(Json(1e21).Dump().find("e") != std::string::npos, true);
}

TEST(Json, StringEscapes)
{
	Json json = Parse("\"a\\\"b\\\\c\\/d\\n\\t\\u0041\\u00e4\\u20ac\"");
	CHECK_EQ(json.AsString(), std::string("a\"b\\c/d\n\tA") + "\xC3\xA4" + "\xE2\x82\xAC");

	// And back: quotes, backslashes and control characters are escaped, other text stays as it is.
	Json text(std::string("line1\nline2\t\"quoted\" \\ \x01"));
	CHECK_EQ(text.Dump(), "\"line1\\nline2\\t\\\"quoted\\\" \\\\ \\u0001\"");
	CHECK_EQ(Json(std::string("\xC3\xA4")).Dump(), "\"\xC3\xA4\"");
}

TEST(Json, SurrogatePairsBecomeOneCharacter)
{
	// U+1F600 (a face) as a pair of \u escapes.
	Json json = Parse("\"\\ud83d\\ude00\"");
	CHECK_EQ(json.AsString(), "\xF0\x9F\x98\x80");

	CHECK(Fails("\"\\ud83d\""));				// half a pair
	CHECK(Fails("\"\\ude00\""));
	CHECK(Fails("\"\\ud83d\\u0041\""));		// the second half is wrong
}

TEST(Json, BrokenTextIsRefusedWithAReason)
{
	for (const char *text : { "", "{", "[1,2", "{\"a\":}", "{\"a\" 1}", "{a:1}", "[1,]", "{\"a\":1,}", "\"open", "tru", "nul", "1 2", "{} x", "--1", "\"\\x\"", "\"tab\there\"" })
	{
		CHECK(Fails(text));
	}
}

TEST(Json, NestingIsLimited)
{
	std::string deep(100, '[');
	deep += std::string(100, ']');
	CHECK(Fails(deep));

	std::string fine(20, '[');
	fine += std::string(20, ']');
	Parse(fine);
}

TEST(Json, SettingAMemberTurnsNullIntoAnObject)
{
	Json json;
	json["a"]["b"] = 1;
	CHECK_EQ(json.Dump(), "{\"a\":{\"b\":1}}");

	Json list;
	list.Push(1);
	CHECK(list.IsArray());
}
