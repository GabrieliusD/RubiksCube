#pragma once
#include "ShaderGraph/ShaderGraph.h"
#include <functional>
#include <string>
#include <vector>

// The node types a shader graph can use: their pins, settings and the HLSL they emit.
namespace sg
{
	enum class PinType : uint8_t
	{
		Float1 = 1, Float2 = 2, Float3 = 3, Float4 = 4,
		Dynamic,   // float1..4: the widest Dynamic input of the node (float1 inputs broadcast)
		Any,       // keeps whatever width is connected (Append, Split, Swizzle, Custom)
	};

	// Width of a fixed pin type (0 for Dynamic / Any).
	inline int PinWidth(PinType type) { return type <= PinType::Float4 ? static_cast<int>(type) : 0; }

	struct PinInfo
	{
		std::string name;
		PinType type = PinType::Float1;
		Float4 constant{ 0, 0, 0, 0 };     // the default constant of an unconnected input
		const char* defaultExpr = nullptr;   // unconnected input uses this HLSL instead (e.g. "input.uv")
	};

	struct NodePins
	{
		std::vector<PinInfo> inputs;
		std::vector<PinInfo> outputs;
	};

	// Which of a node's settings the editor shows.
	enum NodeSetting : uint32_t
	{
		Setting_Name        = 1 << 0,   // Node::name (parameter / function pin name)
		Setting_Value       = 1 << 1,   // Node::value, edited as NodeDef::valueEdit
		Setting_Text        = 1 << 2,   // Node::text, one line (swizzle mask)
		Setting_Code        = 1 << 3,   // Node::text, multi-line HLSL
		Setting_Texture     = 1 << 4,   // Node::asset, a texture
		Setting_Sampler     = 1 << 5,   // Node::option, kSamplerNames
		Setting_TextureKind = 1 << 6,   // Node::option2, kTextureKindNames
		Setting_Width       = 1 << 7,   // Node::option, 1..4 components
		Setting_Octaves     = 1 << 8,   // Node::option, 1..8
		Setting_Subgraph    = 1 << 9,   // Node::asset, a .shadergraph function
	};

	enum class ValueEdit : uint8_t { None, Float1, Float2, Float3, Float4, Color };

	enum class ParamKind : uint8_t { Scalar, Vector, Color, Texture };

	// What a node's emit function works with. Input expressions are already converted to
	// their pins' types; the function fills `out` with one expression per output.
	struct EmitContext
	{
		const Node* node = nullptr;
		std::vector<std::string> in;
		std::vector<int> inWidth;
		int width = 1;                        // width of the node's Dynamic pins
		bool vertexStage = false;             // emitting World Position Offset code
		std::vector<std::string> out;
		std::vector<int> outWidth;            // preset from the pins; Any outputs must be set
		std::string error;                    // set to fail the node
		std::string uid;                      // unique per node instance (for helper names)

		// Declares a local variable holding `expr` and returns its name.
		std::function<std::string(int width, const std::string& expr)> temp;
		// Registers this node as a material parameter; returns the HLSL for its offset/slot.
		std::function<std::string(ParamKind kind)> param;
		// Adds a helper function to the shader (once per key).
		std::function<void(const std::string& key, const std::string& code)> helper;
	};

	struct NodeDef
	{
		std::string type;                     // saved in the graph file
		std::string title;
		std::string category;
		std::string description;
		NodePins pins;                        // Subgraph / Function nodes: see ResolvePins
		uint32_t settings = 0;                // NodeSetting bits
		ValueEdit valueEdit = ValueEdit::None;
		Float4 defaultValue{ 0, 0, 0, 0 };
		int defaultOption = 0;
		std::string defaultName;
		std::string defaultText;
		bool pixelOnly = false;               // not available for World Position Offset
		std::function<void(EmitContext&)> emit;   // null for nodes the compiler handles itself
	};

	extern const char* const kSamplerNames[6];      // UI names, Node::option of Texture Sample
	// Node::option2 of Texture Sample: 0 colour (sRGB), 1 normal map with +Y green (glTF, OpenGL,
	// Blender), 2 linear data, 3 normal map with -Y green (DirectX, Unreal). Normal maps come out
	// as tangent-space normals in -1..1 for the Normal Map node.
	constexpr int kTextureKindCount = 4;
	extern const char* const kTextureKindNames[kTextureKindCount];

	CHERRY_API const std::vector<NodeDef>& NodeDefs();
	CHERRY_API const NodeDef* FindNodeDef(const std::string& type);

	// A subgraph's interface: its Function Input / Function Output nodes, top to bottom.
	struct FunctionPin
	{
		uint32_t node = 0;
		std::string name;
		int width = 1;
		Float4 defaultValue{ 0, 0, 0, 0 };
	};
	CHERRY_API void FunctionSignature(const Graph& graph, std::vector<FunctionPin>& inputs, std::vector<FunctionPin>& outputs);

	// The pins of one node: its type's, or for Subgraph / Function nodes the ones its settings
	// and the graph it references give. Unknown types have none.
	CHERRY_API NodePins ResolvePins(const Node& node, const GraphLoader* loader);
	// A new node of `type` with default settings and input constants.
	CHERRY_API Node MakeNode(const std::string& type, uint32_t id, const GraphLoader* loader);
	// Makes node.inputs match the pins (new inputs get the pins' default constants).
	CHERRY_API void FitInputs(Node& node, const NodePins& pins);
}
