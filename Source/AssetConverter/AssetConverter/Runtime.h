#pragma once
#include <TaskScheduler.h>
#include <json/json.hpp>

#include <filesystem>
namespace fs = std::filesystem;

struct Runtime
{
public:
	struct Paths
	{
		fs::path executable;
		fs::path data;
		fs::path clientDB;
		fs::path texture;
		fs::path textureBlendMap;
		fs::path map;
		fs::path mapObject;
		fs::path complexModel;
	};

public:
	bool isInDebugMode = false;
	Paths paths = { };
	enki::TaskScheduler scheduler;
	nlohmann::ordered_json json = { };
};