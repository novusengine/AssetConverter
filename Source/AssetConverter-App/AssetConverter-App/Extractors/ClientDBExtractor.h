#pragma once
#include <FileFormat/Novus/ClientDB/ClientDB.h>

#include <robinhood/robinhood.h>

#include <functional>
#include <vector>

class ClientDBExtractor
{
public:
    static void Process();

private:
    static bool ExtractModelFileData(const std::string& name);
    static bool ExtractTextureFileData(const std::string& name);
    static bool ExtractMap(const std::string& name);
    static bool ExtractLiquidObject(const std::string& name);
    static bool ExtractLiquidType(const std::string& name);
    static bool ExtractLiquidMaterial(const std::string& name);
    static bool ExtractCinematicCamera(const std::string& name);
    static bool ExtractCinematicSequence(const std::string& name);
    static bool ExtractAnimationData(const std::string& name);
    static bool ExtractCreatureDisplayInfo(const std::string& name);
    static bool ExtractCreatureDisplayInfoExtra(const std::string& name);
    static bool ExtractCreatureModelData(const std::string& name);
    static bool ExtractItemDisplayMaterialResources(const std::string& name);
    static bool ExtractItemDisplayModelMaterialResources(const std::string& name);
    static bool ExtractItemDisplayInfo(const std::string& name);
    static bool ExtractLight(const std::string& name);
    static bool ExtractLightParams(const std::string& name);
    static bool ExtractLightData(const std::string& name);
    static bool ExtractLightSkybox(const std::string& name);

public:
    static ClientDB::Data modelFileDataStorage;
    static ClientDB::Data textureFileDataStorage;
    static ClientDB::Data mapStorage;
    static ClientDB::Data liquidObjectStorage;
    static ClientDB::Data liquidTypeStorage;
    static ClientDB::Data liquidMaterialStorage;
    static ClientDB::Data cinematicCameraStorage;
    static ClientDB::Data cinematicSequenceStorage;
    static ClientDB::Data animationDataStorage;
    static ClientDB::Data creatureModelDataStorage;
    static ClientDB::Data creatureDisplayInfoStorage;
    static ClientDB::Data creatureDisplayInfoExtraStorage;
    static ClientDB::Data itemDisplayMaterialResourcesStorage;
    static ClientDB::Data itemDisplayModelMaterialResourcesStorage;
    static ClientDB::Data itemDisplayInfoStorage;
    static ClientDB::Data lightStorage;
    static ClientDB::Data lightParamsStorage;
    static ClientDB::Data lightDataStorage;
    static ClientDB::Data lightSkyboxStorage;

    static robin_hood::unordered_map<u32, std::vector<u32>> modelResourcesIDToModelFileDataEntry;
    static robin_hood::unordered_map<u32, std::vector<u32>> materialResourcesIDToTextureFileDataEntry;

private:
    struct ExtractionEntry
    {
    public:
        ExtractionEntry(std::string inName, std::string inDescription, std::function<bool(const std::string& name)> inFunction) : name(inName), description(inDescription), function(inFunction) { }
        
        const std::string name;
        const std::string description;
        const std::function<bool(const std::string& name)> function;
    };

    static std::vector<ExtractionEntry> _extractionEntries;
};