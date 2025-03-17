#include "Entity.h"
#include <d3d12.h>
#include <d3dUtil.h>
#include <Geometry.h>

struct Renderable
{
	Material* material = nullptr;
	Geometry* geometry = nullptr;
};

class RenderSystem : public System
{
public:
	void Init();

	void Update(float dt);

private:
	ID3D12GraphicsCommandList4* mCommandList = nullptr;
};