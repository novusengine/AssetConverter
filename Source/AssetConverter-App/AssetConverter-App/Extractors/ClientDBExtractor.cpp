#include "ClientDBExtractor.h"
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/ServiceLocator.h"

#include <Base/Container/StringTable.h>
#include <Base/Memory/FileWriter.h>
#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <FileFormat/Shared.h>
#include <FileFormat/Warcraft/DB2/DB2Definitions.h>
#include <FileFormat/Warcraft/DB2/Wdc3.h>
#include <FileFormat/Warcraft/Parsers/Wdc3Parser.h>
#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/ClientDB/Definitions.h>
#include <FileFormat/Novus/Model/ComplexModel.h>

#include <filesystem>
namespace fs = std::filesystem;

using namespace ClientDB;

std::vector<ClientDBExtractor::ExtractionEntry> ClientDBExtractor::_extractionEntries =
{
    { "ModelFileData",                      "A collection of Model File Data",                  ClientDBExtractor::ExtractModelFileData },
    { "TextureFileData",                    "A collection of Texture File Data",                ClientDBExtractor::ExtractTextureFileData },
    { "Map",				                "A collection of all maps",				            ClientDBExtractor::ExtractMap },
    { "LiquidObject",		                "A collection of liquid objects",		            ClientDBExtractor::ExtractLiquidObject },
    { "LiquidType",			                "A collection of liquid types",			            ClientDBExtractor::ExtractLiquidType },
    { "LiquidMaterial",		                "A collection of liquid materials",		            ClientDBExtractor::ExtractLiquidMaterial },
    { "CinematicCamera",	                "A collection of cinematic cameras",	            ClientDBExtractor::ExtractCinematicCamera },
    { "CinematicSequences",	                "A collection of cinematic sequences",	            ClientDBExtractor::ExtractCinematicSequence },
    { "AnimationData",	                    "A collection of Animation Data",	                ClientDBExtractor::ExtractAnimationData },
    { "CreatureModelData",                  "A collection of Creature Model Data",              ClientDBExtractor::ExtractCreatureModelData },
    { "CreatureDisplayInfo",                "A collection of Creature Display Info Data",	    ClientDBExtractor::ExtractCreatureDisplayInfo },
    { "CreatureDisplayInfoExtra",           "A collection of Creature Display Info Extra Data", ClientDBExtractor::ExtractCreatureDisplayInfoExtra },
    { "ItemDisplayInfoMaterialRes",         "A collection of Item Display Material Data",       ClientDBExtractor::ExtractItemDisplayMaterialResources },
    { "ItemDisplayInfoModelMatRes",         "A collection of Item Display Material Data",       ClientDBExtractor::ExtractItemDisplayModelMaterialResources },
    { "ItemDisplayInfo",                    "A collection of Item Display Data",                ClientDBExtractor::ExtractItemDisplayInfo },
    { "Light",                              "A collection of Light Data",                       ClientDBExtractor::ExtractLight },
    { "LightParams",                        "A collection of Light Parameter Data",             ClientDBExtractor::ExtractLightParams },
    { "LightData",                          "A collection of Light Data Data",                  ClientDBExtractor::ExtractLightData },
    { "LightSkybox",                        "A collection of Light Skybox Data",                ClientDBExtractor::ExtractLightSkybox }
};

ClientDB::Data ClientDBExtractor::modelFileDataStorage;
ClientDB::Data ClientDBExtractor::textureFileDataStorage;
ClientDB::Data ClientDBExtractor::mapStorage;
ClientDB::Data ClientDBExtractor::liquidObjectStorage;
ClientDB::Data ClientDBExtractor::liquidTypeStorage;
ClientDB::Data ClientDBExtractor::liquidMaterialStorage;
ClientDB::Data ClientDBExtractor::cinematicCameraStorage;
ClientDB::Data ClientDBExtractor::cinematicSequenceStorage;
ClientDB::Data ClientDBExtractor::animationDataStorage;
ClientDB::Data ClientDBExtractor::creatureModelDataStorage;
ClientDB::Data ClientDBExtractor::creatureDisplayInfoStorage;
ClientDB::Data ClientDBExtractor::creatureDisplayInfoExtraStorage;
ClientDB::Data ClientDBExtractor::itemDisplayMaterialResourcesStorage;
ClientDB::Data ClientDBExtractor::itemDisplayModelMaterialResourcesStorage;
ClientDB::Data ClientDBExtractor::itemDisplayInfoStorage;
ClientDB::Data ClientDBExtractor::lightStorage;
ClientDB::Data ClientDBExtractor::lightParamsStorage;
ClientDB::Data ClientDBExtractor::lightDataStorage;
ClientDB::Data ClientDBExtractor::lightSkyboxStorage;

robin_hood::unordered_map<u32, std::vector<u32>> ClientDBExtractor::modelResourcesIDToModelFileDataEntry;
robin_hood::unordered_map<u32, std::vector<u32>> ClientDBExtractor::materialResourcesIDToTextureFileDataEntry;

void ClientDBExtractor::Process()
{
    for (u32 i = 0; i < _extractionEntries.size(); i++)
    {
        const ExtractionEntry& entry = _extractionEntries[i];
    
        if (entry.function(entry.name))
        {
            NC_LOG_INFO("[ClientDBExtractor] Extracted (\"{0}\" : \"{1}\")", entry.name, entry.description);
        }
        else
        {
            NC_LOG_WARNING("[ClientDBExtractor] Failed to extract (\"{0}\" : \"{1}\")", entry.name, entry.description);
        }
    }
}

void FixPathExtension(std::string& path)
{
    if (path.length() == 0)
        return;

    if (StringUtils::EndsWith(path, ".mdx"))
    {
        path = path.substr(0, path.length() - 4) + Model::FILE_EXTENSION;
    }
    else if (StringUtils::EndsWith(path, ".m2"))
    {
        path = path.substr(0, path.length() - 3) + Model::FILE_EXTENSION;
    }
    else if (StringUtils::EndsWith(path, ".blp"))
    {
        path = path.substr(0, path.length() - 4) + ".dds";
        std::transform(path.begin(), path.end(), path.begin(), ::tolower);
    }
}

std::string GetStringFromRecordIndex(DB2::WDC3::Layout& layout, DB2::WDC3::Parser& db2Parser, u32 recordIndex, u32 fieldIndex)
{
    std::string value = db2Parser.GetString(layout, recordIndex, fieldIndex);

    FixPathExtension(value);

    std::replace(value.begin(), value.end(), '\\', '/');

    return value;
}
std::string GetStringFromArrRecordIndex(DB2::WDC3::Layout& layout, DB2::WDC3::Parser& db2Parser, u32 recordIndex, u32 fieldIndex, u32 arrIndex)
{
    std::string value = db2Parser.GetStringInArr(layout, recordIndex, fieldIndex, arrIndex);

    FixPathExtension(value);

    std::replace(value.begin(), value.end(), '\\', '/');

    return value;
}

