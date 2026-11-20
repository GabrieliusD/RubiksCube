#include "ShaderGraph/GraphCompiler.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

namespace sg
{
	namespace
	{
		std::string T(int width) { return width <= 1 ? "float" : "float" + std::to_string(width); }

		std::string Number(float f)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.9g", f);
			std::string s = buffer;
			if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
			return s;
		}

		std::string Literal(const Float4& v, int width)
		{
			if (width <= 1) return Number(v[0]);
			std::string s = T(width) + "(";
			for (int i = 0; i < width; ++i) s += (i ? ", " : "") + Number(v[i]);
			return s + ")";
		}

		const char* KindName(ParamKind kind)
		{
			switch (kind)
			{
			case ParamKind::Vector: return "Vector";
			case ParamKind::Color: return "Color";
			case ParamKind::Texture: return "Texture";
			default: return "Scalar";
			}
		}

		struct Value
		{
			std::string expr;
			int width = 1;
		};

		class Compiler
		{
		public:
			explicit Compiler(const GraphLoader& loader) : mLoader(loader) {}
			CompiledGraph Run(const Graph& graph);

		private:
			struct Instance
			{
				const Graph* graph = nullptr;
				std::string prefix;                       // "" at the top, "s5_" inside subgraph node 5 ...
				const std::vector<Value>* args = nullptr; // a subgraph's Function Input values
				uint32_t rootNode = 0;                    // top-level node errors are reported on (0: the node itself)
				std::string path;                         // the subgraph's asset (for messages)
				int depth = 0;
			};

			void Error(const Instance& inst, uint32_t node, const std::string& message);
			std::string Declare(int width, const std::string& expr, const std::string& name);
			std::string Coerce(const Instance& inst, const Node& node, const Value& value, int width, const std::string& what);
			Value Input(const Instance& inst, const Node& node, uint32_t index, const PinInfo& pin);
			const std::vector<Value>& Eval(const Instance& inst, const Node& node);
			std::vector<Value> EvalSubgraph(const Instance& inst, const Node& node, const NodePins& pins);
			std::string Param(const Instance& inst, const Node& node, ParamKind kind);
			void BeginFunction(bool vertex);

			const GraphLoader& mLoader;
			CompiledGraph mResult;
			std::set<std::string> mErrorKeys;

			// Per generated function.
			std::string mBody;
			bool mVertex = false;
			std::map<std::string, std::vector<Value>> mDone;
			std::set<std::string> mVisiting;
			std::map<std::string, std::string> mCse;
			int mTemp = 0;

			// Shared by both functions.
			std::vector<GraphParameter> mParams;      // registration order; @P<index>@ in the code
			std::vector<std::string> mHelpers;
			std::set<std::string> mHelperKeys;
			std::deque<std::vector<Value>> mArgs;     // subgraph arguments (stable addresses)
		};

		void Compiler::Error(const Instance& inst, uint32_t node, const std::string& message)
		{
			GraphError error;
			error.node = inst.rootNode ? inst.rootNode : node;
			error.message = inst.rootNode ? "In " + inst.path + ": " + message : message;
			if (mErrorKeys.insert(std::to_string(error.node) + error.message).second) mResult.errors.push_back(error);
		}

		void Compiler::BeginFunction(bool vertex)
		{
			mVertex = vertex;
			mBody.clear();
			mDone.clear();
			mVisiting.clear();
			mCse.clear();
		}

		std::string Compiler::Declare(int width, const std::string& expr, const std::string& name)
		{
			const std::string key = std::to_string(width) + "|" + expr;
			auto it = mCse.find(key);
			if (it != mCse.end()) return it->second;
			mBody += "\t" + T(width) + " " + name + " = " + expr + ";\n";
			mCse[key] = name;
			return name;
		}

		std::string Compiler::Coerce(const Instance& inst, const Node& node, const Value& value, int width, const std::string& what)
		{
			if (value.width == width) return value.expr;
			if (value.width == 1) return "((" + T(width) + ")" + value.expr + ")";
			if (value.width > width) return value.expr + "." + std::string("xyzw").substr(0, width);
			Error(inst, node.id, what + ": a " + T(value.width) + " is connected where a " + T(width) + " is needed");
			return Literal({ 0, 0, 0, 0 }, width);
		}

