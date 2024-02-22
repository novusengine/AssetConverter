local function SetupLib()
    local basePath = path.getabsolute("Casc/", AssetConverter.dependencyDir)
    local dependencies = { }
    local defines = { "_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS", "_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS" }

    ProjectTemplate("Casc", "StaticLib", nil, AssetConverter.binDir, dependencies, defines)

    local sourceDir = path.getabsolute("Casc/src/", basePath)
    local includeDir = sourceDir
    local files =
    {
        -- Header Files
        sourceDir.. "/CascCommon.h",
        sourceDir.. "/CascLib.h",
        sourceDir.. "/CascPort.h",
        sourceDir.. "/common/Array.h",
        sourceDir.. "/common/Common.h",
        sourceDir.. "/common/Csv.h",
        sourceDir.. "/common/Directory.h",
        sourceDir.. "/common/FileStream.h",
        sourceDir.. "/common/FileTree.h",
        sourceDir.. "/common/ListFile.h",
        sourceDir.. "/common/Map.h",
        sourceDir.. "/common/Mime.h",
        sourceDir.. "/common/Path.h",
        sourceDir.. "/common/RootHandler.h",
        sourceDir.. "/common/Sockets.h",
        sourceDir.. "/jenkins/lookup.h",

        -- Source Files
        sourceDir .. "/common/Common.cpp",
        sourceDir .. "/common/Directory.cpp",
        sourceDir .. "/common/Csv.cpp",
        sourceDir .. "/common/FileStream.cpp",
        sourceDir .. "/common/FileTree.cpp",
        sourceDir .. "/common/ListFile.cpp",
        sourceDir .. "/common/Mime.cpp",
        sourceDir .. "/common/RootHandler.cpp",
        sourceDir .. "/common/Sockets.cpp",
        sourceDir .. "/hashes/md5.cpp",
        sourceDir .. "/hashes/sha1.cpp",
        sourceDir .. "/jenkins/lookup3.c",
        sourceDir .. "/overwatch/apm.cpp",
        sourceDir .. "/overwatch/cmf.cpp",
        sourceDir .. "/overwatch/aes.cpp",
        sourceDir .. "/CascDecompress.cpp",
        sourceDir .. "/CascDecrypt.cpp",
        sourceDir .. "/CascDumpData.cpp",
        sourceDir .. "/CascFiles.cpp",
        sourceDir .. "/CascFindFile.cpp",
        sourceDir .. "/CascIndexFiles.cpp",
        sourceDir .. "/CascOpenFile.cpp",
        sourceDir .. "/CascOpenStorage.cpp",
        sourceDir .. "/CascReadFile.cpp",
        sourceDir .. "/CascRootFile_Diablo3.cpp",
        sourceDir .. "/CascRootFile_Install.cpp",
        sourceDir .. "/CascRootFile_MNDX.cpp",
        sourceDir .. "/CascRootFile_Text.cpp",
        sourceDir .. "/CascRootFile_TVFS.cpp",
        sourceDir .. "/CascRootFile_OW.cpp",
        sourceDir .. "/CascRootFile_WoW.cpp",

        -- Bundled ZLIB
        sourceDir .. "/zlib/adler32.c",
        sourceDir .. "/zlib/crc32.c",
        sourceDir .. "/zlib/inffast.c",
        sourceDir .. "/zlib/inflate.c",
        sourceDir .. "/zlib/inftrees.c",
        sourceDir .. "/zlib/zutil.c",
    }
    AddFiles(files)

    AddIncludeDirs(includeDir)

    AddDefines({"D_7ZIP_ST", "DBZ_STRICT_ANSI", "CASCLIB_NO_AUTO_LINK_LIBRARY"})

    AddLinks("wininet")
end
SetupLib()

local function Include()
    local basePath = path.getabsolute("Casc/", AssetConverter.dependencyDir)
    local includeDir = path.getabsolute("Casc/src/", basePath)

    AddIncludeDirs(includeDir)
    AddDefines({"CASCLIB_NO_AUTO_LINK_LIBRARY"})
    AddLinks("Casc")
end
CreateDep("casc", Include)