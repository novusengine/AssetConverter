if BuildSettings == nil then
    BuildSettings = { }

    BuildSettings.Has = function(settings, name)
        return settings[name] ~= nil
    end

    BuildSettings.Add = function(settings, name, value, ignoreError)
        if ignoreError == nil then
            ignoreError = false
        end

        if not settings:Has(name) and not ignoreError then
            error("Tried to call BuildSettings:Add with name '" .. name .. '" but a setting with that name already exists')
        end

        settings[name] = value
    end

    BuildSettings.Get = function(settings, name)
        if not settings:Has(name) then
            return nil
        end

        return settings[name]
    end
end

-- Determines the system premake will generate a solution for
BuildSettings:Add("Solution Type", "vs2022", true)
BuildSettings:Add("Multithreaded Compilation", true, true) -- Used to enable multi threaded compilation
BuildSettings:Add("Multithreaded Core Count", 0, true) -- Used to adjust the allowed number of cores when enabling multi threaded compilation (0 = All) (Assuming the selected build environment supports it)

-- Enables/Disables Tracy
BuildSettings:Add("Enable Tracy", false, true)

-- Enables/Disables the UnitTest Project
BuildSettings:Add("Build UnitTests", true, true)

-- Settings for Luau
BuildSettings:Add("Luau Warnings as Errors", false, true)

-- Settings for EnkiTS
BuildSettings:Add("EnkiTS Num Task Priorities", 3, true)

-- Settings for Typesafe
BuildSettings:Add("Typesafe Enable Assertions", false, true)
BuildSettings:Add("Typesafe Enable Precondition Checks", false, true)
BuildSettings:Add("Typesafe Enable Wrapper", false, true)
BuildSettings:Add("Typesafe Arithmetic Policy", 0, true) -- (0 = "default", 1 = "ub", 2 = "checked")

-- Custom (DO NOT TOUCH)
local isMSVC = string.sub(_ACTION, 1, 2) == "vs"
BuildSettings:Add("Using MSVC", isMSVC, true)