		Value Compiler::Input(const Instance& inst, const Node& node, uint32_t index, const PinInfo& pin)
		{
			const int fixedWidth = PinWidth(pin.type);
			if (const Link* link = inst.graph->InputLink(node.id, index))
			{
				if (const Node* from = inst.graph->Find(link->fromNode))
				{
					const std::vector<Value>& outputs = Eval(inst, *from);
					if (link->fromPin < outputs.size()) return outputs[link->fromPin];
					Error(inst, node.id, pin.name + " is connected to an output that no longer exists");
				}
			}
			if (pin.defaultExpr) return { pin.defaultExpr, fixedWidth ? fixedWidth : 1 };
			const Float4 constant = index < node.inputs.size() ? node.inputs[index] : pin.constant;
			const int width = fixedWidth ? fixedWidth : 1;
			return { Literal(constant, width), width };
		}

		std::string Compiler::Param(const Instance& inst, const Node& node, ParamKind kind)
		{
			if (node.name.empty())
			{
				Error(inst, node.id, "Parameters need a name");
			}
			for (size_t i = 0; i < mParams.size(); ++i)
			{
				if (mParams[i].name != node.name) continue;
				if (mParams[i].kind != kind)
				{
					Error(inst, node.id, "Parameter \"" + node.name + "\" is used as both a " + KindName(mParams[i].kind) + " and a " + KindName(kind));
				}
				return "@P" + std::to_string(i) + "@";
			}
			GraphParameter param;
			param.name = node.name;
			param.kind = kind;
			param.defaultValue = node.value;
			if (kind == ParamKind::Texture)
			{
				param.defaultTexture = node.asset;
				param.linearTexture = node.option2 != 0;
			}
			mParams.push_back(param);
			return "@P" + std::to_string(mParams.size() - 1) + "@";
		}

		const std::vector<Value>& Compiler::Eval(const Instance& inst, const Node& node)
		{
			const std::string key = inst.prefix + std::to_string(node.id);
			auto done = mDone.find(key);
			if (done != mDone.end()) return done->second;

			const NodeDef* def = FindNodeDef(node.type);
			const NodePins pins = ResolvePins(node, &mLoader);
			std::vector<Value> outputs(pins.outputs.size());
			for (size_t i = 0; i < outputs.size(); ++i)
			{
				const int width = PinWidth(pins.outputs[i].type);
				outputs[i] = { Literal({ 0, 0, 0, 0 }, width ? width : 1), width ? width : 1 };
			}

			if (mVisiting.count(key))
			{
				Error(inst, node.id, "This node is part of a loop (its output feeds back into its inputs)");
				return mDone[key] = outputs;
			}
			if (!def)
			{
				Error(inst, node.id, "Unknown node type \"" + node.type + "\"");
				return mDone[key] = outputs;
			}
			if (mVertex && def->pixelOnly)
			{
				Error(inst, node.id, def->title + " can't be used for World Position Offset");
				return mDone[key] = outputs;
			}
			mVisiting.insert(key);

			if (node.type == "FunctionInput")
			{
				std::vector<FunctionPin> inputs, outs;
				FunctionSignature(*inst.graph, inputs, outs);
				const int width = std::clamp(node.option, 1, 4);
				Value value{ Literal(node.value, width), width };
				for (size_t i = 0; i < inputs.size(); ++i)
				{
					if (inputs[i].node == node.id && inst.args && i < inst.args->size()) value = (*inst.args)[i];
				}
				outputs[0] = { Declare(width, value.expr, inst.prefix + "n" + std::to_string(node.id) + "_0"), width };
			}
			else if (node.type == "Subgraph")
			{
				outputs = EvalSubgraph(inst, node, pins);
			}
			else if (def->emit)
			{
				EmitContext c;
				c.node = &node;
				c.vertexStage = mVertex;
				c.uid = inst.prefix + std::to_string(node.id);

				std::vector<Value> raw;
				for (uint32_t i = 0; i < pins.inputs.size(); ++i) raw.push_back(Input(inst, node, i, pins.inputs[i]));

				// Width of the Dynamic pins: the widest Dynamic input; others must match or be float1.
				int width = 1;
				for (size_t i = 0; i < raw.size(); ++i) if (pins.inputs[i].type == PinType::Dynamic) width = std::max(width, raw[i].width);
				for (size_t i = 0; i < raw.size(); ++i)
				{
					if (pins.inputs[i].type == PinType::Dynamic && raw[i].width != 1 && raw[i].width != width)
					{
						Error(inst, node.id, "Inputs of different widths (" + T(raw[i].width) + " and " + T(width) + "): connect matching widths, or a float");
					}
				}
				c.width = width;
				for (size_t i = 0; i < raw.size(); ++i)
				{
					const PinInfo& pin = pins.inputs[i];
					if (pin.type == PinType::Any)
					{
						c.in.push_back(raw[i].expr);
						c.inWidth.push_back(raw[i].width);
						continue;
					}
					const int target = pin.type == PinType::Dynamic ? width : PinWidth(pin.type);
					c.in.push_back(Coerce(inst, node, raw[i], target, pin.name));
					c.inWidth.push_back(target);
				}
				c.out.resize(pins.outputs.size());
				for (const PinInfo& pin : pins.outputs)
				{
					c.outWidth.push_back(pin.type == PinType::Dynamic ? width : PinWidth(pin.type));
				}
				c.temp = [&](int w, const std::string& expr)
				{
					return Declare(w, expr, inst.prefix + "t" + std::to_string(node.id) + "_" + std::to_string(mTemp++));
				};
				c.param = [&](ParamKind kind) { return Param(inst, node, kind); };
				c.helper = [&](const std::string& helperKey, const std::string& code)
				{
					if (mHelperKeys.insert(helperKey).second) mHelpers.push_back(code);
				};

				def->emit(c);
				if (!c.error.empty())
				{
					Error(inst, node.id, c.error);
				}
				else
				{
					for (size_t i = 0; i < outputs.size(); ++i)
					{
						const int w = std::clamp(c.outWidth[i], 1, 4);
						if (c.out[i].empty()) continue;
						outputs[i] = { Declare(w, c.out[i], inst.prefix + "n" + std::to_string(node.id) + "_" + std::to_string(i)), w };
					}
				}
			}

			mVisiting.erase(key);
			return mDone[key] = outputs;
		}

