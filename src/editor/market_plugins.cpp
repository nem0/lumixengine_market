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
#include "engine/sync.h"
#include "engine/thread.h"
#include "openfbx/miniz.h"

#include "imgui/imgui.h"
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
		while(!m_finished) {
			m_semaphore.wait();
			m_mutex.enter();
			if (!m_jobs.empty()) {
				Job* job = m_jobs.back();
				m_current_job = job;
				m_jobs.pop();
				const bool canceled = job->canceled;
				m_mutex.exit();
				if (!canceled) {
					job->failed_download = !download(job->url.c_str(), job->blob, job->use_cache, m_app.getEngine().getFileSystem());
				}
				m_mutex.enter();
				m_current_job = nullptr;
				m_finished_jobs.emplace(job);
			}
			m_mutex.exit();
		}
		return 0;
	}
	
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
		for (Job* job : m_jobs) {
			job->canceled = true;
		}
		for (Job* job : m_finished_jobs) {
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

		MyJob* job = LUMIX_NEW(m_app.getAllocator(), MyJob)(f, m_app.getAllocator());
		job->url = url;
		job->use_cache = true;
		pushJob(job);
	}

	void pushJob(Job* job) {
		++m_to_do_count;
		MutexGuard guard(m_mutex);
		m_jobs.push(job);
		m_semaphore.signal();
	}

	Job* popFinishedJob() {
		MutexGuard guard(m_mutex);
		if (m_finished_jobs.empty()) return nullptr;
		--m_to_do_count;
		Job* job = m_finished_jobs.back();
		m_finished_jobs.pop();
		return job;
	}

	StudioApp& m_app;
	Semaphore m_semaphore;
	Mutex m_mutex;
	Array<Job*> m_jobs;
	Array<Job*> m_finished_jobs;
	Job* m_current_job = nullptr;
	bool m_finished = false;
	u32 m_to_do_count = 0;
};

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
		, m_download_thread(app, app.getAllocator())
	{
		m_toggle_ui.init("Marketplace", "Toggle marketplace UI", "marketplace", "", false);
		m_toggle_ui.func.bind<&MarketPlugin::toggleUI>(this);
		m_toggle_ui.is_selected.bind<&MarketPlugin::isOpen>(this);

		const char* base_path = m_app.getEngine().getFileSystem().getBasePath();
		if (!os::makePath(StaticString<LUMIX_MAX_PATH>(base_path, "/.market_cache"))) {
			logError("Failed to create .market_cache");
		}
		getList(false);

		m_app.addWindowAction(&m_toggle_ui);
		m_download_thread.create("market_download", true);
	}

	~MarketPlugin() {
		m_app.removeAction(&m_toggle_ui);
	}

	void toggleUI() { m_is_open = !m_is_open; }
	bool isOpen() const { return m_is_open; }

	void getList(bool force) {
		if (force) {
			FileSystem& fs = m_app.getEngine().getFileSystem();
			const StableHash url_hash(LIST_URL);
			fs.deleteFile(StaticString<LUMIX_MAX_PATH>(".market_cache/", url_hash.getHashValue(), ".lua"));
		}
		m_download_thread.cancelAll();
		m_items.clear();
		m_download_thread.download(LIST_URL, [&](const OutputMemoryStream& blob){
			lua_State* L = luaL_newstate();
			OutputMemoryStream tmp_blob(m_app.getAllocator());
			tmp_blob << "return ";
			tmp_blob.write(blob.data(), blob.size());
			if (!LuaWrapper::execute(L, Span((const char*)tmp_blob.data(), (u32)tmp_blob.size()), "market list", 1)) {
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
		});
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
		bool download_tried = false;

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
		StaticString<LUMIX_MAX_PATH> install_path_str = install_path;
		m_download_thread.download(item.path.c_str(), [this, item, install_path_str](const OutputMemoryStream& blob){
			FileSystem& fs = m_app.getEngine().getFileSystem();
			if (Path::hasExtension(item.path.c_str(), "zip")) {
				makePath(install_path_str);
				if (!extract(blob, install_path_str, fs, m_app.getAllocator())) {
					logError("Failed to extract ", item.path.c_str(), " to ", install_path_str);
				}
			}
			else {
				if (!fs.saveContentSync(Path(install_path_str), blob)) {
					logError("Failed to save ", item.path.c_str(), " as ", install_path_str);
				}
			}
		});
	}

	void onSettingsLoaded() override {
		m_is_open = m_app.getSettings().getValue(Settings::GLOBAL, "is_marketplace_open", false);
	}

	void onBeforeSettingsSaved() override {
		m_app.getSettings().setValue(Settings::GLOBAL, "is_marketplace_open", m_is_open);
	}

	void processFinishedJobs() {
		while (DownloadThread::Job* job = m_download_thread.popFinishedJob()) {
			if (!job->canceled) {
				if (job->failed_download) {
					logError("Failed to download ", job->url.c_str());
				}
				else {
					job->finished();
				}
			}
			LUMIX_DELETE(m_app.getAllocator(), job);
		}
	}

	void onWindowGUI() override {
		processFinishedJobs();
		if (!m_is_open) return;

		RenderInterface* ri = m_app.getRenderInterface();
		ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Marketplace", &m_is_open)) {
			const u32 to_do = m_download_thread.m_to_do_count;
			if (to_do > 0) {
				m_total_jobs = maximum(m_total_jobs, to_do);
				ImGui::ProgressBar(to_do / (float)m_total_jobs);
			}
			else {
				m_total_jobs = 0;
			}
			if (ImGui::Button("Refresh")) getList(true);
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##filter", "Filter", m_filter, sizeof(m_filter));
			FileSystem& fs =  m_app.getEngine().getFileSystem();		
			const float width = ImGui::GetContentRegionAvail().x;
			u32 columns = maximum(1, u32((width + 128) / 256));
			
			u32 i = 0;
			for (MarketItem& item : m_items) {
				if (m_filter[0] && stristr(item.name.c_str(), m_filter) == nullptr) continue;
				if (!item.texture && !item.download_tried) {
					item.download_tried = true;
					m_download_thread.download(item.thumbnail.c_str(), [this, &item](const OutputMemoryStream& blob){
						int w, h;
						stbi_uc* pixels = stbi_load_from_memory(blob.data(), (int)blob.size(), &w, &h, nullptr, 4);
						if (pixels) {;
							RenderInterface* ri = m_app.getRenderInterface();
							item.texture = ri->createTexture("market_thumbnail", pixels, w, h);;
							free(pixels);
						}
					});
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
	u32 m_total_jobs = 0;
	Action m_toggle_ui;
	DownloadThread m_download_thread;
};

LUMIX_STUDIO_ENTRY(market)
{
	WorldEditor& editor = app.getWorldEditor();

	auto* plugin = LUMIX_NEW(editor.getAllocator(), MarketPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}

} // namespace Lumix
