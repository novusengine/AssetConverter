AssetConverter = { }
AssetConverter.name = "AssetConverter"
AssetConverter.isRoot = false

AssetConverter.Init = function(self, rootDir, buildDir, binDir)
    self.rootDir = rootDir
    self.buildDir = buildDir
    self.binDir = binDir

    workspace "AssetConverter"
        location (self.buildDir)
        configurations { "Debug", "RelDebug", "Release" }

        filter "system:Windows"
            system "windows"
            platforms "Win64"

        filter "system:Unix"
            system "linux"
            platforms "Linux"

    local projectUtils = path.getabsolute("ProjectUtil.lua", rootDir)
    include(projectUtils)

    local buildSettings = path.getabsolute("BuildSettings.lua", self.rootDir)
    local silentFailOnDuplicateSetting = false
    InitBuildSettings(silentFailOnDuplicateSetting)
    include(buildSettings)

    local engineRootDir = path.getabsolute("Submodules/Engine/", rootDir)
    local engineBuildDir = path.getabsolute("Build/Engine/", rootDir)
    local enginePremakeFile = path.getabsolute("premake5.lua", engineRootDir)
    include(enginePremakeFile)

    Engine:Init(engineRootDir, engineBuildDir, binDir)

    print("-- Configuring (" .. self.name .. ") --\n")
    print("\n-- Directory Info (" .. self.name .. ") --")
    print(" Root Directory : " .. self.rootDir)
    print(" Build Directory : " .. self.buildDir)
    print(" Bin Directory : " .. self.binDir)
    print("--\n")

    local deps = path.getabsolute("Dependencies/Dependencies.lua", self.rootDir)
    local projects = path.getabsolute("Source/Projects.lua", self.rootDir)
    include(deps)
    include(projects)

    print("-- Done (" .. self.name .. ") --")
end

if HasRoot == nil then
    HasRoot = true

    local rootDir = path.getabsolute(".")
    local buildDir = path.getabsolute("Build/", rootDir)
    local binDir = path.getabsolute("Bin/", buildDir)

    AssetConverter.isRoot = true
    AssetConverter:Init(rootDir, buildDir, binDir)
end