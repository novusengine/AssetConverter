#pragma once
#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/ClientDB/Definitions.h>

#include <string>
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

public:
    static ClientDB::Storage<ClientDB::Definitions::Map> mapStorage;
    static ClientDB::Storage<ClientDB::Definitions::LiquidObject> liquidObjectStorage;
    static ClientDB::Storage<ClientDB::Definitions::LiquidType> liquidTypeStorage;
    static ClientDB::Storage<ClientDB::Definitions::LiquidMaterial> liquidMaterialStorage;
    static ClientDB::Storage<ClientDB::Definitions::CinematicCamera> cinematicCameraStorage;
    static ClientDB::Storage<ClientDB::Definitions::CinematicSequence> cinematicSequenceStorage;

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