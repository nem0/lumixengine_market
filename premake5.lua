-- Runs inside the plugin project. Paths are relative to this file.
files { "src/**.c", "src/**.cpp", "src/**.h" }
includedirs { "../../external/luau/include" }
defines { "BUILDING_MARKET" }
if _OPTIONS["dynamic-plugins"] then
	links { "engine" }
end
