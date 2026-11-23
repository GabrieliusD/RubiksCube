#include "SaveGame.h"
#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;

json SaveGame::ToJson() const
{
	json j;
	j["version"] = kVersion;
	j["seed"] = seed;
	j["days"] = days;
	j["personal"] = personal;
	j["era"] = era;
	j["feet"] = { feet.x, feet.y, feet.z };
	j["yaw"] = yaw;
	j["pitch"] = pitch;
	j["backpack"] = backpack;
	j["pickLevel"] = pickLevel;
	j["lanternLevel"] = lanternLevel;
	j["date"] = date;
	j["log"] = log.ToJson();
	return j;
}

bool SaveGame::FromJson(const json& j, std::string& error)
{
	try
	{
		if (!j.is_object() || j.value("version", 0) < 1)
		{
			error = "not an UNDERTIME save";
			return false;
		}
		if (j.value("version", 0) > kVersion)
		{
			error = "saved by a newer version of the game";
			return false;
		}
		seed = j.value("seed", 0u);
		days = j.value("days", 0.0);
		personal = j.value("personal", 0.0);
		era = j.value("era", 1);
		if (j.contains("feet") && j["feet"].is_array() && j["feet"].size() == 3) feet = { j["feet"][0].get<float>(), j["feet"][1].get<float>(), j["feet"][2].get<float>() };
		yaw = j.value("yaw", 0.0f);
		pitch = j.value("pitch", 0.0f);
		backpack.clear();
		if (j.contains("backpack")) backpack = j["backpack"].get<std::map<std::string, int>>();
		pickLevel = j.value("pickLevel", 0);
		lanternLevel = j.value("lanternLevel", 0);
		date = j.value("date", std::string());
		return !j.contains("log") || log.FromJson(j["log"], error);
	}
	catch (const std::exception& e)
	{
		error = e.what();
		return false;
	}
}

bool SaveGame::Write(const std::string& path, std::string& error) const
{
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	const std::string temporary = path + ".tmp";
	{
		std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
		if (!out)
		{
			error = "can't write " + temporary;
			return false;
		}
		out << ToJson().dump(1, '\t');
		if (!out)
		{
			error = "writing " + temporary + " failed";
			return false;
		}
	}
	fs::rename(temporary, path, ec);
	if (ec)
	{
		error = "can't replace " + path + ": " + ec.message();
		return false;
	}
	return true;
}

bool SaveGame::Read(const std::string& path, std::string& error)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		error = "no save at " + path;
		return false;
	}
	std::stringstream text;
	text << in.rdbuf();
	try
	{
		return FromJson(json::parse(text.str()), error);
	}
	catch (const std::exception& e)
	{
		error = std::string("the save is damaged: ") + e.what();
		return false;
	}
}
