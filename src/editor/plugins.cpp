#define LUMIX_NO_CUSTOM_CRT
#include "editor/studio_app.h"
#include "editor/world_editor.h"
#include "engine/allocator.h"

#include "imgui/imgui.h"


using namespace Lumix;

struct EditorPlugin : StudioApp::GUIPlugin {
	EditorPlugin(StudioApp& app) : m_app(app) {}

	void onWindowGUI() override {
		ImGui::SetNextWindowSize(ImVec2(200, 200), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("My plugin")) {
			ImGui::TextUnformatted("Hello world");
			ImGui::DragFloat("Some value", &m_some_value);
		}
		ImGui::End();
	}
	
	const char* getName() const override { return "myplugin"; }

	StudioApp& m_app;
	float m_some_value = 0;
};


LUMIX_STUDIO_ENTRY(myplugin)
{
	WorldEditor& editor = app.getWorldEditor();

	auto* plugin = LUMIX_NEW(editor.getAllocator(), EditorPlugin)(app);
	app.addPlugin(*plugin);
	return nullptr;
}
