#pragma once
#include "ShaderGraph/GraphCompiler.h"
#include "imgui.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class Engine;
namespace ax { namespace NodeEditor { struct EditorContext; } }

// The shader graph editor: a dockable window with the node canvas (imgui-node-editor),
// a details panel (graph settings, the selected node's settings, errors, parameters,
// generated code) and a live material preview. Edits recompile after a short pause and
// show at once on every material using the graph (AssetManager::PreviewShaderGraph);
// Save (Ctrl+S) writes the .shadergraph file. One graph is open at a time.
class ShaderGraphEditor
{
public:
	~ShaderGraphEditor();

	// Opens a .shadergraph (asks first when the open one has unsaved changes).
	void Open(Engine& engine, const std::string& path);
	void Draw(Engine& engine);
	void Shutdown(Engine& engine);

	bool IsOpen() const { return !mPath.empty(); }
	const std::string& Path() const { return mPath; }
	// The graph window had keyboard focus last frame (the scene's hotkeys stand back).
	bool WantsKeyboard() const { return mFocused; }

	// A new graph: a Surface Output with a colour and a roughness parameter.
	static sg::Graph DefaultGraph();

private:
	void Load(Engine& engine, const std::string& path);
	void Close(Engine& engine, bool revert);
	void RequestClose(const std::string& thenOpen);

	void DrawToolbar(Engine& engine);
	void DrawCanvas(Engine& engine);
	void DrawNode(Engine& engine, sg::Node& node, const sg::NodePins& pins);
	void DrawCreatePopup(Engine& engine);
	void DrawDetails(Engine& engine);
	void DrawNodeSettings(Engine& engine, sg::Node& node);
	void DrawPreview(Engine& engine);
	void DrawSavePrompt(Engine& engine);
	void HandleShortcuts(Engine& engine);

	uint32_t AddNode(Engine& engine, const std::string& type, const ImVec2& position);
	void Duplicate(Engine& engine, const std::vector<uint32_t>& nodes);
	bool WouldLoop(uint32_t from, uint32_t to) const;
	std::vector<uint32_t> SelectedNodes() const;

	// Undo: snapshots (graph JSON). A change is committed once nothing is being dragged.
	void Edited(bool recompile = true);
	void CommitIfIdle();
	bool Undo();
	bool Redo();
	void Restore(Engine& engine, const std::string& json);
	void Recompile(Engine& engine);
	bool Save(Engine& engine);

	std::string mPath;
	sg::Graph mGraph;
	ax::NodeEditor::EditorContext* mContext = nullptr;
	std::set<uint32_t> mPlaced;            // nodes whose position the canvas has been given
	int mNavigateIn = 0;                   // frames until the view fits the graph
	bool mFocusWindow = false;
	bool mFocused = false;
	bool mDirty = false;                   // unsaved changes
	bool mPending = false;                 // changed since the last undo snapshot
	bool mMoved = false;                   // nodes being dragged
	std::string mDeferredOpen;             // opened at the start of the next Draw
	std::string mBaseline;
	std::vector<std::string> mUndo;
	std::vector<std::string> mRedo;

	float mRecompileIn = -1.0f;
	sg::CompiledGraph mCompiled;
	std::map<uint32_t, std::string> mNodeErrors;

	ImVec2 mCreatePosition{ 0, 0 };        // canvas coordinates of a node being created
	uint64_t mCreateFromPin = 0;           // the pin a link was dragged from (0: none)
	char mSearch[64] = "";
	uint32_t mContextNode = 0;

	bool mPrompt = false;                  // "save changes?" before closing / opening another
	std::string mPromptThenOpen;

	int mPreviewMesh = 0;
	float mPreviewYaw = 0.6f;
	float mPreviewPitch = 0.35f;
};
