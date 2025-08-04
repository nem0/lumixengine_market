#define LUMIX_NO_CUSTOM_CRT
#include <string.h>
#include "core/allocator.h"
#include "core/log.h"
#include "core/os.h"
#include "core/path.h"
#include "core/profiler.h"
#include "core/stream.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/thread.h"
#include "editor/render_interface.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/component_uid.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "lua/lua_script_system.h"
#include "lua/lua_wrapper.h"

#include "imgui/imgui.h"
#include "miniz.h"
#include "stb/stb_image.h"

#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")

namespace Lumix {

static const char* LIST_URL = "https://raw.githubusercontent.com/nem0/lumixengine_market/master/data/list.lua";

struct DownloadThread : Thread {
	struct Job {
		Job(IAllocator& allocator)
		: url(allocator)
		, blob(allocator)
		{}

		virtual void finished() = 0;

		virtual ~Job() {}

		String url;
		OutputMemoryStream blob;
		bool use_cache;
		bool failed_download = false;
		bool canceled = false;
	};

	DownloadThread(StudioApp& app, IAllocator& allocator)
		: Thread(allocator)
		, m_app(app)
		, m_semaphore(0, 99999)
		, m_jobs(allocator)
		, m_finished_jobs(allocator)
	{}

	int task() override {
		PROFILE_FUNCTION();
		while(!m_finished) {
			m_semaphore.wait();
			m_mutex.enter();
			if (!m_jobs.empty()) {
				UniquePtr<Job> job = m_jobs.back().move();
				m_jobs.pop();
				m_current_job = job.get();
				const bool canceled = job->canceled;
				m_mutex.exit();
				if (!canceled) {
					job->failed_download = !download(job->url.c_str(), job->blob, job->use_cache, m_app.getEngine().getFileSystem());
				}
				m_mutex.enter();
				m_current_job = nullptr;
				m_finished_jobs.emplace(job.move());
			}
			m_mutex.exit();
		}
		return 0;
	}
	
	static bool download(const char* url, OutputMemoryStream& blob, bool use_cache, FileSystem& fs) {
		const StableHash url_hash(url);
		const StaticString<128> cache_path(".lumix/.market_cache/", url_hash.getHashValue(), ".", Path::getExtension(url));
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

		stream->Release();

		if (use_cache) {
			if (!fs.saveContentSync(Path(cache_path), blob)) {
				logError("Failed to save market cache file ", cache_path);
			}
		}

		return true;
	}

	void cancelAll() {
		MutexGuard guard(m_mutex);
		if (m_current_job) m_current_job->canceled = true;
		for (const UniquePtr<Job>& job : m_jobs) {
			job->canceled = true;
		}
		for (const UniquePtr<Job>& job : m_finished_jobs) {
			job->canceled = true;
		}
	}

	template <typename F>
	void download(const char* url, const F& f) {
		struct MyJob : Job {
			MyJob(const F& f, IAllocator& allocator)
				: Job(allocator)
				, func(f)
			{}

			void finished() override { func(blob); }
			F func;
		};

		UniquePtr<MyJob> job = UniquePtr<MyJob>::create(m_app.getAllocator(), f, m_app.getAllocator());
		job->url = url;
		job->use_cache = true;
		pushJob(job.move());
	}

	void pushJob(UniquePtr<Job>&& job) {
		++m_to_do_count;
		MutexGuard guard(m_mutex);
		m_jobs.push(job.move());
		m_semaphore.signal();
	}

	UniquePtr<Job> popFinishedJob() {
		MutexGuard guard(m_mutex);
		if (m_finished_jobs.empty()) return {};
		--m_to_do_count;
		UniquePtr<Job> job = m_finished_jobs.back().move();
		m_finished_jobs.pop();
		return job.move();
	}

