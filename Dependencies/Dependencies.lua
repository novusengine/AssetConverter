-- Dependencies
AssetConverter.dependencyDir = path.getabsolute("Dependencies/", AssetConverter.rootDir)

print("-- Creating Dependencies --")

AssetConverter.dependencyGroup = (AssetConverter.name .. "/Dependencies")
group (AssetConverter.dependencyGroup)

local dependencies =
{
    "Casc/Casc.lua",
    "Cuttlefish/Cuttlefish.lua",
    "jolt/jolt.lua",
}

for k,v in pairs(dependencies) do
    filter { }
    include(v)
end

filter { }
group (AssetConverter.name)

print("-- Finished with Dependencies --\n")