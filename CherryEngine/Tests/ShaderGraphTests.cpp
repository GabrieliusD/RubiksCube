// Tests for shader graphs: the graph file format, the compiler's type rules and code, and
// that the generated HLSL compiles with DXC (no GPU needed).
#include <ShaderGraph/GraphCompiler.h>
#include <Renderer/Renderer.h>
#include <cstdio>
#include <map>
#include <string>

using namespace sg;

namespace
{
	int gGraphFailures = 0;

	void Check(bool condition, const std::string& what)
	{
		std::printf("%s  %s\n", condition ? "[pass]" : "[FAIL]", what.c_str());
		if (!condition) ++gGraphFailures;
	}

	uint32_t Add(Graph& graph, const std::string& type, const GraphLoader* loader = nullptr)
	{
		const uint32_t id = graph.nextId++;
		graph.nodes.push_back(MakeNode(type, id, loader));
		return id;
	}

	bool HasErrorOn(const CompiledGraph& compiled, uint32_t node)
	{
		for (const GraphError& error : compiled.errors) if (error.node == node) return true;
		return false;
	}

	std::string Errors(const CompiledGraph& compiled)
	{
		std::string text;
		for (const GraphError& error : compiled.errors) text += "\n      node " + std::to_string(error.node) + ": " + error.message;
		return text;
	}

	// Compiles generated code with DXC; prints the errors when it fails.
	bool CompilesWithDxc(const std::string& name, const CompiledGraph& compiled)
	{
		std::string errors;
		const bool ok = CompileSurfaceShaderOffline(name, compiled.hlsl, CHERRY_TEST_SHADER_DIR, errors);
		if (!ok) std::printf("      DXC: %s\n----\n%s----\n", errors.c_str(), compiled.hlsl.c_str());
		return ok;
	}

	size_t Count(const std::string& text, const std::string& what)
	{
		size_t count = 0;
		for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++count;
		return count;
	}

	// Output input indices.
	enum { BaseColor, Metallic, Roughness, Specular, Normal, Emissive, Opacity, OpacityMask, WorldPositionOffset };
}

