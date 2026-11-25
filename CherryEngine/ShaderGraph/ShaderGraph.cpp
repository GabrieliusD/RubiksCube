#include "ShaderGraph/ShaderGraph.h"
#include "ShaderGraph/NodeLibrary.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace sg
{
	Node* Graph::Find(uint32_t id)
	{
		for (Node& node : nodes) if (node.id == id) return &node;
		return nullptr;
	}

	const Node* Graph::Find(uint32_t id) const
	{
		for (const Node& node : nodes) if (node.id == id) return &node;
		return nullptr;
	}

	const Link* Graph::InputLink(uint32_t node, uint32_t input) const
	{
		for (const Link& link : links) if (link.toNode == node && link.toPin == input) return &link;
		return nullptr;
	}

	void Graph::Connect(uint32_t fromNode, uint32_t fromPin, uint32_t toNode, uint32_t toPin)
	{
		Disconnect(toNode, toPin);
		links.push_back({ fromNode, fromPin, toNode, toPin });
	}

	void Graph::Disconnect(uint32_t toNode, uint32_t toPin)
	{
		links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& link)
		{
			return link.toNode == toNode && link.toPin == toPin;
		}), links.end());
	}

	void Graph::RemoveNode(uint32_t id)
	{
		links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& link)
		{
			return link.fromNode == id || link.toNode == id;
		}), links.end());
		nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const Node& node) { return node.id == id; }), nodes.end());
	}

	bool Graph::IsFunction() const
	{
		bool hasOutput = false, hasFunctionOutput = false;
		for (const Node& node : nodes)
		{
			hasOutput |= node.type == "Output";
			hasFunctionOutput |= node.type == "FunctionOutput";
		}
		return hasFunctionOutput && !hasOutput;
	}

	const char* BlendModeName(BlendMode mode)
	{
		switch (mode)
		{
		case BlendMode::Masked: return "Masked";
		case BlendMode::Translucent: return "Translucent";
		default: return "Opaque";
		}
	}

	namespace
	{
		nlohmann::json ToJson(const Float4& v) { return nlohmann::json::array({ v[0], v[1], v[2], v[3] }); }

		Float4 ReadFloat4(const nlohmann::json& json, const Float4& fallback)
		{
			Float4 v = fallback;
			if (json.is_number()) { v = { json.get<float>(), 0, 0, 0 }; return v; }
			if (!json.is_array()) return v;
			for (size_t i = 0; i < 4 && i < json.size(); ++i)
			{
				if (json[i].is_number()) v[i] = json[i].get<float>();
			}
			return v;
		}
	}

	std::string GraphToJson(const Graph& graph)
	{
		nlohmann::ordered_json json = nlohmann::ordered_json::object();
		json["format"] = "cherry-shadergraph";
		json["version"] = 1;
		json["blend"] = BlendModeName(graph.blend);
		json["boundsPadding"] = graph.boundsPadding;
		nlohmann::ordered_json nodes = nlohmann::ordered_json::array();
		for (const Node& node : graph.nodes)
		{
			const NodeDef* def = FindNodeDef(node.type);
			nlohmann::ordered_json n = nlohmann::ordered_json::object();
			n["id"] = node.id;
			n["type"] = node.type;
			n["pos"] = { std::round(node.x), std::round(node.y) };
			if (!node.inputs.empty())
			{
				nlohmann::ordered_json inputs = nlohmann::ordered_json::array();
				for (const Float4& input : node.inputs) inputs.push_back(nlohmann::ordered_json::array({ input[0], input[1], input[2], input[3] }));
				n["inputs"] = inputs;
			}
			const uint32_t settings = def ? def->settings : 0xFFFFFFFFu;
			if (!node.name.empty()) n["name"] = node.name;
			if (!node.text.empty()) n["text"] = node.text;
			if (!node.asset.empty()) n["asset"] = node.asset;
			if (node.option != 0 || (settings & (Setting_Sampler | Setting_Width | Setting_Octaves))) n["option"] = node.option;
			if (node.option2 != 0) n["option2"] = node.option2;
			if (settings & Setting_Value) n["value"] = ToJson(node.value);
			nodes.push_back(n);
		}
		json["nodes"] = nodes;
		nlohmann::ordered_json links = nlohmann::ordered_json::array();
		for (const Link& link : graph.links) links.push_back({ link.fromNode, link.fromPin, link.toNode, link.toPin });
		json["links"] = links;
		return json.dump(1, '\t');
	}

	bool GraphFromJson(const std::string& text, Graph& graph, std::string& error)
	{
		const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
		if (json.is_discarded() || !json.is_object())
		{
			error = "invalid JSON";
			return false;
		}
		if (json.value("format", std::string()) != "cherry-shadergraph")
		{
			error = "not a shader graph (\"format\" isn't \"cherry-shadergraph\")";
			return false;
		}
		graph = Graph{};
		const std::string blend = json.value("blend", std::string("Opaque"));
		graph.blend = blend == "Masked" ? BlendMode::Masked : blend == "Translucent" ? BlendMode::Translucent : BlendMode::Opaque;
		graph.boundsPadding = json.value("boundsPadding", 0.0f);

		if (json.contains("nodes") && json["nodes"].is_array())
		{
			for (const auto& n : json["nodes"])
			{
				if (!n.is_object()) continue;
				Node node;
				node.id = n.value("id", 0u);
				node.type = n.value("type", std::string());
				if (node.id == 0 || node.type.empty() || graph.Find(node.id)) continue;
				if (n.contains("pos") && n["pos"].is_array() && n["pos"].size() >= 2)
				{
					node.x = n["pos"][0].get<float>();
					node.y = n["pos"][1].get<float>();
				}
				node.name = n.value("name", std::string());
				node.text = n.value("text", std::string());
				node.asset = n.value("asset", std::string());
				if (const NodeDef* def = FindNodeDef(node.type))
				{
					node.option = def->defaultOption;
					node.value = def->defaultValue;
				}
				node.option = n.value("option", node.option);
				node.option2 = n.value("option2", 0);
				if (n.contains("value")) node.value = ReadFloat4(n["value"], node.value);
				if (n.contains("inputs") && n["inputs"].is_array())
				{
					for (const auto& input : n["inputs"]) node.inputs.push_back(ReadFloat4(input, Float4{ 0, 0, 0, 0 }));
				}
				graph.nextId = std::max(graph.nextId, node.id + 1);
				graph.nodes.push_back(std::move(node));
			}
		}
		// Pins of fixed node types: fill missing input constants with the defaults. (Subgraph
		// pins depend on another file; the compiler and editor fit those.)
		for (Node& node : graph.nodes)
		{
			if (node.type != "Subgraph") FitInputs(node, ResolvePins(node, nullptr));
		}

		if (json.contains("links") && json["links"].is_array())
		{
			for (const auto& l : json["links"])
			{
				if (!l.is_array() || l.size() < 4) continue;
				Link link{ l[0].get<uint32_t>(), l[1].get<uint32_t>(), l[2].get<uint32_t>(), l[3].get<uint32_t>() };
				if (!graph.Find(link.fromNode) || !graph.Find(link.toNode) || graph.InputLink(link.toNode, link.toPin)) continue;
				graph.links.push_back(link);
			}
		}
		return true;
	}
}
