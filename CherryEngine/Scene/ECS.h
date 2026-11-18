#pragma once

// Small archetype-free ECS: entities are ids, components live in packed arrays,
// systems keep a set of the entities whose signature matches theirs.
#include <memory>
#include <vector>
#include <set>
#include <unordered_map>
#include <queue>
#include <bitset>
#include <string>
#include <typeinfo>
#include <cassert>
#include <cstdint>

using Entity = std::uint32_t;
constexpr Entity MAX_ENTITIES = 65536;
constexpr Entity INVALID_ENTITY = 0xFFFFFFFFu;

using ComponentType = std::uint8_t;
constexpr ComponentType MAX_COMPONENTS = 64;
using Signature = std::bitset<MAX_COMPONENTS>;

// Component and system types are keyed by typeid name rather than type_index so
// the keys match across the engine DLL and game DLLs.
template <typename T>
inline std::string EcsTypeName() { return typeid(T).name(); }

class EntityManager
{
public:
	EntityManager()
	{
		for (Entity entity = 0; entity < MAX_ENTITIES; ++entity)
		{
			mAvailableEntities.push(entity);
		}
		mSignatures.resize(MAX_ENTITIES);
		mAlive.resize(MAX_ENTITIES, false);
	}

	Entity CreateEntity()
	{
		assert(!mAvailableEntities.empty() && "Too many entities");
		Entity id = mAvailableEntities.front();
		mAvailableEntities.pop();
		mAlive[id] = true;
		++mLivingEntityCount;
		return id;
	}

	void DestroyEntity(Entity entity)
	{
		assert(entity < MAX_ENTITIES && mAlive[entity]);
		mSignatures[entity].reset();
		mAlive[entity] = false;
		mAvailableEntities.push(entity);
		--mLivingEntityCount;
	}

	void SetSignature(Entity entity, Signature signature) { mSignatures[entity] = signature; }
	Signature GetSignature(Entity entity) const { return mSignatures[entity]; }
	bool IsAlive(Entity entity) const { return entity < MAX_ENTITIES && mAlive[entity]; }
	uint32_t LivingCount() const { return mLivingEntityCount; }

private:
	std::queue<Entity> mAvailableEntities{};
	std::vector<Signature> mSignatures;
	std::vector<bool> mAlive;
	uint32_t mLivingEntityCount = 0;
};

class IComponentArray
{
public:
	virtual ~IComponentArray() = default;
	virtual void EntityDestroyed(Entity entity) = 0;
};

template <typename T>
class ComponentArray : public IComponentArray
{
public:
	// Packed and grown on demand (not preallocated for MAX_ENTITIES): a scene has an
	// array per registered type, and most types are on few entities.
	void InsertData(Entity entity, const T& component)
	{
		auto it = mEntityToIndexMap.find(entity);
		if (it != mEntityToIndexMap.end())
		{
			mComponentArray[it->second] = component;   // replace existing
			return;
		}
		size_t newIndex = mSize;
		mEntityToIndexMap[entity] = newIndex;
		mIndexToEntityMap[newIndex] = entity;
		mComponentArray.push_back(component);
		++mSize;
	}

	void RemoveData(Entity entity)
	{
		assert(mEntityToIndexMap.find(entity) != mEntityToIndexMap.end() && "Removing missing component");
		size_t indexOfRemovedEntity = mEntityToIndexMap[entity];
		size_t indexOfLastElement = mSize - 1;
		if (indexOfRemovedEntity != indexOfLastElement)
		{
			mComponentArray[indexOfRemovedEntity] = std::move(mComponentArray[indexOfLastElement]);
		}
		mComponentArray.pop_back();

		Entity entityOfLastElement = mIndexToEntityMap[indexOfLastElement];
		mEntityToIndexMap[entityOfLastElement] = indexOfRemovedEntity;
		mIndexToEntityMap[indexOfRemovedEntity] = entityOfLastElement;

		mEntityToIndexMap.erase(entity);
		mIndexToEntityMap.erase(indexOfLastElement);
		--mSize;
	}

	T& GetData(Entity entity)
	{
		auto it = mEntityToIndexMap.find(entity);
		assert(it != mEntityToIndexMap.end() && "Entity does not have this component");
		return mComponentArray[it->second];
	}

	bool HasData(Entity entity) const
	{
		return mEntityToIndexMap.find(entity) != mEntityToIndexMap.end();
	}

	void EntityDestroyed(Entity entity) override
	{
		if (HasData(entity))
		{
			RemoveData(entity);
		}
	}

private:
	std::vector<T> mComponentArray;
	std::unordered_map<Entity, size_t> mEntityToIndexMap;
	std::unordered_map<size_t, Entity> mIndexToEntityMap;
	size_t mSize = 0;
};

class ComponentManager
{
public:
	template <typename T>
	void RegisterComponent()
	{
		const std::string typeName = EcsTypeName<T>();
		assert(mComponentTypes.find(typeName) == mComponentTypes.end() && "Component registered twice");
		assert(mNextComponentType < MAX_COMPONENTS);
		mComponentTypes.insert({ typeName, mNextComponentType });
		mComponentArrays.insert({ typeName, std::make_shared<ComponentArray<T>>() });
		++mNextComponentType;
	}

	template <typename T>
	bool IsRegistered() const
	{
		return mComponentTypes.find(EcsTypeName<T>()) != mComponentTypes.end();
	}

	template <typename T>
	ComponentType GetComponentType()
	{
		auto it = mComponentTypes.find(EcsTypeName<T>());
		assert(it != mComponentTypes.end() && "Component not registered");
		return it->second;
	}