		std::vector<Value> Compiler::EvalSubgraph(const Instance& inst, const Node& node, const NodePins& pins)
		{
			std::vector<Value> outputs(pins.outputs.size());
			for (size_t i = 0; i < outputs.size(); ++i)
			{
				const int width = PinWidth(pins.outputs[i].type);
				outputs[i] = { Literal({ 0, 0, 0, 0 }, width), width };
			}
			if (node.asset.empty())
			{
				Error(inst, node.id, "Subgraph: choose a .shadergraph");
				return outputs;
			}
			const Graph* function = mLoader ? mLoader(node.asset) : nullptr;
			if (!function)
			{
				Error(inst, node.id, "Subgraph: can't load " + node.asset);
				return outputs;
			}
			if (!function->IsFunction())
			{
				Error(inst, node.id, "Subgraph: " + node.asset + " has no Function Output node (or has a Surface Output)");
				return outputs;
			}
			if (inst.depth >= 8)
			{
				Error(inst, node.id, "Subgraphs nested too deep (does one use itself?)");
				return outputs;
			}
			if (std::find(mResult.dependencies.begin(), mResult.dependencies.end(), node.asset) == mResult.dependencies.end())
			{
				mResult.dependencies.push_back(node.asset);
			}

			std::vector<FunctionPin> inputs, outs;
			FunctionSignature(*function, inputs, outs);
			std::vector<Value>& args = mArgs.emplace_back();
			for (uint32_t i = 0; i < inputs.size() && i < pins.inputs.size(); ++i)
			{
				const Value raw = Input(inst, node, i, pins.inputs[i]);
				args.push_back({ Coerce(inst, node, raw, inputs[i].width, inputs[i].name), inputs[i].width });
			}

			Instance child;
			child.graph = function;
			child.prefix = inst.prefix + "s" + std::to_string(node.id) + "_";
			child.args = &args;
			child.rootNode = inst.rootNode ? inst.rootNode : node.id;
			child.path = inst.rootNode ? inst.path : node.asset;
			child.depth = inst.depth + 1;

			for (size_t i = 0; i < outs.size() && i < outputs.size(); ++i)
			{
				const Node* out = function->Find(outs[i].node);
				PinInfo pin;
				pin.name = outs[i].name;
				pin.type = static_cast<PinType>(outs[i].width);
				const Value value = Input(child, *out, 0, pin);
				const std::string expr = Coerce(child, *out, value, outs[i].width, outs[i].name);
				outputs[i] = { Declare(outs[i].width, expr, inst.prefix + "n" + std::to_string(node.id) + "_" + std::to_string(i)), outs[i].width };
			}
			return outputs;
		}

