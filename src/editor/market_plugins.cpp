#define LUMIX_NO_CUSTOM_CRT
#include "editor/render_interface.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/allocator.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/os.h"
#include "engine/path.h"
#include "engine/stream.h"
#include "engine/string.h"
#include "openfbx/miniz.h"

#include "imgui/imgui.h"
#include "stb/stb_image.h"

#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")

namespace Lumix {

static bool download(const char* url, OutputMemoryStream& blob, bool use_cache, FileSystem& fs) {
	const StableHash url_hash(url);
	const StaticString<128> cache_path(".market_cache/", url_hash.getHashValue(), ".", Path::getExtension(Span(url, stringLength(url))));
	if (use_cache) {
		if (fs.fileExists(cache_path)) {
			return fs.getContentSync(Path(cache_path), blob);
		}
	}

	IStream* stream = nullptr;
	if (S_OK != URLOpenBlockingStream(nullptr, url, &stream, 0, nullptr)) {
		logError("Failed to download ", url);
		return false;
	}
	char buffer[4096];
	ULONG read = 0;
	HRESULT hr;
	do {
		DWORD bytesRead = 0;
		hr = stream->Read(buffer, sizeof(buffer), &bytesRead);

		if (bytesRead > 0)
		{
			blob.write(buffer, bytesRead);
		}
	} while (SUCCEEDED(hr) && hr != S_FALSE);

	if (use_cache) {
		if (!fs.saveContentSync(Path(cache_path), blob)) {
			logError("Failed to save market cache file ", cache_path);
		}
	}

	return true;
}

static bool extract(Span<const u8> zip, const char* export_dir, FileSystem& fs, IAllocator& allocator) {
	mz_zip_archive archive = {};
	if (!mz_zip_reader_init_mem(&archive, zip.begin(), zip.length(), 0)) return false;

	const mz_uint count = mz_zip_reader_get_num_files(&archive);
	for (u32 i = 0; i < count; ++i) {
		mz_zip_archive_file_stat stat;
		if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
			mz_zip_reader_end(&archive);
			return false;
		}
		OutputMemoryStream blob(allocator);
		auto callback = [](void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n) -> size_t {
			OutputMemoryStream* blob = (OutputMemoryStream*)pOpaque;
			blob->write(pBuf, n);
			return n;
		};
		if (!mz_zip_reader_extract_to_callback(&archive, i, callback, &blob, 0)) {
			mz_zip_reader_end(&archive);
			return false;
		}
		StaticString<LUMIX_MAX_PATH> out_path(export_dir, "/", stat.m_filename);
		if (!fs.saveContentSync(Path(out_path), blob)) {
			logError("Failed to write ", out_path);
			mz_zip_reader_end(&archive);
			return false;
		}
	}
	mz_zip_reader_end(&archive);
	return true;
}

struct MarketPlugin : StudioApp::GUIPlugin {
	MarketPlugin(StudioApp& app)
		: m_app(app)
		, m_items(app.getAllocator())
	{
		m_toggle_ui.init("Marketplace", "Toggle marketplace UI", "marketplace", "", false);
		m_toggle_ui.func.bind<&MarketPlugin::toggleUI>(this);
		m_toggle_ui.is_selected.bind<&MarketPlugin::isOpen>(this);

		const char* base_path = m_app.getEngine().getFileSystem().getBasePath();
		if (!os::makePath(StaticString<LUMIX_MAX_PATH>(base_path, "/.market_cache"))) {
			logError("Failed to create .market_cache");
		}
		getList();

		m_app.addWindowAction(&m_toggle_ui);
	}

	~MarketPlugin() {
		m_app.removeAction(&m_toggle_ui);
	}

	void toggleUI() { m_is_open = !m_is_open; }
	bool isOpen() const { return m_is_open; }

