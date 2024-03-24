#include "Runtime.h"
#include "Blp/BlpConvert.h"
#include "Casc/CascLoader.h"
#include "Extractors/ClientDBExtractor.h"
#include "Extractors/MapExtractor.h"
#include "Extractors/MapObjectExtractor.h"
#include "Extractors/ComplexModelExtractor.h"
#include "Extractors/TextureExtractor.h"
#include "Util/ServiceLocator.h"

#include <Base/Types.h>
#include <Base/Util/JsonUtils.h>
#include <Base/Util/DebugHandler.h>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <filesystem>
namespace fs = std::filesystem;

i32 main()
{
    Runtime* runtime = ServiceLocator::SetRuntime(new Runtime());

    // Setup Runtime
    {
        // Setup Paths
        {
            Runtime::Paths& paths = runtime->paths;

            paths.executable = fs::current_path();
            paths.data = paths.executable / "Data";
            paths.clientDB = paths.data / "ClientDB";
            paths.texture = paths.data / "Texture";
            paths.textureBlendMap = paths.texture / "blendmaps";
            paths.map = paths.data / "Map";
            paths.mapObject = paths.data / "MapObject";
            paths.complexModel = paths.data / "ComplexModel";

            fs::create_directories(paths.data);
            fs::create_directories(paths.clientDB);
            fs::create_directories(paths.texture);
            fs::create_directories(paths.textureBlendMap);
            fs::create_directories(paths.map);
            fs::create_directories(paths.mapObject);
            fs::create_directories(paths.complexModel);
        }

        // Setup Json
        {
            static const std::string CONFIG_VERSION = "0.4";
            static const std::string CONFIG_NAME = "AssetConverterConfig.json";

            fs::path configPath = runtime->paths.executable / CONFIG_NAME;
            std::string absolutePath = fs::absolute(configPath).string();

            nlohmann::ordered_json fallbackJson;

			bool configExists = fs::exists(configPath);
			if (!configExists)
			{
				DebugHandler::PrintFatal("[AssetConverter] Please copy the {0} to this folder.\n\nPress 'Enter' to exit.", CONFIG_NAME);
			}

            if (!JsonUtils::LoadFromPathOrCreate(runtime->json, fallbackJson, configPath))
            {
                DebugHandler::PrintFatal("[AssetConverter] Failed to Load {0} from {1}", CONFIG_NAME, absolutePath.c_str());
            }

            std::string currentVersion = runtime->json["General"]["Version"];
            if (currentVersion != CONFIG_VERSION)
            {
                DebugHandler::PrintFatal("[AssetConverter] Attempted to load outdated {0}. (Config Version : {1}, Expected Version : {2})", CONFIG_NAME.c_str(), currentVersion.c_str(), CONFIG_VERSION.c_str());
            }

			runtime->isInDebugMode = runtime->json["General"]["DebugMode"];
		}

        // Setup Scheduler
        {
            u32 threadCount = runtime->json["General"]["ThreadCount"];
            if (threadCount == 0 || threadCount == std::numeric_limits<u32>().max())
            {
                threadCount = std::thread::hardware_concurrency() - 1;
            }

            runtime->scheduler.Initialize(threadCount);
        }
    }

    // Setup CascLoader
    {
        const std::string& listFile = runtime->json["Casc"]["ListFile"];
        const std::string& locale = runtime->json["Casc"]["Locale"];

        ServiceLocator::SetCascLoader(new CascLoader(listFile, locale));
    }

    // Setup Jolt
    {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    // Run Extractors
    {
        CascLoader* cascLoader = ServiceLocator::GetCascLoader();

        CascLoader::Result result = cascLoader->Load();
        switch (result)
        {
            case CascLoader::Result::Success:
            {
                DebugHandler::Print("");

                bool isExtractingEnabled = runtime->json["Extraction"]["Enabled"];
                if (isExtractingEnabled)
                {
                    DebugHandler::Print("[AssetConverter] Processing Extractors...");

                    // DB2
                    bool isDB2Enabled = runtime->json["Extraction"]["ClientDB"]["Enabled"];
                    if (isDB2Enabled)
                    {
                        DebugHandler::Print("[AssetConverter] Processing ClientDB Extractor...");
                        ClientDBExtractor::Process();
                        DebugHandler::Print("[AssetConverter] ClientDB Extractor Finished\n");
                    }

                    // Map
                    bool isMapEnabled = runtime->json["Extraction"]["Map"]["Enabled"];
                    if (isMapEnabled)
                    {
                        DebugHandler::Print("[AssetConverter] Processing Map Extractor...");
                        MapExtractor::Process();
                        DebugHandler::Print("[AssetConverter] Map Extractor Finished\n");
                    }

                    // Map Object
                    bool isMapObjectEnabled = runtime->json["Extraction"]["MapObject"]["Enabled"];
                    if (isMapObjectEnabled)
                    {
                        DebugHandler::Print("[AssetConverter] Processing MapObject Extractor...");
                        MapObjectExtractor::Process();
                        DebugHandler::Print("[AssetConverter] MapObject Extractor Finished\n");
                    }

                    // Complex Model
                    bool isComplexModelEnabled = runtime->json["Extraction"]["ComplexModel"]["Enabled"];
                    if (isComplexModelEnabled)
                    {
                        DebugHandler::Print("[AssetConverter] Processing ComplexModel Extractor...");
                        ComplexModelExtractor::Process();
                        DebugHandler::Print("[AssetConverter] ComplexModel Extractor Finished\n");
                    }

                    // Texture
                    bool isTextureEnabled = runtime->json["Extraction"]["Texture"]["Enabled"];
                    if (isTextureEnabled)
                    {
                        DebugHandler::Print("[AssetConverter] Processing Texture Extractor...");
                        TextureExtractor::Process();
                        DebugHandler::Print("[AssetConverter] Texture Extractor Finished\n");
                    }
                }

                cascLoader->Close();
                break;
            }

            case CascLoader::Result::MissingCasc:
            {
                DebugHandler::PrintError("[CascLoader] Could not load Casc. Failed to find Installation");
                break;
            }

            case CascLoader::Result::MissingListFile:
            {
                DebugHandler::PrintError("[CascLoader] Could not load Casc. Failed to find Listfile");
                break;
            }

            case CascLoader::Result::MissingLocale:
            {
                DebugHandler::PrintError("[CascLoader] Could not load Casc. Invalid Locale");
                break;
            }

            case CascLoader::Result::AlreadyInitialized:
            {
                DebugHandler::PrintError("[CascLoader] Could not load Casc. Already Initialized.");
                break;
            }

            default:
            {
                DebugHandler::PrintError("[CascLoader] Could not load Casc. Unknown Result.");
                break;
            }
        }
    }

    DebugHandler::Print("\nFinished... Press 'Enter' to exit");
    std::cin.get();

    return 0;
}