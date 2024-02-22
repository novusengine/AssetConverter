-- Dependencies
AssetConverter.dependencyDir = path.getabsolute("Dependencies/", AssetConverter.rootDir)

print("-- Creating Dependencies --")
AssetConverter.dependencyGroup = "AssetConverter/Dependencies"
group (AssetConverter.dependencyGroup)
include("Casc/Casc.lua")
include("Cuttlefish/Cuttlefish.lua")
group "AssetConverter"
print("-- Finished with Dependencies --\n")