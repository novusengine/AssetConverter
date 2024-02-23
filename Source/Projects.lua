-- Engine Projects
AssetConverter.projectsDir = path.getabsolute("Source/", AssetConverter.rootDir)

print("-- Creating Modules --")

if (AssetConverter.isRoot) then
    group "[Build System]"
    include("Generate/Generate.lua")
end

group "AssetConverter/[Modules]"
include("AssetConverter/AssetConverter.lua")
group "AssetConverter"
print("-- Finished with Modules --\n")