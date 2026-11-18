#include "Scene/SceneSerializer.h"
#include "Scene/Scene.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using json = nlohmann::ordered_json;
using namespace DirectX;

namespace
{
	constexpr int kSceneVersion = 1;

	std::string GuidToString(uint64_t guid)
	{
		char text[17];
		std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(guid));
		return text;
	}

	uint64_t GuidFromJson(const json& value)
	{
		if (value.is_string())
		{
			try { return std::stoull(value.get<std::string>(), nullptr, 16); }
			catch (...) { return 0; }
		}
		if (value.is_number_unsigned()) return value.get<uint64_t>();
		return 0;
	}

	int64_t ReadEnumValue(const void* data, size_t size)
	{
		switch (size)
		{
		case 1: { int8_t v; std::memcpy(&v, data, 1); return v; }
		case 2: { int16_t v; std::memcpy(&v, data, 2); return v; }
		case 8: { int64_t v; std::memcpy(&v, data, 8); return v; }
		default: { int32_t v; std::memcpy(&v, data, 4); return v; }
		}
	}

	void WriteEnumValue(void* data, size_t size, int64_t value)
	{
		switch (size)
		{
		case 1: { int8_t v = static_cast<int8_t>(value); std::memcpy(data, &v, 1); break; }
		case 2: { int16_t v = static_cast<int16_t>(value); std::memcpy(data, &v, 2); break; }
		case 8: { std::memcpy(data, &value, 8); break; }
		default: { int32_t v = static_cast<int32_t>(value); std::memcpy(data, &v, 4); break; }
		}
	}

	template <size_t N>
	json FloatArray(const float* values)
	{
		json array = json::array();
		for (size_t i = 0; i < N; ++i) array.push_back(values[i]);
		return array;
	}

	template <size_t N, typename T>
	void ReadArray(const json& value, T* out)
	{
		if (!value.is_array()) return;
		for (size_t i = 0; i < N && i < value.size(); ++i)
		{
			if (value[i].is_number()) out[i] = value[i].get<T>();
		}
	}

	json FieldToJson(Scene& scene, const FieldInfo& field, void* data)
	{
		switch (field.kind)
		{
		case FieldKind::Bool:   return *static_cast<bool*>(data);
		case FieldKind::Int:    return *static_cast<int32_t*>(data);
		case FieldKind::UInt:   return *static_cast<uint32_t*>(data);
		case FieldKind::UInt64: return GuidToString(*static_cast<uint64_t*>(data));
		case FieldKind::Float:  return *static_cast<float*>(data);
		case FieldKind::Float2: return FloatArray<2>(&static_cast<XMFLOAT2*>(data)->x);
		case FieldKind::Float3: return FloatArray<3>(&static_cast<XMFLOAT3*>(data)->x);
		case FieldKind::Float4: return FloatArray<4>(&static_cast<XMFLOAT4*>(data)->x);
		case FieldKind::Int3:
		{
			const XMINT3& v = *static_cast<XMINT3*>(data);
			return json::array({ v.x, v.y, v.z });
		}
		case FieldKind::String: return *static_cast<std::string*>(data);
		case FieldKind::Enum:
		{
			const int64_t value = ReadEnumValue(data, field.size);
			if (value >= 0 && value < static_cast<int64_t>(field.enumNames.size())) return field.enumNames[static_cast<size_t>(value)];
			return value;
		}
		case FieldKind::EntityRef:
		{
			const Entity target = *static_cast<Entity*>(data);
			const uint64_t guid = scene.GetGuid(target);
			if (guid == 0) return nullptr;   // not a saved entity
			return GuidToString(guid);
		}
		}
		return nullptr;
	}

	// `guidToEntity` resolves EntityRef fields.
	void FieldFromJson(const FieldInfo& field, void* data, const json& value, const std::unordered_map<uint64_t, Entity>& guidToEntity)
	{
		try
		{
			switch (field.kind)
			{
			case FieldKind::Bool:   if (value.is_boolean()) *static_cast<bool*>(data) = value.get<bool>(); break;
			case FieldKind::Int:    if (value.is_number()) *static_cast<int32_t*>(data) = value.get<int32_t>(); break;
			case FieldKind::UInt:   if (value.is_number()) *static_cast<uint32_t*>(data) = value.get<uint32_t>(); break;
			case FieldKind::UInt64: *static_cast<uint64_t*>(data) = GuidFromJson(value); break;
			case FieldKind::Float:  if (value.is_number()) *static_cast<float*>(data) = value.get<float>(); break;
			case FieldKind::Float2: ReadArray<2>(value, &static_cast<XMFLOAT2*>(data)->x); break;
			case FieldKind::Float3: ReadArray<3>(value, &static_cast<XMFLOAT3*>(data)->x); break;
			case FieldKind::Float4: ReadArray<4>(value, &static_cast<XMFLOAT4*>(data)->x); break;
			case FieldKind::Int3:   ReadArray<3>(value, &static_cast<XMINT3*>(data)->x); break;
			case FieldKind::String: if (value.is_string()) *static_cast<std::string*>(data) = value.get<std::string>(); break;
			case FieldKind::Enum:
				if (value.is_string())
				{
					for (size_t i = 0; i < field.enumNames.size(); ++i)
					{
						if (field.enumNames[i] == value.get<std::string>()) WriteEnumValue(data, field.size, static_cast<int64_t>(i));
					}
				}
				else if (value.is_number_integer())
				{
					WriteEnumValue(data, field.size, value.get<int64_t>());
				}
				break;
			case FieldKind::EntityRef:
			{
				auto it = guidToEntity.find(GuidFromJson(value));
				*static_cast<Entity*>(data) = it != guidToEntity.end() ? it->second : INVALID_ENTITY;
				break;
			}
			}
		}
		catch (const std::exception&)
		{
			// A value of the wrong type keeps the field's default.
		}
	}

	json ComponentToJson(Scene& scene, const ComponentInfo& info, void* component)
	{
		json object = json::object();
		for (const FieldInfo& field : info.fields)
		{
			if (field.flags & Field_NotSaved) continue;
			object[field.name] = FieldToJson(scene, field, field.access(component));
		}
		return object;
	}

	bool IsSaved(const ComponentInfo& info)
	{
		return !info.Is(Component_Transient) && info.name != "UnknownComponents";
	}

	json EntityToJson(Scene& scene, Entity entity)
	{
		json components = json::object();
		for (const auto& info : TypeRegistry::Get().Components())
		{
			if (!IsSaved(*info)) continue;
			void* component = info->get(scene, entity);
			if (!component) continue;
			json fields = ComponentToJson(scene, *info, component);
			if (info->name == "EntityInfo") fields.erase("guid");   // written as the entity's own key
			components[info->name] = std::move(fields);
		}
		if (UnknownComponents* unknown = scene.TryGet<UnknownComponents>(entity))
		{
			for (const auto& [name, text] : unknown->entries)
			{
				if (components.contains(name)) continue;   // now known: the live data wins
				json value = json::parse(text, nullptr, false);
				components[name] = value.is_discarded() ? json::object() : value;
			}
		}

		json object = json::object();
		object["guid"] = GuidToString(scene.GetGuid(entity));
		object["components"] = std::move(components);
		return object;
	}

	json SceneToJsonObject(Scene& scene)
	{
		// Only entities with an EntityInfo are saved: the rest were made by game or editor
		// code at runtime. Guids first, so references to later entities are written correctly.
		for (Entity entity : scene.Entities())
		{
			if (scene.Has<EntityInfo>(entity)) scene.EnsureGuid(entity);
		}

		json entities = json::array();
		for (Entity entity : scene.Entities())
		{
			if (scene.Has<EntityInfo>(entity)) entities.push_back(EntityToJson(scene, entity));
		}
		json root = json::object();
		root["format"] = "cherry-scene";
		root["version"] = kSceneVersion;
		root["entities"] = std::move(entities);
		return root;
	}

	bool SceneFromJsonObject(Scene& scene, const json& root, std::string& error)
	{
		if (!root.is_object() || !root.contains("entities") || !root["entities"].is_array())
		{
			error = "not a scene file (no \"entities\" list)";
			return false;
		}
		scene.Clear();
		const TypeRegistry& registry = TypeRegistry::Get();

		// Pass 1: create every entity, so references can point anywhere in the file.
		const json& list = root["entities"];
		std::vector<Entity> created;
		std::unordered_map<uint64_t, Entity> guidToEntity;
		created.reserve(list.size());
		for (const json& item : list)
		{
			const Entity entity = scene.CreateEntity();
			uint64_t guid = item.is_object() && item.contains("guid") ? GuidFromJson(item["guid"]) : 0;
			if (guid == 0 || guidToEntity.count(guid)) guid = Scene::NewGuid();
			EntityInfo info;
			info.guid = guid;
			scene.Add(entity, info);
			guidToEntity[guid] = entity;
			created.push_back(entity);
		}

		// Pass 2: components.
		for (size_t i = 0; i < list.size(); ++i)
		{
			const json& item = list[i];
			const Entity entity = created[i];
			if (!item.is_object() || !item.contains("components") || !item["components"].is_object()) continue;

			UnknownComponents unknown;
			for (auto it = item["components"].begin(); it != item["components"].end(); ++it)
			{
				const ComponentInfo* info = registry.Find(it.key());
				if (!info || !IsSaved(*info))
				{
					unknown.entries.emplace_back(it.key(), it.value().dump());
					continue;
				}
				void* component = info->get(scene, entity);
				if (!component) component = info->addDefault(scene, entity);
				for (const FieldInfo& field : info->fields)
				{
					if ((field.flags & Field_NotSaved) || !it.value().contains(field.name)) continue;
					if (info->name == "EntityInfo" && field.name == "guid") continue;
					FieldFromJson(field, field.access(component), it.value()[field.name], guidToEntity);
				}
			}
			if (!unknown.entries.empty()) scene.Add(entity, unknown);
		}
		return true;
	}
}