template <typename T>
void RepopulateFromCopyTable(const DB2::WDC3::Layout& db2, ClientDB::Data& storage)
{
    u32 numSections = static_cast<u32>(db2.sections.size());

    for (u32 i = 0; i < numSections; i++)
    {
        const DB2::WDC3::Layout::SectionHeader& sectionHeader = db2.sectionHeaders[i];
        const DB2::WDC3::Layout::Section& section = db2.sections[i];

        if (sectionHeader.copyTableCount == 0)
            continue;

        u32 numNewElements = sectionHeader.copyTableCount;
        storage.Reserve(numNewElements);

        for (u32 j = 0; j < sectionHeader.copyTableCount; j++)
        {
            const DB2::WDC3::Layout::CopyTableEntry& copyTableEntry = section.copyTable[j];

            bool didOverride = false;
            storage.Clone<T>(copyTableEntry.oldRowID, copyTableEntry.newRowID, didOverride);
        }
    }
}

std::string GetFilePathForDB2ByName(const std::string& name)
{
    std::string result = "dbfilesclient/" + name + ".db2";
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);

    return result;
}

bool ClientDBExtractor::ExtractModelFileData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    modelFileDataStorage.Initialize({
        { "Flags",              FieldType::I8   },
        { "ModelPath",          FieldType::StringRef  },
        { "ModelResourcesID",   FieldType::I32  },
    });
    modelFileDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::ModelFileData modelFileData;
        u32 modelFileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        modelFileData.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        modelFileData.modelResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);

        if (cascLoader->InCascAndListFile(modelFileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(modelFileID);

            fs::path filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
            modelFileData.modelPath = modelFileDataStorage.AddString(filePath.string());
        }
        else
        {
            modelFileData.modelPath = 0;
        }

        auto& modelFileDataEntries = modelResourcesIDToModelFileDataEntry[modelFileData.modelResourcesID];
        modelFileDataEntries.push_back(modelFileID);

        bool didOverride = false;
        modelFileDataStorage.Replace(db2RecordIndex + 1, modelFileData, didOverride);
    }

    RepopulateFromCopyTable<Definitions::ModelFileData>(layout, modelFileDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!modelFileDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractTextureFileData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    textureFileDataStorage.Initialize({
        { "TextureHash",            FieldType::I32  },
        { "MaterialResourcesID",    FieldType::I32  },
    });
    textureFileDataStorage.Reserve(header.recordCount);
    materialResourcesIDToTextureFileDataEntry.reserve(header.recordCount * 2);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::TextureFileData textureFileData;
        u32 id = db2RecordIndex + 1;

        u32 textureFileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        textureFileData.materialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        if (cascLoader->InCascAndListFile(textureFileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(textureFileID);

            fs::path filePath = fs::path(fileStr).replace_extension("dds");
            u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

            textureFileData.textureHash = nameHash;
        }
        else
        {
            textureFileData.textureHash = std::numeric_limits<u32>::max();
        }

        auto& textureFileDataIDs = materialResourcesIDToTextureFileDataEntry[textureFileData.materialResourcesID];
        textureFileDataIDs.push_back(id);

        bool didOverride = false;
        textureFileDataStorage.Replace(id, textureFileData, didOverride);
    }

    RepopulateFromCopyTable<Definitions::TextureFileData>(layout, textureFileDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!textureFileDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractMap(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;

    mapStorage.Initialize( {
        { "Name",           ClientDB::FieldType::StringRef  },
        { "InternalName",   ClientDB::FieldType::StringRef  },
        { "InstanceType",   ClientDB::FieldType::I32        },
        { "Flags",          ClientDB::FieldType::I32        },
        { "Expansion",      ClientDB::FieldType::I32        },
        { "MaxPlayers",     ClientDB::FieldType::I32        },
    });

    mapStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        std::string internalName = db2Parser.GetString(layout, db2RecordIndex, 0);
        std::string cascPath = "world/maps/" + internalName + "/" + internalName + ".wdt";
        std::transform(cascPath.begin(), cascPath.end(), cascPath.begin(), ::tolower);

        u32 fileID = cascLoader->GetFileIDFromListFilePath(cascPath);

        bool hasWDTFile = fileID > 0 && cascLoader->InCascAndListFile(fileID);
        if (hasWDTFile)
        {
            Definitions::Map map = { };

            std::string internalName = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0);;
            std::string mapName = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 1);

            map.name = mapStorage.AddString(mapName);
            map.internalName = mapStorage.AddString(internalName);

            const u8 instanceType = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 7);
            map.instanceType = instanceType;

            const u32* flags = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 22);
            map.flags = flags[0];

            const u8 expansion = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 8);
            map.expansion = expansion;

            const u8 maxPlayers = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 18);
            map.maxPlayers = maxPlayers;

            bool didOverride = false;
            mapStorage.Replace(recordID, map, didOverride);
        }
    }

    RepopulateFromCopyTable<Definitions::Map>(layout, mapStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!mapStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractLiquidObject(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    liquidObjectStorage.Initialize( {
        { "LiquidTypeID",   ClientDB::FieldType::I16    },
        { "Fishable",       ClientDB::FieldType::I8     },
        { "Reflection",     ClientDB::FieldType::I8     },
        { "FlowDirection",  ClientDB::FieldType::F32    },
        { "FlowSpeed",      ClientDB::FieldType::F32    }
    });

    liquidObjectStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LiquidObject liquidObject;
        liquidObject.flowDirection = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 0);
        liquidObject.flowSpeed = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 1);
        liquidObject.liquidTypeID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        liquidObject.fishable = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);
        liquidObject.reflection = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 4);

        bool didOverride = false;
        liquidObjectStorage.Replace(recordID, liquidObject, didOverride);
    }

    RepopulateFromCopyTable<Definitions::LiquidObject>(layout, liquidObjectStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidObjectStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLiquidType(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    liquidTypeStorage.Initialize( {
        { "Name",                   ClientDB::FieldType::StringRef  },
        { "Textures1",              ClientDB::FieldType::StringRef  },
        { "Textures2",              ClientDB::FieldType::StringRef  },
        { "Textures3",              ClientDB::FieldType::StringRef  },
        { "Textures4",              ClientDB::FieldType::StringRef  },
        { "Textures5",              ClientDB::FieldType::StringRef  },
        { "Textures6",              ClientDB::FieldType::StringRef  },
        { "Flags",                  ClientDB::FieldType::I16        },
        { "LightID",                ClientDB::FieldType::I16        },
        { "SoundID",                ClientDB::FieldType::I32        },
        { "SpellID",                ClientDB::FieldType::I32        },
        { "MaxDarkenDepth",         ClientDB::FieldType::F32        },
        { "FogDarkenIntensity",     ClientDB::FieldType::F32        },
        { "AmbDarkenIntensity",     ClientDB::FieldType::F32        },
        { "DirDarkenIntensity",     ClientDB::FieldType::F32        },
        { "ParticleScale",          ClientDB::FieldType::F32        },
        { "materialID",             ClientDB::FieldType::I8         },
        { "SoundBank",              ClientDB::FieldType::I8         },
        { "ParticleMovement",       ClientDB::FieldType::I8         },
        { "ParticleTexSlots",       ClientDB::FieldType::I8         },
        { "MinimapStaticCol",       ClientDB::FieldType::I32        },
        { "FrameCountTextures1",    ClientDB::FieldType::I32        },
        { "FrameCountTextures2",    ClientDB::FieldType::I32        },
        { "FrameCountTextures3",    ClientDB::FieldType::I32        },
        { "FrameCountTextures4",    ClientDB::FieldType::I32        },
        { "FrameCountTextures5",    ClientDB::FieldType::I32        },
        { "FrameCountTextures6",    ClientDB::FieldType::I32        },
        { "Colors1",                ClientDB::FieldType::I32        },
        { "Colors2",                ClientDB::FieldType::I32        },
        { "UnkFloats1",             ClientDB::FieldType::F32        },
        { "UnkFloats2",             ClientDB::FieldType::F32        },
        { "UnkFloats3",             ClientDB::FieldType::F32        },
        { "UnkFloats4",             ClientDB::FieldType::F32        },
        { "UnkFloats5",             ClientDB::FieldType::F32        },
        { "UnkFloats6",             ClientDB::FieldType::F32        },
        { "UnkFloats7",             ClientDB::FieldType::F32        },
        { "UnkFloats8",             ClientDB::FieldType::F32        },
        { "UnkFloats9",             ClientDB::FieldType::F32        },
        { "UnkFloats10",            ClientDB::FieldType::F32        },
        { "UnkFloats11",            ClientDB::FieldType::F32        },
        { "UnkFloats12",            ClientDB::FieldType::F32        },
        { "UnkFloats13",            ClientDB::FieldType::F32        },
        { "UnkFloats14",            ClientDB::FieldType::F32        },
        { "UnkFloats15",            ClientDB::FieldType::F32        },
        { "UnkFloats16",            ClientDB::FieldType::F32        },
        { "UnkInts1",               ClientDB::FieldType::I32        },
        { "UnkInts2",               ClientDB::FieldType::I32        },
        { "UnkInts3",               ClientDB::FieldType::I32        },
        { "UnkInts4",               ClientDB::FieldType::I32        },
        { "Coefficients1",          ClientDB::FieldType::I32        },
        { "Coefficients2",          ClientDB::FieldType::I32        },
        { "Coefficients3",          ClientDB::FieldType::I32        },
        { "Coefficients4",          ClientDB::FieldType::I32        },
    });

    liquidTypeStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LiquidType liquidType;
        liquidType.name = liquidTypeStorage.AddString(GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0));

        for (u32 i = 0; i < 6; i++)
            liquidType.textures[i] = liquidTypeStorage.AddString(GetStringFromArrRecordIndex(layout, db2Parser, db2RecordIndex, 1, i));

        liquidType.flags = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        liquidType.soundBank = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 3);
        liquidType.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        liquidType.spellID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 5);
        liquidType.maxDarkenDepth = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
        liquidType.fogDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        liquidType.ambDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 8);
        liquidType.dirDarkenIntensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 9);
        liquidType.lightID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 10);
        liquidType.particleScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 11);
        liquidType.particleMovement = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 12);
        liquidType.particleTexSlots = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 13);
        liquidType.materialID = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 14);
        liquidType.minimapStaticCol = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 15);

        const u8* frameCountTextures = db2Parser.GetFieldPtr<u8>(layout, sectionID, recordID, recordData, 16);
        for (u32 i = 0; i < 6; i++)
            liquidType.frameCountTextures[i] = frameCountTextures[i];

        const u32* colors = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 17);
        for (u32 i = 0; i < 2; i++)
            liquidType.colors[i] = colors[i];

        const f32* unkFloats = db2Parser.GetFieldPtr<f32>(layout, sectionID, recordID, recordData, 18);
        for (u32 i = 0; i < 16; i++)
            liquidType.unkFloats[i] = unkFloats[i];

        const u32* unkInts = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 19);
        for (u32 i = 0; i < 4; i++)
            liquidType.unkInts[i] = unkInts[i];

        const u32* coefficients = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 20);
        for (u32 i = 0; i < 4; i++)
            liquidType.coefficients[i] = coefficients[i];

        bool didOverride = false;
        liquidTypeStorage.Replace(recordID, liquidType, didOverride);
    }

    RepopulateFromCopyTable<Definitions::LiquidType>(layout, liquidTypeStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidTypeStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLiquidMaterial(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    liquidMaterialStorage.Initialize( {
        { "Flags",              ClientDB::FieldType::I8  },
        { "LiquidVertexFormat", ClientDB::FieldType::I8  }
    });
    liquidMaterialStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LiquidMaterial liquidMaterial;
        liquidMaterial.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 0);
        liquidMaterial.liquidVertexFormat = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);

        bool didOverride = false;
        liquidMaterialStorage.Replace(recordID, liquidMaterial, didOverride);
    }

    RepopulateFromCopyTable<Definitions::LiquidMaterial>(layout, liquidMaterialStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!liquidMaterialStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractCinematicCamera(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    cinematicCameraStorage.Initialize( {
        { "EndPositionX",   ClientDB::FieldType::F32  },
        { "EndPositionY",   ClientDB::FieldType::F32  },
        { "EndPositionZ",   ClientDB::FieldType::F32  },
        { "SoundID",        ClientDB::FieldType::I32  },
        { "Rotation",       ClientDB::FieldType::F32  },
        { "CameraPath",     ClientDB::FieldType::I32  }
    });
    cinematicCameraStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CinematicCamera cinematicCamera;
        const f32* endPosition = db2Parser.GetFieldPtr<f32>(layout, sectionID, recordID, recordData, 0);
        cinematicCamera.endPosition = CoordinateSpaces::CinematicCameraPosToNovus(vec3(endPosition[0], endPosition[1], endPosition[2]));
        cinematicCamera.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        cinematicCamera.rotation = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);
        cinematicCamera.cameraPath = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);

        u32 fileID = cinematicCamera.cameraPath;
        if (cascLoader->InCascAndListFile(fileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(cinematicCamera.cameraPath);

            fs::path filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
            u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

            cinematicCamera.cameraPath = nameHash;
        }
        else
        {
            cinematicCamera.cameraPath = std::numeric_limits<u32>::max();
        }

        bool didOverride = false;
        cinematicCameraStorage.Replace(recordID, cinematicCamera, didOverride);
    }

    RepopulateFromCopyTable<Definitions::CinematicCamera>(layout, cinematicCameraStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!cinematicCameraStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractCinematicSequence(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    cinematicSequenceStorage.Initialize( {
        { "SoundID",    ClientDB::FieldType::I32    },
        { "CameraID1",  ClientDB::FieldType::I16    },
        { "CameraID2",  ClientDB::FieldType::I16    },
        { "CameraID3",  ClientDB::FieldType::I16    },
        { "CameraID4",  ClientDB::FieldType::I16    },
        { "CameraID5",  ClientDB::FieldType::I16    },
        { "CameraID6",  ClientDB::FieldType::I16    },
        { "CameraID7",  ClientDB::FieldType::I16    },
        { "CameraID8",  ClientDB::FieldType::I16    },
    });
    cinematicSequenceStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CinematicSequence cinematicSequence;
        cinematicSequence.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);

        const u16* cameraIDs = db2Parser.GetFieldPtr<u16>(layout, sectionID, recordID, recordData, 1);
        memcpy(&cinematicSequence.cameraIDs[0], cameraIDs, 8 * sizeof(u16));

        bool didOverride = false;
        cinematicSequenceStorage.Replace(recordID, cinematicSequence, didOverride);
    }

    RepopulateFromCopyTable<Definitions::CinematicSequence>(layout, cinematicSequenceStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!cinematicSequenceStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractAnimationData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    animationDataStorage.Initialize( {
        { "Fallback",       ClientDB::FieldType::I16    },
        { "BehaviorTier",   ClientDB::FieldType::I8     },
        { "BehaviorID",     ClientDB::FieldType::I32    },
        { "Flags1",         ClientDB::FieldType::I32    },
        { "Flags2",         ClientDB::FieldType::I32    }
    });
    animationDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::AnimationData animationData;
        animationData.fallback = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 0);
        animationData.behaviorTier = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 1);
        animationData.behaviorID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);

        const u32* flags = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 3);
        memcpy(&animationData.flags[0], flags, 2 * sizeof(u32));

        bool didOverride = false;
        animationDataStorage.Replace(recordID, animationData, didOverride);
    }

    RepopulateFromCopyTable<Definitions::AnimationData>(layout, animationDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!animationDataStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractCreatureModelData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    creatureModelDataStorage.Initialize( {
        { "BoundBoxMinX",               ClientDB::FieldType::F32    },
        { "BoundBoxMinY",               ClientDB::FieldType::F32    },
        { "BoundBoxMinZ",               ClientDB::FieldType::F32    },
        { "BoundBoxMaxX",               ClientDB::FieldType::F32    },
        { "BoundBoxMaxY",               ClientDB::FieldType::F32    },
        { "BoundBoxMaxZ",               ClientDB::FieldType::F32    },
        { "Flags",                      ClientDB::FieldType::I32    },
        { "ModelHash",                  ClientDB::FieldType::I32    },
        { "BloodID",                    ClientDB::FieldType::I32    },
        { "FootprintTextureID",         ClientDB::FieldType::I32    },
        { "FootprintTextureLength",     ClientDB::FieldType::F32    },
        { "FootprintTextureWidth",      ClientDB::FieldType::F32    },
        { "FootprintParticleScale",     ClientDB::FieldType::F32    },
        { "FoleyMaterialID",            ClientDB::FieldType::I32    },
        { "FootstepCameraEffectID",     ClientDB::FieldType::I32    },
        { "DeathThudCameraEffectID",    ClientDB::FieldType::I32    },
        { "SoundID",                    ClientDB::FieldType::I32    },
        { "SizeClass",                  ClientDB::FieldType::I32    },
        { "CollisionWidth",             ClientDB::FieldType::F32    },
        { "CollisionHeight",            ClientDB::FieldType::F32    },
        { "WorldEffectScale",           ClientDB::FieldType::F32    },
        { "CreatureGeosetDataID",       ClientDB::FieldType::I32    },
        { "HoverHeight",                ClientDB::FieldType::F32    },
        { "AttachedEffectScale",        ClientDB::FieldType::F32    },
        { "ModelScale",                 ClientDB::FieldType::F32    },
        { "MissileCollisionRadius",     ClientDB::FieldType::F32    },
        { "MissileCollisionPush",       ClientDB::FieldType::F32    },
        { "MissileCollisionRaise",      ClientDB::FieldType::F32    },
        { "MountHeight",                ClientDB::FieldType::F32    },
        { "OverrideLootEffectScale",    ClientDB::FieldType::F32    },
        { "OverrideNameScale",          ClientDB::FieldType::F32    },
        { "OverrideSelectionRadius",    ClientDB::FieldType::F32    },
        { "TamedPetBaseScale",          ClientDB::FieldType::F32    },
    });
    creatureModelDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CreatureModelData creatureModelData;
        creatureModelData.boundingBox = *db2Parser.GetFieldPtr<Geometry::AABoundingBox>(layout, sectionID, recordID, recordData, 0);
        creatureModelData.flags = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        creatureModelData.modelHash = std::numeric_limits<u32>::max();
        u32 fileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        creatureModelData.bloodID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        creatureModelData.footprintTextureID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        creatureModelData.footprintTextureLength = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 5);
        creatureModelData.footprintTextureWidth = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
        creatureModelData.footprintParticleScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        creatureModelData.foleyMaterialID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 8);
        creatureModelData.footstepCameraEffectID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 9);
        creatureModelData.deathThudCameraEffectID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);
        creatureModelData.soundID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 11);
        creatureModelData.sizeClass = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 12);
        creatureModelData.collisionWidth = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 13);
        creatureModelData.collisionHeight = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 14);
        creatureModelData.worldEffectScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 15);
        creatureModelData.creatureGeosetDataID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 16);
        creatureModelData.hoverHeight = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 17);
        creatureModelData.attachedEffectScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 18);
        creatureModelData.modelScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 19);
        creatureModelData.missileCollisionRadius = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 20);
        creatureModelData.missileCollisionPush = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 21);
        creatureModelData.missileCollisionRaise = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 22);
        creatureModelData.mountHeight = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 23);
        creatureModelData.overrideLootEffectScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 24);
        creatureModelData.overrideNameScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 25);
        creatureModelData.overrideSelectionRadius = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 26);
        creatureModelData.tamedPetBaseScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 27);

        if (cascLoader->InCascAndListFile(fileID))
        {
            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(fileID);

            fs::path filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
            u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

            creatureModelData.modelHash = nameHash;
        }

        bool didOverride = false;
        creatureModelDataStorage.Replace(recordID, creatureModelData, didOverride);
    }

    RepopulateFromCopyTable<Definitions::CreatureModelData>(layout, creatureModelDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureModelDataStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractCreatureDisplayInfo(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    creatureDisplayInfoStorage.Initialize( {
        { "ModelID",                        ClientDB::FieldType::I16    },
        { "SoundID",                        ClientDB::FieldType::I16    },
        { "Flags",                          ClientDB::FieldType::I8     },
        { "Gender",                         ClientDB::FieldType::I8     },
        { "SizeClass",                      ClientDB::FieldType::I8     },
        { "BloodID",                        ClientDB::FieldType::I8     },
        { "UnarmedWeaponType",              ClientDB::FieldType::I8     },
        { "CreatureModelAlpha",             ClientDB::FieldType::I8     },
        { "CreatureModelScale",             ClientDB::FieldType::F32    },
        { "ExtendedDisplayInfoID",          ClientDB::FieldType::I32    },
        { "NpcSoundID",                     ClientDB::FieldType::I16    },
        { "ParticleColorID",                ClientDB::FieldType::I16    },
        { "PortraitCreatureDisplayInfoID",  ClientDB::FieldType::I32    },
        { "PortraitTextureFileDataID",      ClientDB::FieldType::I32    },
        { "ObjectEffectPackageID",          ClientDB::FieldType::I16    },
        { "AnimReplacementSetID",           ClientDB::FieldType::I16    },
        { "StateSpellVisualKitID",          ClientDB::FieldType::I32    },
        { "PlayerOverrideScale",            ClientDB::FieldType::F32    },
        { "PetInstanceScale",               ClientDB::FieldType::F32    },
        { "MountPoofSpellVisualKitID",      ClientDB::FieldType::I32    },
        { "DissolveEffectID",               ClientDB::FieldType::I32    },
        { "DissolveOutEffectID",            ClientDB::FieldType::I32    },
        { "TextureVariations1",             ClientDB::FieldType::I32    },
        { "TextureVariations2",             ClientDB::FieldType::I32    },
        { "TextureVariations3",             ClientDB::FieldType::I32    },
        { "TextureVariations4",             ClientDB::FieldType::I32    },
    });
    creatureDisplayInfoStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CreatureDisplayInfo creatureDisplayInfo;
        recordID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        creatureDisplayInfo.modelID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 1);
        creatureDisplayInfo.soundID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 2);
        creatureDisplayInfo.sizeClass = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 3);
        creatureDisplayInfo.creatureModelScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 4);
        creatureDisplayInfo.creatureModelAlpha = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 5);
        creatureDisplayInfo.bloodID = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 6);
        creatureDisplayInfo.extendedDisplayInfoID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 7);
        creatureDisplayInfo.npcSoundID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 8);
        creatureDisplayInfo.particleColorID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 9);
        creatureDisplayInfo.portraitCreatureDisplayInfoID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 10);
        creatureDisplayInfo.portraitTextureFileDataID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 11);
        creatureDisplayInfo.objectEffectPackageID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 12);
        creatureDisplayInfo.animReplacementSetID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 13);
        creatureDisplayInfo.flags = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 14);
        creatureDisplayInfo.stateSpellVisualKitID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 15);
        creatureDisplayInfo.playerOverrideScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 16);
        creatureDisplayInfo.petInstanceScale = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 17);
        creatureDisplayInfo.unarmedWeaponType = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 18);
        creatureDisplayInfo.mountPoofSpellVisualKitID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 19);
        creatureDisplayInfo.dissolveEffectID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 20);
        creatureDisplayInfo.gender = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 21);
        creatureDisplayInfo.dissolveOutEffectID = db2Parser.GetField<i32>(layout, sectionID, recordID, recordData, 22);

        const u32* textureVariations = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 25);
        memcpy(&creatureDisplayInfo.textureVariations[0], textureVariations, 4 * sizeof(u32));

        for (i32 i = 0; i < 4; i++)
        {
            i32 textureVariationFileID = creatureDisplayInfo.textureVariations[i];

            if (textureVariationFileID == 0)
                continue;

            if (!cascLoader->InCascAndListFile(textureVariationFileID))
                continue;

            const std::string& fileStr = cascLoader->GetFilePathFromListFileID(textureVariationFileID);

            fs::path filePath = fs::path(fileStr).replace_extension("dds");
            u32 nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());

            creatureDisplayInfo.textureVariations[i] = nameHash;
        }

        bool didOverride = false;
        creatureDisplayInfoStorage.Replace(recordID, creatureDisplayInfo, didOverride);
    }

    RepopulateFromCopyTable<Definitions::CreatureDisplayInfo>(layout, creatureDisplayInfoStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureDisplayInfoStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractCreatureDisplayInfoExtra(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    creatureDisplayInfoExtraStorage.Initialize( {
        { "Flags",                  ClientDB::FieldType::I8     },
        { "DisplayRaceID",          ClientDB::FieldType::I8     },
        { "DisplaySexID",           ClientDB::FieldType::I8     },
        { "DisplayClassID",         ClientDB::FieldType::I8     },
        { "SkinID",                 ClientDB::FieldType::I8     },
        { "FaceID",                 ClientDB::FieldType::I8     },
        { "HairStyleID",            ClientDB::FieldType::I8     },
        { "HairColorID",            ClientDB::FieldType::I8     },
        { "FacialHairID",           ClientDB::FieldType::I8     },
        { "CustomDisplayOptions1",  ClientDB::FieldType::I8     },
        { "CustomDisplayOptions2",  ClientDB::FieldType::I8     },
        { "CustomDisplayOptions3",  ClientDB::FieldType::I8     },
        { "BakedTextureHash",       ClientDB::FieldType::I32    },
    });
    creatureDisplayInfoExtraStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::CreatureDisplayInfoExtra creatureDisplayInfoExtra;
        recordID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);
        creatureDisplayInfoExtra.displayRaceID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 1);
        creatureDisplayInfoExtra.displaySexID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 2) + 1;
        creatureDisplayInfoExtra.displayClassID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 3);
        creatureDisplayInfoExtra.skinID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 4);
        creatureDisplayInfoExtra.faceID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 5);
        creatureDisplayInfoExtra.hairStyleID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 6);
        creatureDisplayInfoExtra.hairColorID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 7);
        creatureDisplayInfoExtra.facialHairID = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 8);
        creatureDisplayInfoExtra.flags = db2Parser.GetField<i8>(layout, sectionID, recordID, recordData, 9);
        creatureDisplayInfoExtra.bakedTextureHash = std::numeric_limits<u32>::max();
        u32 bakedMaterialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);

        const u8* customDisplayOptions = db2Parser.GetFieldPtr<u8>(layout, sectionID, recordID, recordData, 12);
        memcpy(&creatureDisplayInfoExtra.customDisplayOptions[0], customDisplayOptions, 3 * sizeof(u8));

        if (materialResourcesIDToTextureFileDataEntry.contains(bakedMaterialResourcesID))
        {
            u32 textureFileDataID = materialResourcesIDToTextureFileDataEntry[bakedMaterialResourcesID][0];
            
            auto& textureFileData = textureFileDataStorage.Get<ClientDB::Definitions::TextureFileData>(textureFileDataID);
            creatureDisplayInfoExtra.bakedTextureHash = textureFileData.textureHash;
        }

        bool didOverride = false;
        creatureDisplayInfoExtraStorage.Replace(recordID, creatureDisplayInfoExtra, didOverride);
    }

    RepopulateFromCopyTable<Definitions::CreatureDisplayInfoExtra>(layout, creatureDisplayInfoExtraStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!creatureDisplayInfoExtraStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractItemDisplayMaterialResources(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    itemDisplayMaterialResourcesStorage.Initialize( {
        { "DisplayID",              ClientDB::FieldType::I32 },
        { "ComponentSection",       ClientDB::FieldType::I8 },
        { "MaterialResourcesID",    ClientDB::FieldType::I32 }
    });
    itemDisplayMaterialResourcesStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::ItemDisplayMaterialResources itemDisplayMaterialResource;
        itemDisplayMaterialResource.displayID = 0;
        u8 componentSection = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 0);
        itemDisplayMaterialResource.materialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);

        switch (componentSection)
        {
            case 0: // ArmUpper
            {
                componentSection = 5;
                break;
            }
            case 1: // ArmLower
            {
                componentSection = 6;
                break;
            }
            case 2: // Hand
            {
                componentSection = 7;
                break;
            }
            case 3: // TorsoUpper
            {
                componentSection = 3;
                break;
            }
            case 4: // TorsoLower
            {
                componentSection = 4;
                break;
            }
            case 5: // LegUpper
            {
                componentSection = 8;
                break;
            }
            case 6: // LegLower
            {
                componentSection = 9;
                break;
            }
            case 7: // Foot
            {
                componentSection = 10;
                break;
            }
            case 9: // ScalpUpper
            {
                componentSection = 1;
                break;
            }
            case 10: // ScalpLower
            {
                componentSection = 2;
                break;
            }

            default:
            {
                componentSection = 255;
            }
        }

        itemDisplayMaterialResource.componentSection = componentSection;
        itemDisplayMaterialResourcesStorage.Replace(recordID, itemDisplayMaterialResource);
    }

    for (u32 i = 0; i < layout.sections[0].relationshipMap.entriesCount; i++)
    {
        DB2::WDC3::Layout::RelationshipMapEntry* relationshipEntry = layout.sections[0].relationshipMap.entries + i;

        u32 rowID = *(layout.sections[0].idListData + relationshipEntry->recordIndex);
        auto& itemDisplayMaterialResource = itemDisplayMaterialResourcesStorage.Get<Definitions::ItemDisplayMaterialResources>(rowID);
        itemDisplayMaterialResource.displayID = relationshipEntry->foreignID;
    }

    RepopulateFromCopyTable<Definitions::ItemDisplayMaterialResources>(layout, itemDisplayMaterialResourcesStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!itemDisplayMaterialResourcesStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractItemDisplayModelMaterialResources(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    itemDisplayModelMaterialResourcesStorage.Initialize( {
        { "DisplayID",              ClientDB::FieldType::I32    },
        { "ModelIndex",             ClientDB::FieldType::I8     },
        { "TextureType",            ClientDB::FieldType::I8     },
        { "MaterialResourcesID",    ClientDB::FieldType::I32    }
    });
    itemDisplayModelMaterialResourcesStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::ItemDisplayModelMaterialResources itemDisplayModelMaterialResource;
        itemDisplayModelMaterialResource.displayID = 0;
        itemDisplayModelMaterialResource.modelIndex = static_cast<u8>(db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2));
        itemDisplayModelMaterialResource.textureType = static_cast<u8>(db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1));
        itemDisplayModelMaterialResource.materialResourcesID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 0);

        itemDisplayModelMaterialResourcesStorage.Replace(recordID, itemDisplayModelMaterialResource);
    }

    for (u32 i = 0; i < layout.sections[0].relationshipMap.entriesCount; i++)
    {
        DB2::WDC3::Layout::RelationshipMapEntry* relationshipEntry = layout.sections[0].relationshipMap.entries + i;

        u32 rowID = *(layout.sections[0].idListData + relationshipEntry->recordIndex);
        auto& itemDisplayModelMaterialResource = itemDisplayModelMaterialResourcesStorage.Get<Definitions::ItemDisplayModelMaterialResources>(rowID);
        itemDisplayModelMaterialResource.displayID = relationshipEntry->foreignID;
    }

    RepopulateFromCopyTable<Definitions::ItemDisplayModelMaterialResources>(layout, itemDisplayModelMaterialResourcesStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!itemDisplayModelMaterialResourcesStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractItemDisplayInfo(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    itemDisplayInfoStorage.Initialize( {
        { "ItemVisual",                 ClientDB::FieldType::I32    },
        { "ParticleColorID",            ClientDB::FieldType::I32    },
        { "ItemRangedDisplayInfoID",    ClientDB::FieldType::I32    },
        { "OverrideSwooshSoundKitID",   ClientDB::FieldType::I32    },
        { "SheatheTransformMatrixID",   ClientDB::FieldType::I32    },
        { "StateSpellVisualKitID",      ClientDB::FieldType::I32    },
        { "SheathedSpellVisualKitID",   ClientDB::FieldType::I32    },
        { "UnsheathedSpellVisualKitID", ClientDB::FieldType::I32    },
        { "Flags",                      ClientDB::FieldType::I32    },
        { "ModelResourcesID1",          ClientDB::FieldType::I32    },
        { "ModelResourcesID2",          ClientDB::FieldType::I32    },
        { "MaterialResourcesID1",       ClientDB::FieldType::I32    },
        { "MaterialResourcesID2",       ClientDB::FieldType::I32    },
        { "ModelType1",                 ClientDB::FieldType::I32    },
        { "ModelType2",                 ClientDB::FieldType::I32    },
        { "GeosetGroup1",               ClientDB::FieldType::I32    },
        { "GeosetGroup2",               ClientDB::FieldType::I32    },
        { "GeosetGroup3",               ClientDB::FieldType::I32    },
        { "GeosetGroup4",               ClientDB::FieldType::I32    },
        { "GeosetGroup5",               ClientDB::FieldType::I32    },
        { "GeosetGroup6",               ClientDB::FieldType::I32    },
        { "GeosetAttachmentGroup1",     ClientDB::FieldType::I32    },
        { "GeosetAttachmentGroup2",     ClientDB::FieldType::I32    },
        { "GeosetAttachmentGroup3",     ClientDB::FieldType::I32    },
        { "GeosetAttachmentGroup4",     ClientDB::FieldType::I32    },
        { "GeosetAttachmentGroup5",     ClientDB::FieldType::I32    },
        { "GeosetAttachmentGroup6",     ClientDB::FieldType::I32    },
        { "GeosetHelmetVis1",           ClientDB::FieldType::I32    },
        { "GeosetHelmetVis2",           ClientDB::FieldType::I32    },
    });
    itemDisplayInfoStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::ItemDisplayInfo itemDisplayInfo;
        itemDisplayInfo.itemVisual = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        itemDisplayInfo.particleColorID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        itemDisplayInfo.itemRangedDisplayInfoID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        itemDisplayInfo.overrideSwooshSoundKitID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        itemDisplayInfo.sheatheTransformMatrixID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 5);
        itemDisplayInfo.stateSpellVisualKitID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 6);
        itemDisplayInfo.sheathedSpellVisualKitID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 7);
        itemDisplayInfo.unsheathedSpellVisualKitID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 8);
        itemDisplayInfo.flags = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 9);

        // modelResourcesID
        const u32* modelResourceIDs = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 10);
        memcpy(&itemDisplayInfo.modelResourcesID[0], modelResourceIDs, 2 * sizeof(u32));

        // materialResourcesID
        const u32* materialResourcesIDs = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 11);
        memcpy(&itemDisplayInfo.materialResourcesID[0], materialResourcesIDs, 2 * sizeof(u32));

        // modelType
        const u32* modelTypes = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 12);
        memcpy(&itemDisplayInfo.modelType[0], modelTypes, 2 * sizeof(u32));

        // goesetGroup
        const u32* goesetGroups = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 13);
        memcpy(&itemDisplayInfo.geosetGroup[0], goesetGroups, 6 * sizeof(u32));

        // geosetAttachmentGroup
        const u32* geosetAttachmentGroups = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 14);
        memcpy(&itemDisplayInfo.geosetAttachmentGroup[0], geosetAttachmentGroups, 6 * sizeof(u32));

        // geosetHelmetVis
        const u32* geosetHelmetVis = db2Parser.GetFieldPtr<u32>(layout, sectionID, recordID, recordData, 15);
        memcpy(&itemDisplayInfo.geosetHelmetVis[0], geosetHelmetVis, 2 * sizeof(u32));

        itemDisplayInfoStorage.Replace(recordID, itemDisplayInfo);
    }

    RepopulateFromCopyTable<Definitions::ItemDisplayInfo>(layout, itemDisplayInfoStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!itemDisplayInfoStorage.Save(path))
        return false;

    return true;
}

