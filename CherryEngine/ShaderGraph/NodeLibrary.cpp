#include "ShaderGraph/NodeLibrary.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sg
{
	const char* const kSamplerNames[6] = { "Point, wrap", "Point, clamp", "Linear, wrap", "Linear, clamp", "Anisotropic, wrap", "Anisotropic, clamp" };
	const char* const kTextureKindNames[3] = { "Color (sRGB)", "Normal map", "Data (linear)" };

	namespace
	{
		const char* const kSamplerHlsl[6] = { "gPointWrap", "gPointClamp", "gLinearWrap", "gLinearClamp", "gAnisoWrap", "gAnisoClamp" };

		std::string T(int width) { return width <= 1 ? "float" : "float" + std::to_string(width); }

		PinInfo P(const char* name, PinType type, float a = 0, float b = 0, float c = 0, float d = 0)
		{
			PinInfo pin;
			pin.name = name;
			pin.type = type;
			pin.constant = { a, b, c, d };
			return pin;
		}

		PinInfo E(const char* name, PinType type, const char* expr)
		{
			PinInfo pin = P(name, type);
			pin.defaultExpr = expr;
			return pin;
		}

		constexpr PinType F1 = PinType::Float1, F2 = PinType::Float2, F3 = PinType::Float3, F4 = PinType::Float4;
		constexpr PinType Dyn = PinType::Dynamic, Any = PinType::Any;

		// Component `index` of an expression `width` wide ("0.0" past the end).
		std::string Component(const std::string& expr, int width, int index)
		{
			if (index >= width) return "0.0";
			if (width == 1) return expr;
			return expr + "." + "xyzw"[index];
		}

		std::string Literal(const Float4& v, int width)
		{
			auto number = [](float f)
			{
				char buffer[32];
				std::snprintf(buffer, sizeof(buffer), "%.9g", f);
				std::string s = buffer;
				if (s.find_first_of(".eEn") == std::string::npos) s += ".0";   // keep it a float literal
				return s;
			};
			if (width <= 1) return number(v[0]);
			std::string s = T(width) + "(";
			for (int i = 0; i < width; ++i) s += (i ? ", " : "") + number(v[i]);
			return s + ")";
		}

		std::vector<NodeDef> BuildDefs()
		{
			std::vector<NodeDef> defs;
			auto add = [&](const char* type, const char* title, const char* category, const char* description,
				std::vector<PinInfo> inputs, std::vector<PinInfo> outputs, std::function<void(EmitContext&)> emit) -> NodeDef&
			{
				NodeDef def;
				def.type = type;
				def.title = title;
				def.category = category;
				def.description = description;
				def.pins.inputs = std::move(inputs);
				def.pins.outputs = std::move(outputs);
				def.emit = std::move(emit);
				defs.push_back(std::move(def));
				return defs.back();
			};
			auto fixed = [](std::vector<std::string> exprs)
			{
				return [exprs](EmitContext& c) { for (size_t i = 0; i < exprs.size(); ++i) c.out[i] = exprs[i]; };
			};

			// ---- Output ----------------------------------------------------------------
			add("Output", "Surface Output", "Output", "What the material looks like. Unconnected inputs use their constants.",
				{ P("Base Color", F3, 0.8f, 0.8f, 0.8f), P("Metallic", F1), P("Roughness", F1, 0.5f), P("Specular", F1, 0.5f),
				  E("Normal", F3, "input.normalW"), P("Emissive", F3), P("Opacity", F1, 1), P("Opacity Mask", F1, 1),
				  P("World Position Offset", F3) },
				{}, nullptr);

			// ---- Inputs ----------------------------------------------------------------
			add("TexCoord", "Texture Coordinates", "Inputs", "The mesh UVs, times Tiling.",
				{ P("Tiling", F2, 1, 1) }, { P("UV", F2) },
				[](EmitContext& c) { c.out[0] = "(input.uv * " + c.in[0] + ")"; });
			add("VertexColor", "Vertex Color", "Inputs", "The mesh's vertex colour.",
				{}, { P("RGBA", F4), P("RGB", F3), P("A", F1) },
				fixed({ "input.vertexColor", "input.vertexColor.rgb", "input.vertexColor.a" }));
			add("WorldPosition", "World Position", "Inputs", "Position of the pixel (or vertex) in world space, metres.",
				{}, { P("Position", F3) }, fixed({ "input.positionW" }));
			add("WorldNormal", "World Normal", "Inputs", "The mesh normal in world space.",
				{}, { P("Normal", F3) }, fixed({ "input.normalW" }));
			add("ViewDirection", "View Direction", "Inputs", "From the surface towards the camera, normalized.",
				{}, { P("Direction", F3) }, fixed({ "input.viewDirW" }));
			add("CameraPosition", "Camera Position", "Inputs", "The camera's world position.",
				{}, { P("Position", F3) }, fixed({ "gView.eyePosW" }));
			add("Time", "Time", "Inputs", "Seconds since the engine started.",
				{}, { P("Time", F1) }, fixed({ "input.time" }));
			add("ScreenPosition", "Screen Position", "Inputs", "0..1 across the view, (0, 0) top left.",
				{}, { P("UV", F2) }, fixed({ "input.screenUV" })).pixelOnly = true;

			// ---- Constants ---------------------------------------------------------------
			{
				NodeDef& d = add("Float", "Float", "Constants", "A constant number.",
					{}, { P("Value", F1) }, [](EmitContext& c) { c.out[0] = Literal(c.node->value, 1); });
				d.settings = Setting_Value; d.valueEdit = ValueEdit::Float1;
			}
			{
				NodeDef& d = add("Vector2", "Vector 2", "Constants", "A constant float2.",
					{}, { P("Value", F2) }, [](EmitContext& c) { c.out[0] = Literal(c.node->value, 2); });
				d.settings = Setting_Value; d.valueEdit = ValueEdit::Float2;
			}
			{
				NodeDef& d = add("Vector3", "Vector 3", "Constants", "A constant float3.",
					{}, { P("Value", F3) }, [](EmitContext& c) { c.out[0] = Literal(c.node->value, 3); });
				d.settings = Setting_Value; d.valueEdit = ValueEdit::Float3;
			}
			{
				NodeDef& d = add("Vector4", "Vector 4", "Constants", "A constant float4.",
					{}, { P("Value", F4) }, [](EmitContext& c) { c.out[0] = Literal(c.node->value, 4); });
				d.settings = Setting_Value; d.valueEdit = ValueEdit::Float4;
			}
			{
				NodeDef& d = add("Color", "Color", "Constants", "A constant colour (linear).",
					{}, { P("RGBA", F4), P("RGB", F3), P("A", F1) }, [](EmitContext& c)
				{
					c.out[0] = Literal(c.node->value, 4);
					c.out[1] = Literal(c.node->value, 3);
					c.out[2] = Literal({ c.node->value[3], 0, 0, 0 }, 1);
				});
				d.settings = Setting_Value; d.valueEdit = ValueEdit::Color; d.defaultValue = { 1, 1, 1, 1 };
			}

			// ---- Parameters ------------------------------------------------------------------
			{
				NodeDef& d = add("ScalarParameter", "Scalar Parameter", "Parameters", "A number each material can set.",
					{}, { P("Value", F1) }, [](EmitContext& c) { c.out[0] = "ParamFloat(input, " + c.param(ParamKind::Scalar) + ")"; });
				d.settings = Setting_Name | Setting_Value; d.valueEdit = ValueEdit::Float1; d.defaultName = "Scalar"; d.defaultValue = { 1, 0, 0, 0 };
			}
			{
				NodeDef& d = add("VectorParameter", "Vector Parameter", "Parameters", "A float4 each material can set.",
					{}, { P("XYZW", F4), P("XYZ", F3) }, [](EmitContext& c)
				{
					const std::string v = c.temp(4, "ParamFloat4(input, " + c.param(ParamKind::Vector) + ")");
					c.out[0] = v;
					c.out[1] = v + ".xyz";
				});
				d.settings = Setting_Name | Setting_Value; d.valueEdit = ValueEdit::Float4; d.defaultName = "Vector";
			}
			{
				NodeDef& d = add("ColorParameter", "Color Parameter", "Parameters", "A colour each material can set (linear).",
					{}, { P("RGBA", F4), P("RGB", F3), P("A", F1) }, [](EmitContext& c)
				{
					const std::string v = c.temp(4, "ParamFloat4(input, " + c.param(ParamKind::Color) + ")");
					c.out[0] = v;
					c.out[1] = v + ".rgb";
					c.out[2] = v + ".a";
				});
				d.settings = Setting_Name | Setting_Value; d.valueEdit = ValueEdit::Color; d.defaultName = "Color"; d.defaultValue = { 1, 1, 1, 1 };
			}
			{
				NodeDef& d = add("TextureSample", "Texture Sample", "Parameters",
					"Samples a texture each material can set (the asset is the default). Normal maps give a tangent-space normal in -1..1.",
					{ E("UV", F2, "input.uv") }, { P("RGBA", F4), P("RGB", F3), P("R", F1), P("G", F1), P("B", F1), P("A", F1) },
					[](EmitContext& c)
				{
					const int sampler = std::clamp(c.node->option, 0, 5);
					const bool normalMap = c.node->option2 == 1;
					const std::string fallback = normalMap ? "float4(0.5, 0.5, 1.0, 1.0)" : "float4(1.0, 1.0, 1.0, 1.0)";
					const std::string slot = c.param(ParamKind::Texture);
					std::string sample = c.vertexStage
						? "SampleParamTextureLevel(input, " + slot + ", " + kSamplerHlsl[sampler] + ", " + c.in[0] + ", 0.0, " + fallback + ")"
						: "SampleParamTexture(input, " + slot + ", " + kSamplerHlsl[sampler] + ", " + c.in[0] + ", " + fallback + ")";
					const std::string v = c.temp(4, sample);
					c.out[0] = v;
					c.out[1] = normalMap ? "(" + v + ".rgb * 2.0 - 1.0)" : v + ".rgb";
					c.out[2] = v + ".r";
					c.out[3] = v + ".g";
					c.out[4] = v + ".b";
					c.out[5] = v + ".a";
				});
				d.settings = Setting_Name | Setting_Texture | Setting_Sampler | Setting_TextureKind;
				d.defaultName = "Texture";
				d.defaultOption = 4;   // anisotropic, wrap
			}

			// ---- Math --------------------------------------------------------------------
			auto binary = [&](const char* type, const char* title, const char* description, float a, float b, const char* format)
			{
				std::string fmt = format;
				add(type, title, "Math", description, { P("A", Dyn, a), P("B", Dyn, b) }, { P("Result", Dyn) },
					[fmt](EmitContext& c)
				{
					std::string s = fmt;
					s.replace(s.find("$A"), 2, c.in[0]);
					s.replace(s.find("$B"), 2, c.in[1]);
					c.out[0] = s;
				});
			};
			binary("Add", "Add", "A + B", 0, 0, "($A + $B)");
			binary("Subtract", "Subtract", "A - B", 0, 0, "($A - $B)");
			binary("Multiply", "Multiply", "A x B, per component", 1, 1, "($A * $B)");
			binary("Divide", "Divide", "A / B, per component", 1, 1, "($A / $B)");
			binary("Min", "Min", "The smaller of A and B", 0, 0, "min($A, $B)");
			binary("Max", "Max", "The larger of A and B", 0, 0, "max($A, $B)");
			binary("Power", "Power", "A to the power B (A below 0 counts as 0)", 1, 2, "pow(max($A, 0.0), $B)");
			binary("Step", "Step", "1 where B >= A (the edge), else 0", 0.5f, 0, "step($A, $B)");

			auto unary = [&](const char* type, const char* title, const char* description, const char* format)
			{
				std::string fmt = format;
				add(type, title, "Math", description, { P("In", Dyn) }, { P("Result", Dyn) }, [fmt](EmitContext& c)
				{
					std::string s = fmt;
					s.replace(s.find("$X"), 2, c.in[0]);
					c.out[0] = s;
				});
			};
			unary("OneMinus", "One Minus", "1 - In", "(1.0 - $X)");
			unary("Saturate", "Saturate", "Clamps to 0..1", "saturate($X)");
			unary("Abs", "Abs", "Absolute value", "abs($X)");
			unary("Negate", "Negate", "-In", "(-$X)");
			unary("Sine", "Sine", "sin(In), In in radians", "sin($X)");
			unary("Cosine", "Cosine", "cos(In), In in radians", "cos($X)");
			unary("Frac", "Frac", "The fractional part", "frac($X)");
			unary("Floor", "Floor", "Rounds down", "floor($X)");
			unary("Ceil", "Ceil", "Rounds up", "ceil($X)");
			unary("Round", "Round", "Rounds to the nearest whole number", "round($X)");
			unary("Sign", "Sign", "-1, 0 or 1", "sign($X)");
			unary("Sqrt", "Square Root", "Square root (below 0 counts as 0)", "sqrt(max($X, 0.0))");
			unary("Normalize", "Normalize", "Scales a vector to length 1", "normalize($X)");

			add("Lerp", "Lerp", "Math", "Blends from A (Alpha 0) to B (Alpha 1).",
				{ P("A", Dyn, 0), P("B", Dyn, 1), P("Alpha", Dyn, 0.5f) }, { P("Result", Dyn) },
				[](EmitContext& c) { c.out[0] = "lerp(" + c.in[0] + ", " + c.in[1] + ", " + c.in[2] + ")"; });
			add("Clamp", "Clamp", "Math", "Keeps In between Min and Max.",
				{ P("In", Dyn), P("Min", Dyn, 0), P("Max", Dyn, 1) }, { P("Result", Dyn) },
				[](EmitContext& c) { c.out[0] = "clamp(" + c.in[0] + ", " + c.in[1] + ", " + c.in[2] + ")"; });
			add("Smoothstep", "Smoothstep", "Math", "0 below Min, 1 above Max, a smooth curve between.",
				{ P("Min", Dyn, 0), P("Max", Dyn, 1), P("In", Dyn) }, { P("Result", Dyn) },
				[](EmitContext& c) { c.out[0] = "smoothstep(" + c.in[0] + ", " + c.in[1] + ", " + c.in[2] + ")"; });
			add("Remap", "Remap", "Math", "Maps In from In Min..In Max to Out Min..Out Max.",
				{ P("In", Dyn), P("In Min", Dyn, 0), P("In Max", Dyn, 1), P("Out Min", Dyn, 0), P("Out Max", Dyn, 1) }, { P("Result", Dyn) },
				[](EmitContext& c)
			{
				c.out[0] = "lerp(" + c.in[3] + ", " + c.in[4] + ", (" + c.in[0] + " - " + c.in[1] + ") / (" + c.in[2] + " - " + c.in[1] + "))";
			});
			add("Dot", "Dot", "Math", "Dot product of A and B.",
				{ P("A", Dyn), P("B", Dyn) }, { P("Result", F1) },
				[](EmitContext& c) { c.out[0] = "dot(" + c.in[0] + ", " + c.in[1] + ")"; });
			add("Cross", "Cross", "Math", "Cross product of A and B.",
				{ P("A", F3), P("B", F3) }, { P("Result", F3) },
				[](EmitContext& c) { c.out[0] = "cross(" + c.in[0] + ", " + c.in[1] + ")"; });
			add("Length", "Length", "Math", "Length of a vector.",
				{ P("In", Dyn) }, { P("Result", F1) }, [](EmitContext& c) { c.out[0] = "length(" + c.in[0] + ")"; });
			add("Distance", "Distance", "Math", "Distance between A and B.",
				{ P("A", Dyn), P("B", Dyn) }, { P("Result", F1) },
				[](EmitContext& c) { c.out[0] = "distance(" + c.in[0] + ", " + c.in[1] + ")"; });

			// ---- Vector ------------------------------------------------------------------
			add("Split", "Split", "Vector", "The components of a vector (0 past its width).",
				{ P("In", Any) }, { P("R", F1), P("G", F1), P("B", F1), P("A", F1) },
				[](EmitContext& c)
			{
				const std::string v = c.inWidth[0] > 1 ? c.temp(c.inWidth[0], c.in[0]) : c.in[0];
				for (int i = 0; i < 4; ++i) c.out[i] = Component(v, c.inWidth[0], i);
			});
			add("Append", "Append", "Vector", "Joins A and B into one vector (at most 4 wide).",
				{ P("A", Any), P("B", Any) }, { P("Result", Any) },
				[](EmitContext& c)
			{
				const int width = c.inWidth[0] + c.inWidth[1];
				if (width > 4) { c.error = "Append: the result would be " + std::to_string(width) + " wide (4 at most)"; return; }
				c.out[0] = T(width) + "(" + c.in[0] + ", " + c.in[1] + ")";
				c.outWidth[0] = width;
			});
			add("Combine", "Combine", "Vector", "Builds a vector from components.",
				{ P("R", F1), P("G", F1), P("B", F1), P("A", F1, 1) }, { P("RGBA", F4), P("RGB", F3), P("RG", F2) },
				[](EmitContext& c)
			{
				c.out[0] = "float4(" + c.in[0] + ", " + c.in[1] + ", " + c.in[2] + ", " + c.in[3] + ")";
				c.out[1] = "float3(" + c.in[0] + ", " + c.in[1] + ", " + c.in[2] + ")";
				c.out[2] = "float2(" + c.in[0] + ", " + c.in[1] + ")";
			});
			{
				NodeDef& d = add("Swizzle", "Swizzle", "Vector", "Picks and reorders components, e.g. \"zyx\" or \"rrr\".",
					{ P("In", Any) }, { P("Result", Any) },
					[](EmitContext& c)
				{
					std::string mask;
					for (char ch : c.node->text)
					{
						const char* letters = "xyzwrgba";
						const char* found = ch ? std::strchr(letters, ch) : nullptr;
						if (!found) { c.error = std::string("Swizzle: '") + ch + "' isn't one of xyzw / rgba"; return; }
						const int index = static_cast<int>(found - letters) % 4;
						if (index >= c.inWidth[0]) { c.error = std::string("Swizzle: the input has no '") + ch + "' component"; return; }
						mask += "xyzw"[index];
					}
					if (mask.empty() || mask.size() > 4) { c.error = "Swizzle: the mask needs 1 to 4 letters"; return; }
					const std::string v = c.temp(c.inWidth[0], c.in[0]);
					c.out[0] = v + "." + mask;
					c.outWidth[0] = static_cast<int>(mask.size());
				});
				d.settings = Setting_Text; d.defaultText = "xyz";
			}

			// ---- Utility -------------------------------------------------------------------
			add("Fresnel", "Fresnel", "Utility", "Brighter where the surface turns away from the camera (Schlick).",
				{ P("Exponent", F1, 5), P("Base Reflect", F1, 0.04f), E("Normal", F3, "input.normalW") }, { P("Result", F1) },
				[](EmitContext& c)
			{
				c.out[0] = "(" + c.in[1] + " + (1.0 - " + c.in[1] + ") * pow(1.0 - saturate(dot(normalize(" + c.in[2] + "), input.viewDirW)), " + c.in[0] + "))";
			});
			add("Panner", "Panner", "Utility", "Scrolls UVs over time: UV + Speed x Time.",
				{ E("UV", F2, "input.uv"), P("Speed", F2, 0.1f, 0), E("Time", F1, "input.time") }, { P("UV", F2) },
				[](EmitContext& c) { c.out[0] = "(" + c.in[0] + " + " + c.in[1] + " * " + c.in[2] + ")"; });
			{
				NodeDef& d = add("Noise", "Noise", "Utility", "Smooth value noise in 0..1, summed over octaves.",
					{ E("Position", F3, "input.positionW"), P("Scale", F1, 1) }, { P("Result", F1) },
					[](EmitContext& c)
				{
					const int octaves = std::clamp(c.node->option, 1, 8);
					c.out[0] = "CherryGraphFbm(" + c.in[0] + " * " + c.in[1] + ", " + std::to_string(octaves) + ")";
				});
				d.settings = Setting_Octaves; d.defaultOption = 3;
			}
			{
				NodeDef& d = add("NormalMap", "Normal Map", "Utility",
					"Turns a tangent-space normal (from a Texture Sample set to Normal map) into a world normal for Surface Output.",
					{ P("Tangent Normal", F3, 0, 0, 1), P("Strength", F1, 1) }, { P("Normal", F3) },
					[](EmitContext& c) { c.out[0] = "CherryPerturbNormal(input, " + c.in[0] + ", " + c.in[1] + ")"; });
				d.pixelOnly = true;
			}
			{
				NodeDef& d = add("Custom", "Custom HLSL", "Utility",
					"Your own HLSL. Inputs A..D keep the width connected to them; `input` (SurfaceInput) is available. "
					"Write a function body that returns the result, or a single expression.",
					{ P("A", Any), P("B", Any), P("C", Any), P("D", Any) }, { P("Result", Any) },
					[](EmitContext& c)
				{
					const int width = std::clamp(c.node->option, 1, 4);
					std::string body = c.node->text;
					if (body.find("return") == std::string::npos) body = "return " + (body.empty() ? std::string("0.0") : body) + ";";
					std::string name = "CherryCustom_" + c.uid;
					std::string code = T(width) + " " + name + "(SurfaceInput input";
					const char* names[4] = { "A", "B", "C", "D" };
					for (int i = 0; i < 4; ++i) code += ", " + T(c.inWidth[i]) + " " + names[i];
					code += ")\n{\n" + body + "\n}\n";
					c.helper(name, code);
					c.out[0] = name + "(input, " + c.in[0] + ", " + c.in[1] + ", " + c.in[2] + ", " + c.in[3] + ")";
					c.outWidth[0] = width;
				});
				d.settings = Setting_Code | Setting_Width; d.defaultOption = 3; d.defaultText = "return A * 0.5;";
			}

			// ---- Functions (subgraphs) -----------------------------------------------------
			{
				NodeDef& d = add("FunctionInput", "Function Input", "Functions",
					"An input of this graph when another graph uses it as a subgraph. The value is used when nothing is connected there.",
					{}, { P("Value", Any) }, nullptr);
				d.settings = Setting_Name | Setting_Width | Setting_Value; d.valueEdit = ValueEdit::Float4; d.defaultName = "In"; d.defaultOption = 1;
			}
			{
				NodeDef& d = add("FunctionOutput", "Function Output", "Functions",
					"An output of this graph when another graph uses it as a subgraph.",
					{ P("Value", Any) }, {}, nullptr);
				d.settings = Setting_Name | Setting_Width; d.defaultName = "Out"; d.defaultOption = 1;
			}
			{
				NodeDef& d = add("Subgraph", "Subgraph", "Functions",
					"Uses another .shadergraph (one with Function Input / Output nodes) as a node.",
					{}, {}, nullptr);
				d.settings = Setting_Subgraph;
			}
			return defs;
		}
	}

	const std::vector<NodeDef>& NodeDefs()
	{
		static const std::vector<NodeDef> defs = BuildDefs();
		return defs;
	}

	const NodeDef* FindNodeDef(const std::string& type)
	{
		for (const NodeDef& def : NodeDefs()) if (def.type == type) return &def;
		return nullptr;
	}

	void FunctionSignature(const Graph& graph, std::vector<FunctionPin>& inputs, std::vector<FunctionPin>& outputs)
	{
		inputs.clear();
		outputs.clear();
		std::vector<const Node*> sorted;
		for (const Node& node : graph.nodes) sorted.push_back(&node);
		std::sort(sorted.begin(), sorted.end(), [](const Node* a, const Node* b) { return a->y != b->y ? a->y < b->y : a->id < b->id; });
		for (const Node* node : sorted)
		{
			if (node->type != "FunctionInput" && node->type != "FunctionOutput") continue;
			FunctionPin pin;
			pin.node = node->id;
			pin.name = node->name.empty() ? (node->type == "FunctionInput" ? "In" : "Out") : node->name;
			pin.width = std::clamp(node->option, 1, 4);
			pin.defaultValue = node->value;
			(node->type == "FunctionInput" ? inputs : outputs).push_back(pin);
		}
	}

	NodePins ResolvePins(const Node& node, const GraphLoader* loader)
	{
		const NodeDef* def = FindNodeDef(node.type);
		if (!def) return {};
		NodePins pins = def->pins;
		const PinType width = static_cast<PinType>(std::clamp(node.option, 1, 4));
		if (node.type == "FunctionInput")
		{
			pins.outputs[0].type = width;
		}
		else if (node.type == "FunctionOutput")
		{
			pins.inputs[0].type = width;
		}
		else if (node.type == "Custom")
		{
			pins.outputs[0].type = width;
		}
		else if (node.type == "Subgraph" && loader && *loader && !node.asset.empty())
		{
			if (const Graph* function = (*loader)(node.asset))
			{
				std::vector<FunctionPin> inputs, outputs;
				FunctionSignature(*function, inputs, outputs);
				for (const FunctionPin& in : inputs)
				{
					PinInfo pin = P("", static_cast<PinType>(in.width));
					pin.name = in.name;
					pin.constant = in.defaultValue;
					pins.inputs.push_back(pin);
				}
				for (const FunctionPin& out : outputs)
				{
					PinInfo pin = P("", static_cast<PinType>(out.width));
					pin.name = out.name;
					pins.outputs.push_back(pin);
				}
			}
		}
		return pins;
	}

	void FitInputs(Node& node, const NodePins& pins)
	{
		while (node.inputs.size() < pins.inputs.size()) node.inputs.push_back(pins.inputs[node.inputs.size()].constant);
		if (node.inputs.size() > pins.inputs.size()) node.inputs.resize(pins.inputs.size());
	}

	Node MakeNode(const std::string& type, uint32_t id, const GraphLoader* loader)
	{
		Node node;
		node.id = id;
		node.type = type;
		if (const NodeDef* def = FindNodeDef(type))
		{
			node.name = def->defaultName;
			node.text = def->defaultText;
			node.option = def->defaultOption;
			node.value = def->defaultValue;
		}
		FitInputs(node, ResolvePins(node, loader));
		return node;
	}
}