	StudioApp& m_app;
	Semaphore m_semaphore;
	Mutex m_mutex;
	Array<UniquePtr<Job>> m_jobs;
	Array<UniquePtr<Job>> m_finished_jobs;
	Job* m_current_job = nullptr;
	bool m_finished = false;
	u32 m_to_do_count = 0;
};

static bool extract(Span<const u8> zip, const char* export_dir, FileSystem& fs, IAllocator& allocator) {
	mz_zip_archive archive = {};
	if (!mz_zip_reader_init_mem(&archive, zip.begin(), zip.length(), 0)) return false;

	const mz_uint count = mz_zip_reader_get_num_files(&archive);
	bool res = true;
	for (u32 i = 0; i < count; ++i) {
		mz_zip_archive_file_stat stat;
		if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
			mz_zip_reader_end(&archive);
			return false;
		}
		if (stat.m_is_directory) {
			StaticString<MAX_PATH> path(fs.getBasePath(), export_dir, "/", stat.m_filename);
			if (!os::makePath(path)) {
				logError("Failed to create ", path);
				res = false;
			}
		}
		else {
			OutputMemoryStream blob(allocator);
			auto callback = [](void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n) -> size_t {
				OutputMemoryStream* blob = (OutputMemoryStream*)pOpaque;
				blob->write(pBuf, n);
				return n;
			};
			if (!mz_zip_reader_extract_to_callback(&archive, i, callback, &blob, 0)) {
				logError("Failed to extract ", stat.m_filename);
				res = false;
			}
			StaticString<MAX_PATH> out_path(export_dir, "/", stat.m_filename);
			if (!fs.saveContentSync(Path(out_path), blob)) {
				logError("Failed to write ", out_path);
				res = false;
			}
		}
	}
	mz_zip_reader_end(&archive);
	return res;
}

struct MarketPlugin : StudioApp::GUIPlugin {
	MarketPlugin(StudioApp& app)
		: m_app(app)
		, m_items(app.getAllocator())
		, m_download_thread(app, app.getAllocator())
	{
		createLuaAPI();

		const char* base_path = m_app.getEngine().getFileSystem().getBasePath();
		if (!os::makePath(StaticString<MAX_PATH>(base_path, "/.lumix/.market_cache"))) {
			logError("Failed to create .lumix/.market_cache");
		}
		getList(false);

		m_download_thread.create("market_download", true);
		m_app.getSettings().registerOption("market_open", &m_is_open);

		m_app.addPlugin((StudioApp::GUIPlugin&)*this);
	}

	~MarketPlugin() {
		m_app.removePlugin((StudioApp::GUIPlugin&)*this);
		m_download_thread.cancelAll();
		m_download_thread.m_finished = true;
		m_download_thread.m_semaphore.signal();
		m_download_thread.destroy();
	}

	static int LUA_nextFile(lua_State* L) {
		os::FileIterator* iter = LuaWrapper::checkArg<os::FileIterator*>(L, 1);
		os::FileInfo info;
		if (!os::getNextFile(iter, &info)) return 0;

		LuaWrapper::push(L, info.is_directory);
		LuaWrapper::push(L, info.filename);
		return 2;
	}

	static int LUA_createFileIterator(lua_State* L) {
		const char* dir = LuaWrapper::checkArg<const char*>(L, 1);
		int upvalue_index = lua_upvalueindex(1);
		if (!LuaWrapper::isType<MarketPlugin*>(L, upvalue_index)) {
			ASSERT(false);
			luaL_error(L, "Invalid Lua closure");
		}
		MarketPlugin* plugin = LuaWrapper::checkArg<MarketPlugin*>(L, upvalue_index);

		StaticString<MAX_PATH> path(plugin->m_app.getEngine().getFileSystem().getBasePath(), dir);
		LuaWrapper::push(L, os::createFileIterator(path, plugin->m_app.getAllocator()));
		return 1;
	}

	static int LUA_downloadAndExtract(lua_State* L) {
		int upvalue_index = lua_upvalueindex(1);
		if (!LuaWrapper::isType<MarketPlugin*>(L, upvalue_index)) {
			ASSERT(false);
			luaL_error(L, "Invalid Lua closure");
		}
		MarketPlugin* plugin = LuaWrapper::checkArg<MarketPlugin*>(L, upvalue_index);

		const char* url = LuaWrapper::checkArg<const char*>(L, 1);
		StaticString<MAX_PATH> dir = LuaWrapper::checkArg<const char*>(L, 2);

		plugin->m_download_thread.download(url, [L, dir, plugin](OutputMemoryStream& blob){
			plugin->makePath(dir);
			bool res = extract(blob, dir, plugin->m_app.getEngine().getFileSystem(), plugin->m_app.getAllocator());
			lua_pushboolean(L, res);
			int status = lua_resume(L, nullptr, 1);
			if (status != LUA_YIELD && status != LUA_OK) {
				logError(lua_tostring(L, -1));
			}
		});
		return lua_yield(L, 1);
	}

	lua_State* getLuaState() {
		auto* lua_system = (LuaScriptSystem*)m_app.getEngine().getSystemManager().getSystem("lua_script");
		return lua_system->getState();
	}