	void getList() {
		lua_State* L = luaL_newstate();
		OutputMemoryStream list(m_app.getAllocator());
		FileSystem& fs =  m_app.getEngine().getFileSystem();
		list << "return ";
		if (!download("https://raw.githubusercontent.com/nem0/lumixengine_market/master/data/list.lua", list, true, fs)) {
			logError("Failed to download market list");
			return;
		}
		
		if (!LuaWrapper::execute(L, Span((const char*)list.data(), (u32)list.size()), "market list", 1)) {
			logError("Failed to parse market list");
			return;
		}

		const int n = (int)lua_objlen(L, -1);
		for (int i = 0; i < n; ++i) {
			MarketItem& item = m_items.emplace(m_app.getAllocator());
			lua_rawgeti(L, -1, i + 1);
			char tmp[LUMIX_MAX_PATH];
			#define CHECK(N) \
				do {\
					if (!LuaWrapper::checkStringField(L, -1, #N, Span(tmp))) luaL_error(L, "missing " #N); \
					item.N = tmp; \
				} while (false)

			CHECK(name);
			CHECK(category);
			CHECK(path);
			CHECK(thumbnail);
			LuaWrapper::getOptionalField(L, -1, "version", &item.version);

			#undef CHECK
			lua_pop(L, 1);
		}
		lua_close(L);
	}
		
	struct MarketItem {
		MarketItem(IAllocator& allocator)
			: name(allocator)
			, category(allocator)
			, path(allocator)
			, thumbnail(allocator)
		{}

		String name;
		String category;
		String path;
		String thumbnail;
		u32 version = 0;

		ImTextureID texture = nullptr;
	};

	void makePath(const char* path) {
		FileSystem& fs = m_app.getEngine().getFileSystem();
		StaticString<LUMIX_MAX_PATH> fullpath(fs.getBasePath(), "/", path);
		if (!os::makePath(fullpath)) {
			logError("Failed to create ", fullpath);
		}
	}

	void install(const MarketItem& item, const char* install_path) {
		FileSystem& fs =  m_app.getEngine().getFileSystem();		
		OutputMemoryStream blob(m_app.getAllocator());
		if (!download(item.path.c_str(), blob, true, fs)) {
			logError("Failed to download ", item.path.c_str());
			return;
		}

		if (Path::hasExtension(item.path.c_str(), "zip")) {
			makePath(install_path);
			if (!extract(blob, install_path, fs, m_app.getAllocator())) {
				logError("Failed to extract ", item.path.c_str(), " to ", install_path);
			}
		}
		else {
			if (!fs.saveContentSync(Path(install_path), blob)) {
				logError("Failed to save ", item.path.c_str(), " as ", install_path);
			}
		}
	}

	void onSettingsLoaded() override {
		m_is_open = m_app.getSettings().getValue(Settings::GLOBAL, "is_marketplace_open", false);
	}

	void onBeforeSettingsSaved() override {
		m_app.getSettings().setValue(Settings::GLOBAL, "is_marketplace_open", m_is_open);
	}

	void onWindowGUI() override {
		if (!m_is_open) return;

		RenderInterface* ri = m_app.getRenderInterface();
		ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Marketplace", &m_is_open)) {
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##filter", "Filter", m_filter, sizeof(m_filter));
			FileSystem& fs =  m_app.getEngine().getFileSystem();		
			const float width = ImGui::GetContentRegionAvail().x;
			u32 columns = maximum(1, u32((width + 128) / 256));
			
			u32 i = 0;
			for (MarketItem& item : m_items) {
				if (m_filter[0] && stristr(item.name.c_str(), m_filter) == nullptr) continue;
				if (!item.texture) {
					OutputMemoryStream thumbnail_data(m_app.getAllocator());
					if (download(item.thumbnail.c_str(), thumbnail_data, true, fs)) {
						int w, h;
						stbi_uc* pixels = stbi_load_from_memory(thumbnail_data.data(), (int)thumbnail_data.size(), &w, &h, nullptr, 4);
						if (pixels) {;
							item.texture = ri->createTexture("market_thumbnail", pixels, w, h);;
							free(pixels);
						}
					}
				}
				
				if (i % columns != 0) ImGui::SameLine();
				ImGui::BeginGroup();
				ImGui::PushID(&item);
				ImVec2 img_size(256, 256);
				ImVec2 tl = ImGui::GetCursorPos();
				ImGui::Image(item.texture, img_size);
				ImVec2 text_size = ImGui::CalcTextSize(item.name.c_str());
				ImVec2 pos = ImGui::GetCursorPos();
				pos.x += (256 - text_size.x) * 0.5f;
				ImGui::SetCursorPos(pos);
				ImGui::TextUnformatted(item.name.c_str());
				
				ImGui::SetCursorPos(tl);
				if (ImGuiEx::IconButton(ICON_FA_DOWNLOAD, "Download & Install")) {
					m_show_install_path = true;
					m_item_to_install = i32(&item - m_items.begin());
				}

				ImGui::PopID();
				ImGui::EndGroup();
				++i;
			}
		}
		DirSelector& ds = m_app.getDirSelector();
		if (ds.gui("Select install path", &m_show_install_path)) install(m_items[m_item_to_install], ds.getDir());
		ImGui::End();
	}
	
	const char* getName() const override { return "market"; }
	
	StudioApp& m_app;
	Array<MarketItem> m_items;
	i32 m_item_to_install = -1;
	bool m_show_install_path = false;
	char m_filter[64] = "";
	bool m_is_open = false;
	Action m_toggle_ui;
};

LUMIX_STUDIO_ENTRY(market)
{
	WorldEditor& editor = app.getWorldEditor();

	auto* plugin = LUMIX_NEW(editor.getAllocator(), MarketPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}

} // namespace Lumix
