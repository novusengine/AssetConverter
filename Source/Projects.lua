-- Projects
AssetConverter.projectsDir = path.getabsolute("Source/", AssetConverter.rootDir)

print("-- Creating Modules --")

if (AssetConverter.isRoot) then
    group "[Build System]"
    include("Generate/Generate.lua")
end

group (AssetConverter.name .. "/[Modules]")
local modules =
{
    "AssetConverter/AssetConverter.lua",
}

for k,v in pairs(modules) do
    filter { }
    include(v)
end

filter { }
group (AssetConverter.name)

print("-- Finished with Modules --\n")