		CompiledGraph Compiler::Run(const Graph& graph)
		{
			mResult.blend = graph.blend;
			mResult.boundsPadding = graph.boundsPadding;

			const Node* output = nullptr;
			for (const Node& node : graph.nodes)
			{
				if (node.type != "Output") continue;
				if (output)
				{
					mResult.errors.push_back({ node.id, "Only one Surface Output node is used; delete the others" });
					continue;
				}
				output = &node;
			}

			Instance top;
			top.graph = &graph;
			const NodePins outputPins = output ? ResolvePins(*output, &mLoader) : NodePins{};

			// ---- Pixel: EvaluateSurface -----------------------------------------------------
			BeginFunction(false);
			std::string assign;
			if (output)
			{
				const char* fields[] = { "baseColor", "metallic", "roughness", "specular", "normalW", "emissive", "alpha", "opacityMask" };
				for (uint32_t i = 0; i < 8; ++i)
				{
					const PinInfo& pin = outputPins.inputs[i];
					const Value value = Input(top, *output, i, pin);
					assign += std::string("\ts.") + fields[i] + " = " + Coerce(top, *output, value, PinWidth(pin.type), pin.name) + ";\n";
				}
			}
			else if (graph.IsFunction())
			{
				// A subgraph on its own: show its first output, unlit.
				std::vector<FunctionPin> inputs, outs;
				FunctionSignature(graph, inputs, outs);
				const Node* first = graph.Find(outs[0].node);
				PinInfo pin;
				pin.name = outs[0].name;
				pin.type = static_cast<PinType>(outs[0].width);
				const Value value = Input(top, *first, 0, pin);
				std::string rgb = value.width == 2 ? "float3(" + value.expr + ", 0.0)" : Coerce(top, *first, value, 3, pin.name);
				assign = "\ts.baseColor = 0.0;\n\ts.metallic = 0.0;\n\ts.roughness = 1.0;\n\ts.specular = 0.0;\n"
					"\ts.normalW = input.normalW;\n\ts.emissive = " + rgb + ";\n\ts.alpha = 1.0;\n\ts.opacityMask = 1.0;\n";
			}
			else
			{
				mResult.errors.push_back({ 0, "Add a Surface Output node" });
			}
			const std::string pixelBody = mBody;

			// ---- Vertex: EvaluateVertexOffset ------------------------------------------------
			std::string vertexBody, vertexResult;
			if (output && graph.InputLink(output->id, 8))
			{
				BeginFunction(true);
				const PinInfo& pin = outputPins.inputs[8];
				vertexResult = Coerce(top, *output, Input(top, *output, 8, pin), 3, pin.name);
				vertexBody = mBody;
				mResult.vertexOffset = true;
			}

			// ---- Parameter block layout: by name, so it doesn't depend on node order ---------------
			std::vector<size_t> order(mParams.size());
			for (size_t i = 0; i < order.size(); ++i) order[i] = i;
			std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return mParams[a].name < mParams[b].name; });
			std::vector<uint32_t> location(mParams.size());
			for (size_t i : order)
			{
				GraphParameter& param = mParams[i];
				if (param.kind == ParamKind::Texture)
				{
					param.offset = mResult.textureCount++;
				}
				else
				{
					param.offset = mResult.paramCount;
					mResult.paramCount += param.kind == ParamKind::Scalar ? 1 : 4;
				}
				location[i] = param.offset;
				mResult.parameters.push_back(param);
			}
			auto resolve = [&](std::string code)
			{
				for (size_t i = 0; i < mParams.size(); ++i)
				{
					const std::string token = "@P" + std::to_string(i) + "@";
					for (size_t at = code.find(token); at != std::string::npos; at = code.find(token, at))
					{
						code.replace(at, token.size(), std::to_string(location[i]));
					}
				}
				return code;
			};

			// ---- Assemble ---------------------------------------------------------------------
			std::string hlsl = "// Generated by the shader graph compiler.\n#include \"ShaderGraph.hlsli\"\n";
			if (graph.blend == BlendMode::Masked) hlsl += "#define CHERRY_SURFACE_MASKED 1\n";
			hlsl += "\n";
			for (const std::string& helper : mHelpers) hlsl += resolve(helper) + "\n";
			hlsl += "SurfaceOutput EvaluateSurface(SurfaceInput input)\n{\n" + resolve(pixelBody) + "\tSurfaceOutput s;\n" + resolve(assign) + "\treturn s;\n}\n";
			if (mResult.vertexOffset)
			{
				hlsl += "\n#define CHERRY_SURFACE_VERTEX_OFFSET 1\nfloat3 EvaluateVertexOffset(SurfaceInput input)\n{\n"
					+ resolve(vertexBody) + "\treturn " + resolve(vertexResult) + ";\n}\n";
			}
			mResult.hlsl = hlsl;
			mResult.ok = mResult.errors.empty();
			return mResult;
		}
	}

	CompiledGraph CompileGraph(const Graph& graph, const GraphLoader& loader)
	{
		Compiler compiler(loader);
		return compiler.Run(graph);
	}

	const GraphParameter* FindParameter(const CompiledGraph& compiled, const std::string& name)
	{
		for (const GraphParameter& param : compiled.parameters) if (param.name == name) return &param;
		return nullptr;
	}
}
