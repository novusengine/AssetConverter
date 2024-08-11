local dep = Solution.Util.CreateDepTable("Casc", {})

Solution.Util.CreateStaticLib(dep.Name, Solution.Projects.Current.BinDir, dep.Dependencies, function()
    local defines = { "_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS", "_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS", "D_7ZIP_ST", "DBZ_STRICT_ANSI", "CASCLIB_NO_AUTO_LINK_LIBRARY", "CASCLIB_NODEBUG" }

    Solution.Util.SetLanguage("C++")
    Solution.Util.SetCppDialect(20)

    local sourceDir = dep.Path .. "/" .. dep.Name
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

    Solution.Util.SetFiles(files)
    Solution.Util.SetIncludes({ dep.Path, sourceDir })
    Solution.Util.SetDefines(defines)

    Solution.Util.SetFilter("platforms:Win64", function()
        Solution.Util.SetLinks({ "wininet" })
    end)
end)

Solution.Util.CreateDep(dep.NameLow, dep.Dependencies, function()
    Solution.Util.SetIncludes(dep.Path)
    Solution.Util.SetLinks(dep.Name)
    
    local defines = { "CASCLIB_NO_AUTO_LINK_LIBRARY", "CASCLIB_NODEBUG" }
    Solution.Util.SetDefines(defines)
end)