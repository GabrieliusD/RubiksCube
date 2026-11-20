#pragma once
#include "CherryAPI.h"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Shader graphs: node graphs that compile to a surface shader (see GraphCompiler.h).
// A .shadergraph file in Content is a Graph saved as JSON:
//
//   { "format": "cherry-shadergraph", "version": 1, "blend": "Opaque", "boundsPadding": 0,
//     "nodes": [ { "id": 1, "type": "Output", "pos": [400, 0], "inputs": [[0.8, 0.8, 0.8, 0], ...] }, ... ],
//     "links": [ [fromNode, fromOutput, toNode, toInput], ... ] }
//
// Nodes are identified by id, pins by their index in the node type's input/output list
// (NodeLibrary.h). A graph with Function Output nodes and no Output node is a subgraph:
// other graphs use it through a Subgraph node.
namespace sg
{
	using Float4 = std::array<float, 4>;

	enum class BlendMode : uint8_t
	{
		Opaque,       // writes depth, no blending
		Masked,       // opaque, but pixels whose Opacity Mask is below 1/3 are discarded
		Translucent,  // drawn after opaque surfaces, back to front, blended by Opacity
	};

	struct Node
	{
		uint32_t id = 0;
		std::string type;              // NodeDef::type
		float x = 0.0f, y = 0.0f;      // position in the editor
		// Per input: the constant used while nothing is connected to it.
		std::vector<Float4> inputs;

		// Settings; what they mean depends on the node type (see NodeLibrary.cpp).
		std::string name;              // parameter / function pin name
		std::string text;              // swizzle mask, custom HLSL code
		std::string asset;             // default texture, or the subgraph's path
		int option = 0;                // sampler, component count, octaves ...
		int option2 = 0;               // texture kind ...
		Float4 value{ 0, 0, 0, 0 };    // constant value / parameter default
	};

	struct Link
	{
		uint32_t fromNode = 0;
		uint32_t fromPin = 0;          // output index
		uint32_t toNode = 0;
		uint32_t toPin = 0;            // input index
	};

	struct CHERRY_API Graph
	{
		std::vector<Node> nodes;
		std::vector<Link> links;
		BlendMode blend = BlendMode::Opaque;
		float boundsPadding = 0.0f;    // metres added to mesh bounds (for World Position Offset)
		uint32_t nextId = 1;

		Node* Find(uint32_t id);
		const Node* Find(uint32_t id) const;
		// The link into a node's input, or nullptr.
		const Link* InputLink(uint32_t node, uint32_t input) const;
		// Connects an output to an input, replacing whatever was connected to that input.
		void Connect(uint32_t fromNode, uint32_t fromPin, uint32_t toNode, uint32_t toPin);
		void Disconnect(uint32_t toNode, uint32_t toPin);
		// Removes a node and its links.
		void RemoveNode(uint32_t id);
		bool IsFunction() const;       // has Function Output nodes and no Output node
	};

	// Loads another graph by asset path (for Subgraph nodes); nullptr if it can't.
	using GraphLoader = std::function<const Graph*(const std::string& path)>;

	CHERRY_API std::string GraphToJson(const Graph& graph);
	CHERRY_API bool GraphFromJson(const std::string& json, Graph& graph, std::string& error);

	CHERRY_API const char* BlendModeName(BlendMode mode);
}
