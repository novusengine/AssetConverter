#pragma once
#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/ClientDB/Definitions.h>

#include <robinhood/robinhood.h>

#include <functional>
#include <vector>

class ClientDBExtractor
{
public:
    static void Process();

private:
    static bool ExtractMap();
    static bool ExtractLiquidObject();
    static bool ExtractLiquidType();
    static bool ExtractLiquidMaterial();
    static bool ExtractCinematicCamera();
    static bool ExtractCinematicSequence();
    static bool ExtractAnimationData();
    static bool ExtractCreatureDisplayInfo();
    static bool ExtractCreatureDisplayInfoExtra();
    static bool ExtractCreatureModelData();
    static bool ExtractTextureFileData();
    static bool ExtractCharSection();

public:
    static ClientDB::Storage<ClientDB::Definitions::Map> mapStorage;
    static ClientDB::Storage<ClientDB::Definitions::LiquidObject> liquidObjectStorage;
    static ClientDB::Storage<ClientDB::Definitions::LiquidType> liquidTypeStorage;
    static ClientDB::Storage<ClientDB::Definitions::LiquidMaterial> liquidMaterialStorage;
    static ClientDB::Storage<ClientDB::Definitions::CinematicCamera> cinematicCameraStorage;
    static ClientDB::Storage<ClientDB::Definitions::CinematicSequence> cinematicSequenceStorage;
    static ClientDB::Storage<ClientDB::Definitions::AnimationData> animationDataStorage;
    static ClientDB::Storage<ClientDB::Definitions::CreatureDisplayInfo> creatureDisplayInfoStorage;
    static ClientDB::Storage<ClientDB::Definitions::CreatureDisplayInfoExtra> creatureDisplayInfoExtraStorage;
    static ClientDB::Storage<ClientDB::Definitions::CreatureModelData> creatureModelDataStorage;
    static ClientDB::Storage<ClientDB::Definitions::TextureFileData> textureFileDataStorage;
    static ClientDB::Storage<ClientDB::Definitions::CharSection> charSectionStorage;

    static robin_hood::unordered_map<u32, u32> materialResourcesIDToTextureFileDataEntry;

private:
    struct ExtractionEntry
    {
    public:
        ExtractionEntry(std::string inName, std::string inDescription, std::function<bool()> inFunction) : name(inName), description(inDescription), function(inFunction) { }
        
        const std::string name;
        const std::string description;
        const std::function<bool()> function;
    };

    static std::vector<ExtractionEntry> _extractionEntries;
};