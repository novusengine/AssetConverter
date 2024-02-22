local dependencies = { }
local defines = { }
ProjectTemplate("Generate", "StaticLib", ".", AssetConverter.binDir, dependencies, defines)

local solutionType = BuildSettings:Get("Solution Type")
postbuildcommands
{
    "cd " .. AssetConverter.rootDir,
    "premake5 " .. solutionType
}