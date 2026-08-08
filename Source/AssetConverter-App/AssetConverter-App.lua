local mod = Solution.Util.CreateModuleTable("AssetConverter-App", { "base", "fileformat", "filesystem", "meta", "enkits", "casc", "cuttlefish", "jolt", "tracyprofiler", "recastnavigation-recast", "recastnavigation-detour", "libsodium" })

Solution.Util.CreateConsoleApp(mod.Name, Solution.Projects.Current.BinDir, mod.Dependencies, function()
    local defines = { "_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS", "_SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS", "WIN32_LEAN_AND_MEAN" }

    Solution.Util.SetLanguage("C++")
    Solution.Util.SetCppDialect(20)

    local projFile = mod.Path .. "/" .. mod.Name .. ".lua"
    local files = Solution.Util.GetFilesForCpp(mod.Path)
    table.insert(files, projFile)

    Solution.Util.SetFiles(files)
    Solution.Util.SetIncludes(mod.Path)
    Solution.Util.SetDefines(defines)
    
    Solution.Util.SetFilter("system:Windows", function()
        local appIconFiles =
        {
            "appicon.rc",
            "**.ico"
        }
        Solution.Util.SetFiles(appIconFiles)
    end)
    
    vpaths {
        ['Resources/*'] = { '*.rc', '**.ico' },
        ["/*"] = { "*.lua", mod.Name .. "/**" }
    }
end)
