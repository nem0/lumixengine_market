local cfg_file

function createTextureMeta(path)
	local mp = io.open(path, "w")
	mp:write("srgb = true")
	mp:close()
end

function createFBXMeta(path)
	local mp = io.open(path, "w")
	mp:write('origin = "bottom"')
	mp:close()
end

local center_packages = { "kenney_city-kit-roads", "kenney_castle-kit",  "kenney_pirate-kit", "kenney_racing-kit", "kenney_retro-urban-kit", "kenney_space-kit" }

-- put original zip file from kenney in scripts/ folder
-- run ./genie kenney
-- it creates zip and png file in plugins/market/data/kenney/*
-- and scripts/list.lua
-- edit and copy scripts/list.lua to plugins/market/data/kenney/list.lua
function repackage_kenney(file)
	printf("repacking " .. file .. "...")
	local basename = path.getbasename(file)
	os.mkdir("unpacked/" .. basename)
	os.copyfile(file, "unpacked/" .. basename .. "/" .. file)
	os.chdir("unpacked/" .. basename)
	os.outputof("unzip " .. file)

	local repacked_dir = "../../repacked/" .. basename .. "/";

	local files = os.matchfiles("**.fbx")
	
	if #files == 0 then
		printf("Warning: No FBX files in " .. basename .. ", skipped.")
		os.chdir("../..") 
		return
	end

	os.mkdir(repacked_dir)

	local center_meshes = table.contains(center_packages, basename)

	for _, v in pairs(files) do
		os.copyfile(v, repacked_dir .. path.getbasename(v) .. ".fbx")
		if center_meshes then
			createFBXMeta(repacked_dir .. path.getbasename(v) .. ".fbx.meta")
		end
	end

	os.copyfile("Preview.png", "../../../plugins/market/data/kenney/" .. basename .. ".png")
	os.copyfile("License.txt", "../../repacked/".. basename .. "/License.txt")

	os.rmdir("Side")
	os.rmdir("Isometric")
	os.rmdir("Topdown")
	os.rmdir("Previews")

	local files = os.matchfiles("**.png")
	for _, v in pairs(files) do
		if v ~= "Preview.png" and v ~= "Sample.png" then 
			os.copyfile(v, repacked_dir .. path.getbasename(v) .. ".png")
			createTextureMeta(repacked_dir .. path.getbasename(v) .. ".png.meta")
		end
	end

	os.chdir("../../repacked/" .. basename)
	os.outputof("tar -a -c -f ../../../plugins/market/data/kenney/" .. basename .. ".zip *")

	os.chdir("../..")

	cfg_file:write("\t{\n")
	cfg_file:write('\t\tname = "' .. basename .. '",\n')
	cfg_file:write('\t\ttags = "model, lowpoly",\n')
	cfg_file:write('\t\tpath = "https://raw.githubusercontent.com/nem0/lumixengine_market/master/data/kenney/' .. basename .. '.zip",\n')
	cfg_file:write('\t\tthumbnail = "https://raw.githubusercontent.com/nem0/lumixengine_market/master/data/kenney/' .. basename .. '.png"\n')
	cfg_file:write("\t},\n")
end

newaction {
	trigger     = "kenney",
	description = "Repackage Kenney's assets for Lumix Engine",
	execute     = function()
		local files = os.matchfiles("*.zip")
		cfg_file = io.open("list.lua", "w")
		cfg_file:write("{\n")
		for _, v in pairs(files) do
			repackage_kenney(v)
		end
		cfg_file:write("}\n")
		cfg_file:close()
	end
}

if plugin "market" then
	files { 
		"src/**.c",
		"src/**.cpp",
		"src/**.h",
		"genie.lua"
	}
	includedirs { "../../external/luau/include" }
	defines { "BUILDING_MARKET" }
	links { "engine" }
end