namespace cherry
{
	std::string SceneToJson(Scene& scene)
	{
		return SceneToJsonObject(scene).dump(1, '\t');
	}

	bool SceneFromJson(Scene& scene, const std::string& text, std::string& error)
	{
		json root = json::parse(text, nullptr, false);
		if (root.is_discarded())
		{
			error = "invalid JSON";
			return false;
		}
		return SceneFromJsonObject(scene, root, error);
	}

	bool LoadScene(Scene& scene, const std::filesystem::path& file, std::string& error)
	{
		std::ifstream stream(file, std::ios::binary);
		if (!stream)
		{
			error = "can't open " + file.string();
			return false;
		}
		std::stringstream buffer;
		buffer << stream.rdbuf();
		if (!SceneFromJson(scene, buffer.str(), error))
		{
			error = file.filename().string() + ": " + error;
			return false;
		}
		return true;
	}

	bool SaveScene(Scene& scene, const std::filesystem::path& file, std::string& error)
	{
		std::error_code ec;
		if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
		// Write next to the target, then replace it, so a failed write never leaves half a file.
		std::filesystem::path temp = file;
		temp += ".tmp";
		{
			std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				error = "can't write " + file.string();
				return false;
			}
			stream << SceneToJson(scene) << "\n";
			if (!stream)
			{
				error = "write failed for " + file.string();
				return false;
			}
		}
		std::filesystem::rename(temp, file, ec);
		if (ec)
		{
			error = "can't replace " + file.string() + ": " + ec.message();
			return false;
		}
		return true;
	}

	void CopyScene(Scene& from, Scene& to)
	{
		const TypeRegistry& registry = TypeRegistry::Get();
		std::unordered_map<Entity, Entity> map;
		for (Entity entity : from.Entities())
		{
			map[entity] = to.CreateEntity();
		}
		for (Entity entity : from.Entities())
		{
			const Entity copy = map[entity];
			for (const auto& info : registry.Components())
			{
				if (info->Is(Component_Transient)) continue;
				const void* component = info->get(from, entity);
				if (!component) continue;
				info->addCopy(to, copy, component);
				void* copied = info->get(to, copy);
				for (const FieldInfo& field : info->fields)
				{
					if (field.kind != FieldKind::EntityRef) continue;
					Entity& target = *static_cast<Entity*>(field.access(copied));
					auto it = map.find(target);
					target = it != map.end() ? it->second : INVALID_ENTITY;
				}
			}
		}
	}

	Entity DuplicateEntity(Scene& scene, Entity entity)
	{
		if (!scene.IsAlive(entity)) return INVALID_ENTITY;
		const TypeRegistry& registry = TypeRegistry::Get();

		// The entity and its descendants, parents before children.
		std::vector<Entity> tree{ entity };
		for (size_t i = 0; i < tree.size(); ++i)
		{
			for (Entity child : scene.Children(tree[i])) tree.push_back(child);
		}

		std::unordered_map<Entity, Entity> map;
		for (Entity source : tree) map[source] = scene.CreateEntity();
		for (Entity source : tree)
		{
			const Entity copy = map[source];
			for (const auto& info : registry.Components())
			{
				if (info->Is(Component_Transient)) continue;
				const void* component = info->get(scene, source);
				if (!component) continue;
				info->addCopy(scene, copy, component);
				void* copied = info->get(scene, copy);
				for (const FieldInfo& field : info->fields)
				{
					if (field.kind != FieldKind::EntityRef) continue;
					Entity& target = *static_cast<Entity*>(field.access(copied));
					auto it = map.find(target);
					if (it != map.end()) target = it->second;   // inside the copied tree: point at the copy
				}
			}
			EntityInfo& info = scene.Has<EntityInfo>(copy) ? scene.Get<EntityInfo>(copy) : (scene.Add(copy, EntityInfo{}), scene.Get<EntityInfo>(copy));
			info.guid = Scene::NewGuid();
			if (source == entity) info.name = scene.DisplayName(entity) + " (copy)";
		}

		// Keep the copy next to the original in the list.
		const auto& order = scene.Entities();
		const size_t index = static_cast<size_t>(std::find(order.begin(), order.end(), entity) - order.begin());
		scene.MoveEntity(map[entity], index + 1);
		return map[entity];
	}
}