	void createLuaAPI() {
		m_state = lua_newthread(getLuaState());
		LuaWrapper::createSystemClosure(m_state, "LumixMarket", this, "downloadAndExtract", &LUA_downloadAndExtract);
		LuaWrapper::createSystemClosure(m_state, "LumixMarket", this, "createFileIterator", &LUA_createFileIterator);
		LuaWrapper::createSystemFunction(m_state, "LumixMarket", "destroyFileIterator", &LuaWrapper::wrap<&os::destroyFileIterator>);
		LuaWrapper::createSystemFunction(m_state, "LumixMarket", "nextFile", &LUA_nextFile);
	}

	void getList(bool force) {
		if (force) {
			FileSystem& fs = m_app.getEngine().getFileSystem();
			const StableHash url_hash(LIST_URL);
			fs.deleteFile(StaticString<MAX_PATH>(".lumix/.market_cache/", url_hash.getHashValue(), ".lua"));
		}
		m_download_thread.cancelAll();
		m_items.clear();
		m_download_thread.download(LIST_URL, [&](const OutputMemoryStream& blob){
			lua_State* L = m_state;
			if (!LuaWrapper::execute(L, StringView((const char*)blob.data(), (u32)blob.size()), "market list", 1)) {
				logError("Failed to parse market list");
				return;
			}

			// TODO leak
			m_list_ref = LuaWrapper::createRef(L);

			const int n = (int)lua_objlen(L, -1);
			m_items.reserve(n);
			for (int i = 0; i < n; ++i) {
				MarketItem& item = m_items.emplace(m_app.getAllocator());
				lua_rawgeti(L, -1, i + 1);
				char tmp[MAX_PATH];
				#define CHECK(N) \
					if (!LuaWrapper::checkStringField(L, -1, #N, Span(tmp))) { \
						logError("Missing " #N " in market list in item ", i); \
						m_items.pop(); \
						lua_pop(L, 1); \
						continue; \
					} \
					item.N = tmp; \
	
				CHECK(name);
				CHECK(tags);
				CHECK(thumbnail);
				item.index = i;

				LuaWrapper::getOptionalField(L, -1, "root_install", &item.root_install);
				LuaWrapper::getOptionalField(L, -1, "version", &item.version);
	
				#undef CHECK
				lua_pop(L, 1);
			}
		});
	}
		
	struct MarketItem {
		MarketItem(IAllocator& allocator)
			: name(allocator)
			, tags(allocator)
			, thumbnail(allocator)
		{}

		String name;
		String tags;
		String thumbnail;
		i32 index = -1;
		u32 version = 0;
		bool root_install = false;
		bool download_tried = false;

		ImTextureID texture = nullptr;
	};

	void makePath(const char* path) {
		FileSystem& fs = m_app.getEngine().getFileSystem();
		StaticString<MAX_PATH> fullpath(fs.getBasePath(), "/", path);
		if (!os::makePath(fullpath)) {
			logError("Failed to create ", fullpath);
		}
	}

	void install(const MarketItem& item, const char* install_path) {
		LuaWrapper::DebugGuard guard(m_state);

		int tt = lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_list_ref);
		int qq = lua_rawgeti(m_state, -1, item.index + 1);
		if (lua_getfield(m_state, -1, "path") == LUA_TSTRING) {
			FileSystem& fs =  m_app.getEngine().getFileSystem();
			StaticString<MAX_PATH> install_path_str = install_path;
			String url(lua_tostring(m_state, -1), m_app.getAllocator());
			m_download_thread.download(url.c_str(), [this, install_path_str, url](const OutputMemoryStream& blob){
				FileSystem& fs = m_app.getEngine().getFileSystem();
				if (Path::hasExtension(url.c_str(), "zip")) {
					makePath(install_path_str);
					if (!extract(blob, install_path_str, fs, m_app.getAllocator())) {
						logError("Failed to extract ", url.c_str(), " to ", install_path_str);
					}
				}
				else {
					if (!fs.saveContentSync(Path(install_path_str), blob)) {
						logError("Failed to save ", url.c_str(), " as ", install_path_str);
					}
				}
			});
		}
		else {
			if (lua_getfield(m_state, -2, "install") == LUA_TFUNCTION) {
				lua_remove(m_state, -2);
				lua_remove(m_state, -2);
				lua_pushstring(m_state, install_path);
				int status = lua_resume(m_state, nullptr, 1);
				if (status != LUA_YIELD && status != LUA_OK) {
					logError(lua_tostring(m_state, -1));
				}
				return;
//				LuaWrapper::pcall(m_state, 1, 0);
			}
			else {
				logError("No path or callback found");
			}
			lua_pop(m_state, 1);
		}
		lua_pop(m_state, 3);
	}

	void processFinishedJobs() {
		while (UniquePtr<DownloadThread::Job> job = m_download_thread.popFinishedJob()) {
			if (!job->canceled) {
				if (job->failed_download) {
					logError("Failed to download ", job->url.c_str());
				}
				else {
					job->finished();
				}
			}
		}
	}

	void onGUI() override {
		processFinishedJobs();
		if (m_app.checkShortcut(m_toggle_ui, true)) m_is_open = !m_is_open;
		if (!m_is_open) return;

		RenderInterface* ri = m_app.getRenderInterface();
		ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Marketplace", &m_is_open)) {
			const u32 to_do = m_download_thread.m_to_do_count;
			if (to_do > 0) {
				m_total_jobs = maximum(m_total_jobs, to_do);
				ImGui::ProgressBar(1 - to_do / (float)m_total_jobs);
			}
			else {
				m_total_jobs = 0;
			}
			if (ImGui::Button("Refresh")) getList(true);
			ImGui::SameLine();
			if (ImGuiEx::IconButton(ICON_FA_TIMES, "Clear filter")) m_filter[0] = '\0';
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##filter", "Filter", m_filter, sizeof(m_filter), ImGuiInputTextFlags_AutoSelectAll);
			FileSystem& fs =  m_app.getEngine().getFileSystem();		
			const float width = ImGui::GetContentRegionAvail().x;
			u32 columns = maximum(1, u32((width + 128) / 256));
			
			u32 i = 0;
			for (MarketItem& item : m_items) {
				if (m_filter[0] && findInsensitive(item.name.c_str(), m_filter) == nullptr && findInsensitive(item.tags.c_str(), m_filter) == nullptr) continue;
				if (!item.texture && !item.download_tried) {
					item.download_tried = true;
					m_download_thread.download(item.thumbnail.c_str(), [this, &item](const OutputMemoryStream& blob){
						int w, h;
						stbi_uc* pixels = stbi_load_from_memory(blob.data(), (int)blob.size(), &w, &h, nullptr, 4);
						if (pixels) {;
							RenderInterface* ri = m_app.getRenderInterface();
							item.texture = ri->createTexture("market_thumbnail", pixels, w, h, gpu::TextureFormat::SRGBA);
							free(pixels);
						}
					});
				}
				
				if (i % columns != 0) ImGui::SameLine();
				ImGui::BeginGroup();
				ImGui::PushID(&item);
				ImVec2 img_size(256, 256);
				ImVec2 tl = ImGui::GetCursorPos();
				if (item.texture) 
					ImGui::Image(item.texture, img_size);
				else 
					ImGuiEx::Rect(img_size.x, img_size.y, 0);
				ImVec2 text_size = ImGui::CalcTextSize(item.name.c_str());
				ImVec2 pos = ImGui::GetCursorPos();
				pos.x += (256 - text_size.x) * 0.5f;
				ImGui::SetCursorPos(pos);
				ImGui::TextUnformatted(item.name.c_str());
				
				ImGui::SetCursorPos(tl);
				if (ImGuiEx::IconButton(ICON_FA_DOWNLOAD, "Download & Install")) {
					if (item.root_install) {
						install(item, "");
					}
					else {
						m_show_install_path = true;
						m_item_to_install = i32(&item - m_items.begin());
					}
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
	
	lua_State* m_state;
	i32 m_list_ref = -1;
	StudioApp& m_app;
	Array<MarketItem> m_items;
	i32 m_item_to_install = -1;
	bool m_show_install_path = false;
	char m_filter[64] = "";
	bool m_is_open = false;
	u32 m_total_jobs = 0;
	Action m_toggle_ui{"Marketplace", "Marketplace", "Toggle UI", "marketplace_toggle_ui", nullptr, Action::WINDOW};
	DownloadThread m_download_thread;
};

struct MarketHelper : StudioApp::IPlugin {
	MarketHelper(StudioApp& app)
		: m_app(app)
	{
		m_plugin.create(app);
	}
	
	const char* getName() const override { return "market"; }
	void init() override {}
	bool showGizmo(struct WorldView& view, ComponentUID) override { return false; }

	StudioApp& m_app;
	Local<MarketPlugin> m_plugin;
};

LUMIX_STUDIO_ENTRY(market) {
	PROFILE_FUNCTION();
	WorldEditor& editor = app.getWorldEditor();

	auto* plugin = LUMIX_NEW(editor.getAllocator(), MarketHelper)(app);
	return plugin;
}

} // namespace Lumix