	template <typename T>
	void AddComponent(Entity entity, const T& component) { GetComponentArray<T>()->InsertData(entity, component); }

	template <typename T>
	void RemoveComponent(Entity entity) { GetComponentArray<T>()->RemoveData(entity); }

	template <typename T>
	T& GetComponent(Entity entity) { return GetComponentArray<T>()->GetData(entity); }

	template <typename T>
	bool HasComponent(Entity entity) { return GetComponentArray<T>()->HasData(entity); }

	void EntityDestroyed(Entity entity)
	{
		for (auto const& pair : mComponentArrays)
		{
			pair.second->EntityDestroyed(entity);
		}
	}

private:
	std::unordered_map<std::string, ComponentType> mComponentTypes{};
	std::unordered_map<std::string, std::shared_ptr<IComponentArray>> mComponentArrays{};
	ComponentType mNextComponentType{};

	template <typename T>
	std::shared_ptr<ComponentArray<T>> GetComponentArray()
	{
		auto it = mComponentArrays.find(EcsTypeName<T>());
		assert(it != mComponentArrays.end() && "Component not registered");
		return std::static_pointer_cast<ComponentArray<T>>(it->second);
	}
};

class System
{
public:
	virtual ~System() = default;
	// Called once when an entity starts / stops matching this system's signature.
	virtual void OnEntityAdded(Entity) {}
	virtual void OnEntityRemoved(Entity) {}
	std::set<Entity> mEntities;
};

class SystemManager
{
public:
	template <typename T>
	std::shared_ptr<T> RegisterSystem()
	{
		const std::string typeName = EcsTypeName<T>();
		assert(mSystems.find(typeName) == mSystems.end() && "System registered twice");
		auto system = std::make_shared<T>();
		mSystems.insert({ typeName, system });
		return system;
	}

	template <typename T>
	void SetSignature(Signature signature)
	{
		mSignatures[EcsTypeName<T>()] = signature;
	}

	void EntityDestroyed(Entity entity)
	{
		for (auto const& pair : mSystems)
		{
			auto const& system = pair.second;
			if (system->mEntities.erase(entity) > 0)
			{
				system->OnEntityRemoved(entity);
			}
		}
	}

	void EntitySignatureChanged(Entity entity, Signature entitySignature)
	{
		for (auto const& pair : mSystems)
		{
			auto const& system = pair.second;
			const Signature& systemSignature = mSignatures[pair.first];
			const bool matches = systemSignature.any() && (entitySignature & systemSignature) == systemSignature;
			const bool present = system->mEntities.count(entity) > 0;

			// Only notify on transitions, so adding unrelated components to an
			// entity that already matches doesn't re-run OnEntityAdded.
			if (matches && !present)
			{
				system->mEntities.insert(entity);
				system->OnEntityAdded(entity);
			}
			else if (!matches && present)
			{
				system->mEntities.erase(entity);
				system->OnEntityRemoved(entity);
			}
		}
	}

private:
	std::unordered_map<std::string, Signature> mSignatures{};
	std::unordered_map<std::string, std::shared_ptr<System>> mSystems{};
};

class Coordinator
{
public:
	void Init()
	{
		mComponentManager = std::make_unique<ComponentManager>();
		mEntityManager = std::make_unique<EntityManager>();
		mSystemManager = std::make_unique<SystemManager>();
	}

	Entity CreateEntity() { return mEntityManager->CreateEntity(); }

	void DestroyEntity(Entity entity)
	{
		// Systems first, so OnEntityRemoved can still read the components.
		mSystemManager->EntityDestroyed(entity);
		mComponentManager->EntityDestroyed(entity);
		mEntityManager->DestroyEntity(entity);
	}

	bool IsAlive(Entity entity) const { return mEntityManager->IsAlive(entity); }
	uint32_t LivingCount() const { return mEntityManager->LivingCount(); }
	Signature GetSignature(Entity entity) const { return mEntityManager->GetSignature(entity); }

	template <typename T>
	void RegisterComponent() { mComponentManager->RegisterComponent<T>(); }

	template <typename T>
	bool IsComponentRegistered() const { return mComponentManager->IsRegistered<T>(); }

	template <typename T>
	void AddComponent(Entity entity, const T& component)
	{
		mComponentManager->AddComponent<T>(entity, component);
		auto signature = mEntityManager->GetSignature(entity);
		signature.set(mComponentManager->GetComponentType<T>(), true);
		mEntityManager->SetSignature(entity, signature);
		mSystemManager->EntitySignatureChanged(entity, signature);
	}

	template <typename T>
	void RemoveComponent(Entity entity)
	{
		auto signature = mEntityManager->GetSignature(entity);
		signature.set(mComponentManager->GetComponentType<T>(), false);
		mEntityManager->SetSignature(entity, signature);
		mSystemManager->EntitySignatureChanged(entity, signature);
		mComponentManager->RemoveComponent<T>(entity);
	}

	template <typename T>
	T& GetComponent(Entity entity) { return mComponentManager->GetComponent<T>(entity); }

	template <typename T>
	bool HasComponent(Entity entity) { return mComponentManager->HasComponent<T>(entity); }

	template <typename T>
	ComponentType GetComponentType() { return mComponentManager->GetComponentType<T>(); }

	template <typename T>
	std::shared_ptr<T> RegisterSystem() { return mSystemManager->RegisterSystem<T>(); }

	template <typename T>
	void SetSystemSignature(Signature signature) { mSystemManager->SetSignature<T>(signature); }

private:
	std::unique_ptr<ComponentManager> mComponentManager;
	std::unique_ptr<EntityManager> mEntityManager;
	std::unique_ptr<SystemManager> mSystemManager;
};
