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

    static auto GetMapStorage()
    {
        if (!_mapStorage.IsInitialized())
            _mapStorage.Init(sizeof(ClientDB::Definitions::Map));

        return ClientDB::Storage<ClientDB::Definitions::Map>(&_mapStorage);
    }
    static auto GetLiquidObjectStorage()
    {
        if (!_liquidObjectStorage.IsInitialized())
            _liquidObjectStorage.Init(sizeof(ClientDB::Definitions::LiquidObject));

        return ClientDB::Storage<ClientDB::Definitions::LiquidObject>(&_liquidObjectStorage);
    }
    static auto GetLiquidTypeStorage()
    {
        if (!_liquidTypeStorage.IsInitialized())
            _liquidTypeStorage.Init(sizeof(ClientDB::Definitions::LiquidType));

        return ClientDB::Storage<ClientDB::Definitions::LiquidType>(&_liquidTypeStorage);
    }
    static auto GetLiquidMaterialStorage()
    {
        if (!_liquidMaterialStorage.IsInitialized())
            _liquidMaterialStorage.Init(sizeof(ClientDB::Definitions::LiquidMaterial));

        return ClientDB::Storage<ClientDB::Definitions::LiquidMaterial>(&_liquidMaterialStorage);
    }
    static auto GetCinematicCameraStorage()
    {
        if (!_cinematicCameraStorage.IsInitialized())
            _cinematicCameraStorage.Init(sizeof(ClientDB::Definitions::CinematicCamera));

        return ClientDB::Storage<ClientDB::Definitions::CinematicCamera>(&_cinematicCameraStorage);
    }

    static auto GetCinematicSequenceStorage()
    {
        if (!_cinematicSequenceStorage.IsInitialized())
            _cinematicSequenceStorage.Init(sizeof(ClientDB::Definitions::CinematicSequence));

        return ClientDB::Storage<ClientDB::Definitions::CinematicSequence>(&_cinematicSequenceStorage);
    }

private:
    static bool ExtractMap();
    static bool ExtractLiquidObject();
    static bool ExtractLiquidType();
    static bool ExtractLiquidMaterial();
    static bool ExtractCinematicCamera();
    static bool ExtractCinematicSequence();

private:
    static ClientDB::StorageRaw _mapStorage;
    static ClientDB::StorageRaw _liquidObjectStorage;
    static ClientDB::StorageRaw _liquidTypeStorage;
    static ClientDB::StorageRaw _liquidMaterialStorage;
    static ClientDB::StorageRaw _cinematicCameraStorage;
    static ClientDB::StorageRaw _cinematicSequenceStorage;

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