int RunShaderGraphTests()
{
	const GraphLoader noLoader;

	// ---- Basics ------------------------------------------------------------------------
	{
		Graph empty;
		const CompiledGraph compiled = CompileGraph(empty, noLoader);
		Check(!compiled.ok && HasErrorOn(compiled, 0), "a graph without Surface Output is an error");
	}
	{
		Graph graph;
		Add(graph, "Output");
		const CompiledGraph compiled = CompileGraph(graph, noLoader);
		Check(compiled.ok, "a lone Surface Output compiles" + Errors(compiled));
		Check(CompilesWithDxc("Lone.shadergraph", compiled), "  ...and its HLSL compiles with DXC");
	}

	// ---- Parameters: layout by name, shared by name, unused ones dropped ----------------------
	{
		Graph graph;
		const uint32_t output = Add(graph, "Output");
		const uint32_t tint = Add(graph, "ColorParameter");
		graph.Find(tint)->name = "Tint";
		const uint32_t texture = Add(graph, "TextureSample");
		graph.Find(texture)->name = "Albedo";
		graph.Find(texture)->asset = "Textures/rock.png";
		const uint32_t multiply = Add(graph, "Multiply");
		const uint32_t rough = Add(graph, "ScalarParameter");
		graph.Find(rough)->name = "Rough";
		graph.Find(rough)->value = { 0.7f, 0, 0, 0 };
		const uint32_t unused = Add(graph, "ScalarParameter");
		graph.Find(unused)->name = "Unused";
		graph.Connect(tint, 1, multiply, 0);       // RGB
		graph.Connect(texture, 1, multiply, 1);    // RGB
		graph.Connect(multiply, 0, output, BaseColor);
		graph.Connect(rough, 0, output, Roughness);

		const CompiledGraph compiled = CompileGraph(graph, noLoader);
		Check(compiled.ok, "a textured, tinted graph compiles" + Errors(compiled));
		const GraphParameter* albedo = FindParameter(compiled, "Albedo");
		const GraphParameter* roughParam = FindParameter(compiled, "Rough");
		const GraphParameter* tintParam = FindParameter(compiled, "Tint");
		Check(compiled.parameters.size() == 3 && !FindParameter(compiled, "Unused"), "only parameters that reach the output are kept");
		Check(albedo && albedo->kind == ParamKind::Texture && albedo->offset == 0 && albedo->defaultTexture == "Textures/rock.png",
			"a Texture Sample is a texture parameter with its default asset");
		Check(roughParam && tintParam && roughParam->offset == 0 && tintParam->offset == 1 && compiled.paramCount == 5 && compiled.textureCount == 1,
			"parameters are laid out by name (Rough at 0, Tint at 1..4)");
		Check(roughParam && roughParam->defaultValue[0] == 0.7f, "a parameter keeps its default value");
		Check(compiled.hlsl.find("ParamFloat(input, 0)") != std::string::npos && compiled.hlsl.find("ParamFloat4(input, 1)") != std::string::npos,
			"the code reads parameters at their offsets");
		Check(CompilesWithDxc("Textured.shadergraph", compiled), "  ...and its HLSL compiles with DXC");

		// Moving nodes around doesn't change the layout.
		std::swap(graph.nodes[1], graph.nodes[4]);
		const CompiledGraph reordered = CompileGraph(graph, noLoader);
		Check(FindParameter(reordered, "Tint") && FindParameter(reordered, "Tint")->offset == 1, "the layout doesn't depend on node order");

		// Two nodes with one name share the parameter; different kinds under one name are an error.
		const uint32_t again = Add(graph, "ScalarParameter");
		graph.Find(again)->name = "Rough";
		graph.Connect(again, 0, output, Metallic);
		const CompiledGraph shared = CompileGraph(graph, noLoader);
		Check(shared.ok && shared.parameters.size() == 3, "two parameter nodes with the same name share one parameter");
		graph.Find(again)->type = "ColorParameter";
		graph.Find(again)->name = "Rough";
		const CompiledGraph clash = CompileGraph(graph, noLoader);
		Check(!clash.ok && (HasErrorOn(clash, again) || HasErrorOn(clash, rough)), "one name used as a Scalar and a Color is an error on the node");

		// JSON round trip.
		const std::string json = GraphToJson(graph);
		Graph loaded;
		std::string error;
		Check(GraphFromJson(json, loaded, error) && GraphToJson(loaded) == json, "a graph survives a JSON round trip");
		Check(loaded.nextId == graph.nextId && loaded.links.size() == graph.links.size(), "  ...with its links and next id");
	}

	// ---- Type rules --------------------------------------------------------------------------
	{
		Graph graph;
		const uint32_t output = Add(graph, "Output");
		const uint32_t v2 = Add(graph, "Vector2");
		graph.Connect(v2, 0, output, BaseColor);
		const CompiledGraph compiled = CompileGraph(graph, noLoader);
		Check(!compiled.ok && HasErrorOn(compiled, output), "a float2 into a float3 input is an error on that node");

		graph.links.clear();
		const uint32_t v4 = Add(graph, "Vector4");
		graph.Connect(v4, 0, output, BaseColor);
		const CompiledGraph narrowed = CompileGraph(graph, noLoader);
		Check(narrowed.ok && narrowed.hlsl.find(".xyz") != std::string::npos, "a float4 into a float3 input keeps xyz");

		graph.links.clear();
		const uint32_t add = Add(graph, "Add");
		const uint32_t v3 = Add(graph, "Vector3");
		graph.Connect(v2, 0, add, 0);
		graph.Connect(v3, 0, add, 1);
		graph.Connect(add, 0, output, Emissive);
		const CompiledGraph mismatch = CompileGraph(graph, noLoader);
		Check(!mismatch.ok && HasErrorOn(mismatch, add), "adding a float2 and a float3 is an error on the Add");

		graph.Disconnect(add, 0);
		const uint32_t f = Add(graph, "Float");
		graph.Connect(f, 0, add, 0);
		const CompiledGraph broadcast = CompileGraph(graph, noLoader);
		Check(broadcast.ok, "adding a float and a float3 broadcasts the float" + Errors(broadcast));
		Check(CompilesWithDxc("Broadcast.shadergraph", broadcast), "  ...and its HLSL compiles with DXC");
	}

	// ---- Loops, shared expressions ---------------------------------------------------------------
	{
		Graph graph;
		const uint32_t output = Add(graph, "Output");
		const uint32_t a = Add(graph, "Add");
		const uint32_t b = Add(graph, "Add");
		graph.Connect(a, 0, b, 0);
		graph.Connect(b, 0, a, 0);
		graph.Connect(a, 0, output, Roughness);
		const CompiledGraph compiled = CompileGraph(graph, noLoader);
		Check(!compiled.ok && (HasErrorOn(compiled, a) || HasErrorOn(compiled, b)), "a loop is an error, not a hang");
	}
	{
		Graph graph;
		const uint32_t output = Add(graph, "Output");
		const uint32_t position = Add(graph, "WorldPosition");
		const uint32_t m1 = Add(graph, "Multiply");
		const uint32_t m2 = Add(graph, "Multiply");
		graph.Find(m1)->inputs[1] = { 2, 0, 0, 0 };
		graph.Find(m2)->inputs[1] = { 2, 0, 0, 0 };
		const uint32_t add = Add(graph, "Add");
		graph.Connect(position, 0, m1, 0);
		graph.Connect(position, 0, m2, 0);
		graph.Connect(m1, 0, add, 0);
		graph.Connect(m2, 0, add, 1);
		graph.Connect(add, 0, output, Emissive);
		const CompiledGraph compiled = CompileGraph(graph, noLoader);
		Check(compiled.ok && Count(compiled.hlsl, "* ((float3)2.0)") == 1, "identical nodes compute their expression once");
	}

	// ---- Subgraphs ----------------------------------------------------------------------------------
	{
		Graph function;
		const uint32_t in = Add(function, "FunctionInput");
		function.Find(in)->name = "X";
		function.Find(in)->option = 3;
		const uint32_t times = Add(function, "Multiply");
		function.Find(times)->inputs[1] = { 2, 0, 0, 0 };
		const uint32_t out = Add(function, "FunctionOutput");
		function.Find(out)->name = "Y";
		function.Find(out)->option = 3;
		function.Connect(in, 0, times, 0);
		function.Connect(times, 0, out, 0);
		Check(function.IsFunction(), "a graph with Function Output and no Surface Output is a function");

		Graph selfUser = function;   // a function that uses itself
		std::map<std::string, const Graph*> files{ { "Functions/Double.shadergraph", &function }, { "Functions/Self.shadergraph", &selfUser } };
		const GraphLoader loader = [&](const std::string& path) -> const Graph*
		{
			auto it = files.find(path);
			return it != files.end() ? it->second : nullptr;
		};

		const CompiledGraph alone = CompileGraph(function, loader);
		Check(alone.ok, "a function graph compiles on its own (as a preview)" + Errors(alone));
		Check(CompilesWithDxc("Double.shadergraph", alone), "  ...and its HLSL compiles with DXC");

		Graph graph;
		const uint32_t output = Add(graph, "Output");
		const uint32_t position = Add(graph, "WorldPosition");
		Node sub = MakeNode("Subgraph", graph.nextId++, nullptr);
		sub.asset = "Functions/Double.shadergraph";
		FitInputs(sub, ResolvePins(sub, &loader));
		const uint32_t subId = sub.id;
		graph.nodes.push_back(sub);
		const NodePins pins = ResolvePins(*graph.Find(subId), &loader);
		Check(pins.inputs.size() == 1 && pins.outputs.size() == 1 && pins.inputs[0].name == "X" && pins.outputs[0].type == PinType::Float3,
			"a Subgraph node takes its pins from the function's inputs and outputs");
		graph.Connect(position, 0, subId, 0);
		graph.Connect(subId, 0, output, Emissive);
		const CompiledGraph compiled = CompileGraph(graph, loader);
		Check(compiled.ok, "a graph using a subgraph compiles" + Errors(compiled));
		Check(compiled.dependencies.size() == 1 && compiled.dependencies[0] == "Functions/Double.shadergraph", "the subgraph is listed as a dependency");
		Check(CompilesWithDxc("UsesDouble.shadergraph", compiled), "  ...and its HLSL compiles with DXC");

		// Two uses of one subgraph get separate code.
		Node sub2 = sub;
		sub2.id = graph.nextId++;
		graph.nodes.push_back(sub2);
		const uint32_t add = Add(graph, "Add");
		graph.Connect(position, 0, sub2.id, 0);
		graph.Connect(subId, 0, add, 0);
		graph.Connect(sub2.id, 0, add, 1);
		graph.Connect(add, 0, output, Emissive);
		const CompiledGraph twice = CompileGraph(graph, loader);
		Check(twice.ok && CompilesWithDxc("UsesDoubleTwice.shadergraph", twice), "a subgraph used twice compiles");

		// A function that uses itself.
		const uint32_t selfNode = Add(selfUser, "Subgraph");
		selfUser.Find(selfNode)->asset = "Functions/Self.shadergraph";
		FitInputs(*selfUser.Find(selfNode), ResolvePins(*selfUser.Find(selfNode), &loader));
		selfUser.Connect(selfNode, 0, out, 0);
		Graph user;
		const uint32_t userOutput = Add(user, "Output");
		const uint32_t userSub = Add(user, "Subgraph");
		user.Find(userSub)->asset = "Functions/Self.shadergraph";
		FitInputs(*user.Find(userSub), ResolvePins(*user.Find(userSub), &loader));
		user.Connect(userSub, 0, userOutput, Emissive);
		const CompiledGraph recursive = CompileGraph(user, loader);
		Check(!recursive.ok && HasErrorOn(recursive, userSub), "a subgraph that uses itself is an error, not a hang");

		Graph missing;
		const uint32_t missingOutput = Add(missing, "Output");
		const uint32_t missingSub = Add(missing, "Subgraph");
		missing.Find(missingSub)->asset = "Functions/Nope.shadergraph";
		missing.Connect(missingSub, 0, missingOutput, Emissive);
		const CompiledGraph missingCompiled = CompileGraph(missing, loader);
		Check(!missingCompiled.ok && HasErrorOn(missingCompiled, missingSub), "a missing subgraph file is an error on its node");
	}

	// ---- Blend modes, World Position Offset ------------------------------------------------------
	{
		Graph graph;
		const uint32_t output = Add(graph, "Output");
		const uint32_t noise = Add(graph, "Noise");
		graph.Connect(noise, 0, output, OpacityMask);
		graph.blend = BlendMode::Masked;
		const CompiledGraph masked = CompileGraph(graph, noLoader);
		Check(masked.ok && masked.hlsl.find("CHERRY_SURFACE_MASKED") != std::string::npos, "a Masked graph turns on clipping");
		Check(CompilesWithDxc("Masked.shadergraph", masked), "  ...and its HLSL compiles with DXC");

		const uint32_t normal = Add(graph, "WorldNormal");
		const uint32_t multiply = Add(graph, "Multiply");
		graph.Connect(normal, 0, multiply, 0);
		graph.Connect(noise, 0, multiply, 1);
		graph.Connect(multiply, 0, output, WorldPositionOffset);
		const CompiledGraph offset = CompileGraph(graph, noLoader);
		Check(offset.ok && offset.vertexOffset && offset.hlsl.find("EvaluateVertexOffset") != std::string::npos,
			"World Position Offset makes a vertex function" + Errors(offset));
		Check(CompilesWithDxc("Offset.shadergraph", offset), "  ...and its HLSL compiles with DXC");

		const uint32_t texture = Add(graph, "TextureSample");
		graph.Connect(texture, 2, multiply, 1);   // R
		const CompiledGraph sampled = CompileGraph(graph, noLoader);
		Check(sampled.ok && sampled.hlsl.find("SampleParamTextureLevel") != std::string::npos, "a texture in World Position Offset samples a mip level");
		Check(CompilesWithDxc("OffsetTexture.shadergraph", sampled), "  ...and its HLSL compiles with DXC");

		const uint32_t screen = Add(graph, "ScreenPosition");
		const uint32_t split = Add(graph, "Split");
		graph.Connect(screen, 0, split, 0);
		graph.Connect(split, 0, multiply, 1);
		const CompiledGraph pixelOnly = CompileGraph(graph, noLoader);
		Check(!pixelOnly.ok && HasErrorOn(pixelOnly, screen), "a pixel-only node in World Position Offset is an error on it");
	}

	// ---- Every node type produces code DXC accepts ---------------------------------------------
	{
		int compiledTypes = 0, failedTypes = 0;
		for (const NodeDef& def : NodeDefs())
		{
			if (def.type == "Output" || def.type == "FunctionOutput" || def.type == "Subgraph") continue;
			Graph graph;
			const uint32_t output = Add(graph, "Output");
			const uint32_t node = Add(graph, def.type);
			if (def.type == "Swizzle") graph.Find(node)->text = "xx";   // the unconnected input is a float
			// Reduce output 0 to a float with Dot(out, out), whatever its width.
			const uint32_t dot = Add(graph, "Dot");
			graph.Connect(node, 0, dot, 0);
			graph.Connect(node, 0, dot, 1);
			graph.Connect(dot, 0, output, Roughness);
			const CompiledGraph compiled = CompileGraph(graph, noLoader);
			const bool ok = compiled.ok && CompilesWithDxc(def.type + ".shadergraph", compiled);
			if (!ok) std::printf("      %s failed%s\n", def.type.c_str(), Errors(compiled).c_str());
			ok ? ++compiledTypes : ++failedTypes;
		}
		Check(failedTypes == 0, "every node type compiles through DXC (" + std::to_string(compiledTypes) + " types)");
	}

	return gGraphFailures;
}
