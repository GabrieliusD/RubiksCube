#pragma once
#include "ShaderGraph/ShaderGraph.h"
#include "ShaderGraph/NodeLibrary.h"
#include <string>
#include <vector>

// Compiles a shader graph into a surface shader: HLSL defining EvaluateSurface (and
// EvaluateVertexOffset when World Position Offset is connected) for
// Renderer::CreateSurfaceShader, plus the layout of the material parameter block.
//
// The compiler walks back from the Surface Output node, so nodes that don't reach it cost
// nothing. Each node output becomes one local variable; identical expressions share one.
// Subgraphs are inlined. Type rules: a float1 converts to any width (broadcast), a wider
// value converts to a narrower one (it keeps the first components), and anything else is
// an error on the node.
namespace sg
{
	struct GraphError
	{
		uint32_t node = 0;        // 0 = the graph as a whole
		std::string message;
	};

	// A material parameter: a Scalar/Vector/Color Parameter or Texture Sample node.
	// Nodes with the same name share one parameter.
	struct GraphParameter
	{
		std::string name;
		ParamKind kind = ParamKind::Scalar;
		uint32_t offset = 0;              // float index in MaterialDesc::params, or texture slot
		Float4 defaultValue{ 0, 0, 0, 0 };
		std::string defaultTexture;       // texture asset path
		bool linearTexture = false;       // normal map / data: sample without sRGB decoding
	};

	struct CompiledGraph
	{
		bool ok = false;
		std::string hlsl;
		std::vector<GraphError> errors;
		std::vector<GraphParameter> parameters;   // in name order
		uint32_t paramCount = 0;                  // floats in the parameter block
		uint32_t textureCount = 0;                // texture slots
		BlendMode blend = BlendMode::Opaque;
		float boundsPadding = 0.0f;
		bool vertexOffset = false;
		std::vector<std::string> dependencies;    // subgraph assets used (reload when they change)
	};

	CHERRY_API CompiledGraph CompileGraph(const Graph& graph, const GraphLoader& loader);

	CHERRY_API const GraphParameter* FindParameter(const CompiledGraph& compiled, const std::string& name);
}