bool ClientDBExtractor::ExtractLight(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    lightStorage.Initialize( {
        { "MapID",          ClientDB::FieldType::I16    },
        { "PositionX",      ClientDB::FieldType::F32    },
        { "PositionY",      ClientDB::FieldType::F32    },
        { "PositionZ",      ClientDB::FieldType::F32    },
        { "FallOffX",       ClientDB::FieldType::F32    },
        { "FallOffY",       ClientDB::FieldType::F32    },
        { "LightParamsID1", ClientDB::FieldType::I16    },
        { "LightParamsID2", ClientDB::FieldType::I16    },
        { "LightParamsID3", ClientDB::FieldType::I16    },
        { "LightParamsID4", ClientDB::FieldType::I16    },
        { "LightParamsID5", ClientDB::FieldType::I16    },
        { "LightParamsID6", ClientDB::FieldType::I16    },
        { "LightParamsID7", ClientDB::FieldType::I16    },
        { "LightParamsID8", ClientDB::FieldType::I16    }
    });
    lightStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::Light light;
        light.mapID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 3);

        vec3 position = *db2Parser.GetFieldPtr<vec3>(layout, sectionID, recordID, recordData, 0);
        light.position = CoordinateSpaces::TerrainPosToNovus(position);
        light.fallOff.x = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 1);
        light.fallOff.y = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 2);

        const u16* lightParamIDs = db2Parser.GetFieldPtr<u16>(layout, sectionID, recordID, recordData, 4);
        memcpy(&light.lightParamsID[0], lightParamIDs, 8 * sizeof(u16));
        lightStorage.Replace(recordID, light);
    }

    RepopulateFromCopyTable<Definitions::Light>(layout, lightStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLightParams(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    lightParamsStorage.Initialize( {
        { "Flags",              ClientDB::FieldType::I8    },
        { "LightSkyboxID",      ClientDB::FieldType::I16    },
        { "Glow",               ClientDB::FieldType::F32    },
        { "WaterShallowAlpha",  ClientDB::FieldType::F32    },
        { "WaterDeepAlpha",     ClientDB::FieldType::F32    },
        { "OceanShallowAlpha",  ClientDB::FieldType::F32    },
        { "OceanDeepAlpha",     ClientDB::FieldType::F32    }
    });
    lightParamsStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LightParam lightParam;
        recordID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        lightParam.flags.highlightSky = db2Parser.GetField<u8>(layout, sectionID, recordID, recordData, 2);
        lightParam.lightSkyboxID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 3);
        lightParam.glow = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 5);
        lightParam.waterShallowAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 6);
        lightParam.waterDeepAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 7);
        lightParam.oceanShallowAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 8);
        lightParam.oceanDeepAlpha = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 9);
        lightParamsStorage.Replace(db2RecordIndex, lightParam);
    }

    RepopulateFromCopyTable<Definitions::LightParam>(layout, lightParamsStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightParamsStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLightData(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    lightDataStorage.Initialize( {
        { "LightParamID",               ClientDB::FieldType::I16    },
        { "Timestamp",                  ClientDB::FieldType::I32    },
        { "DiffuseColor",               ClientDB::FieldType::I32    },
        { "AmbientColor",               ClientDB::FieldType::I32    },
        { "SkyTopColor",                ClientDB::FieldType::I32    },
        { "SkyMiddleColor",             ClientDB::FieldType::I32    },
        { "SkyBand1Color",              ClientDB::FieldType::I32    },
        { "SkyBand2Color",              ClientDB::FieldType::I32    },
        { "SkySmogColor",               ClientDB::FieldType::I32    },
        { "SkyFogColor",                ClientDB::FieldType::I32    },
        { "SunColor",                   ClientDB::FieldType::I32    },
        { "SunFogColor",                ClientDB::FieldType::I32    },
        { "SunFogStrength",             ClientDB::FieldType::F32    },
        { "CloudSunColor",              ClientDB::FieldType::I32    },
        { "CloudEmissiveColor",         ClientDB::FieldType::I32    },
        { "CloudLayer1AmbientColor",    ClientDB::FieldType::I32    },
        { "CloudLayer2AmbientColor",    ClientDB::FieldType::I32    },
        { "OceanShallowColor",          ClientDB::FieldType::I32    },
        { "OceanDeppColor",             ClientDB::FieldType::I32    },
        { "RiverShallowColor",          ClientDB::FieldType::I32    },
        { "RiverDeepColor",             ClientDB::FieldType::I32    },
        { "ShadowColor",                ClientDB::FieldType::I32    },
        { "FogEnd",                     ClientDB::FieldType::F32    },
        { "FogScaler",                  ClientDB::FieldType::F32    },
        { "FogDensity",                 ClientDB::FieldType::F32    },
        { "SunFogAngle",                ClientDB::FieldType::F32    },
        { "CloudDensity",               ClientDB::FieldType::F32    },
        { "FogHeightColor",             ClientDB::FieldType::I32    },
        { "FogEndColor",                ClientDB::FieldType::I32    },
        { "FogEndHeightColor",          ClientDB::FieldType::I32    },
    });
    lightDataStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LightData lightData;
        lightData.lightParamID = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 0);
        u16 timestamp = db2Parser.GetField<u16>(layout, sectionID, recordID, recordData, 1);
        lightData.timestamp = static_cast<u32>(timestamp) * 30;
        lightData.diffuseColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        lightData.ambientColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 3);
        lightData.skyTopColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 4);
        lightData.skyMiddleColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 5);
        lightData.skyBand1Color = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 6);
        lightData.skyBand2Color = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 7);
        lightData.skySmogColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 8);
        lightData.skyFogColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 9);
        lightData.sunColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 10);
        lightData.sunFogColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 38);
        lightData.sunFogStrength = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 39);
        lightData.sunFogAngle = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 29);
        lightData.cloudSunColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 11);
        lightData.cloudEmissiveColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 12);
        lightData.cloudLayer1AmbientColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 13);
        lightData.cloudLayer2AmbientColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 14);
        lightData.oceanShallowColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 15);
        lightData.oceanDeepColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 16);
        lightData.riverShallowColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 17);
        lightData.riverDeepColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 18);
        lightData.shadowColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 19);
        lightData.fogEnd = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 20) / 36;
        lightData.fogScaler = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 21);
        lightData.fogDensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 22);
        lightData.cloudDensity = db2Parser.GetField<f32>(layout, sectionID, recordID, recordData, 30);
        lightData.fogHeightColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 1);
        lightData.fogEndColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 35);
        lightData.fogEndHeightColor = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 41);

        lightDataStorage.Replace(recordID, lightData);
    }

    RepopulateFromCopyTable<Definitions::LightData>(layout, lightDataStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightDataStorage.Save(path))
        return false;

    return true;
}
bool ClientDBExtractor::ExtractLightSkybox(const std::string& name)
{
    CascLoader* cascLoader = ServiceLocator::GetCascLoader();

    DB2::WDC3::Layout layout = { };
    DB2::WDC3::Parser db2Parser = { };

    std::shared_ptr<Bytebuffer> buffer = cascLoader->GetFileByListFilePath(GetFilePathForDB2ByName(name));
    if (!buffer || !db2Parser.TryParse(buffer, layout))
        return false;

    const DB2::WDC3::Layout::Header& header = layout.header;
    
    lightSkyboxStorage.Initialize( {
        { "ModelHash",   ClientDB::FieldType::I32    },
    });
    lightSkyboxStorage.Reserve(header.recordCount);

    for (u32 db2RecordIndex = 0; db2RecordIndex < header.recordCount; db2RecordIndex++)
    {
        u32 sectionID = 0;
        u32 recordID = 0;
        u8* recordData = nullptr;

        if (!db2Parser.TryReadRecord(layout, db2RecordIndex, sectionID, recordID, recordData))
            continue;

        Definitions::LightSkybox lightSkybox;

        u32 nameHash = std::numeric_limits<u32>::max();
        
        u32 fileID = db2Parser.GetField<u32>(layout, sectionID, recordID, recordData, 2);
        if (fileID == 0)
        {
            std::string modelPath = GetStringFromRecordIndex(layout, db2Parser, db2RecordIndex, 0);

            if (cascLoader->ListFileContainsPath(modelPath))
            {
                if (!modelPath.empty())
                {
                    nameHash = StringUtils::fnv1a_32(modelPath.c_str(), modelPath.size());
                }
            }
        }
        else
        {
            if (cascLoader->InCascAndListFile(fileID))
            {
                const std::string& fileStr = cascLoader->GetFilePathFromListFileID(fileID);

                fs::path filePath = fs::path(fileStr).replace_extension(Model::FILE_EXTENSION);
                nameHash = StringUtils::fnv1a_32(filePath.string().c_str(), filePath.string().size());
            }
        }

        lightSkybox.modelHash = nameHash;
        lightSkyboxStorage.Replace(recordID, lightSkybox);
    }

    RepopulateFromCopyTable<Definitions::LightSkybox>(layout, lightSkyboxStorage);

    std::string path = (ServiceLocator::GetRuntime()->paths.clientDB / name).replace_extension(ClientDB::FILE_EXTENSION).string();
    if (!lightSkyboxStorage.Save(path))
        return false;

    return true;
}