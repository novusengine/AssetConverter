local dependencies = { "base", "fileformat", "enkiTS", "casc", "cuttlefish" }
local defines = { "_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS", "_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS", "WIN32_LEAN_AND_MEAN" }
ProjectTemplate("AssetConverter", "ConsoleApp", ".", AssetConverter.binDir, dependencies, defines)