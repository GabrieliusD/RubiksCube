#pragma once
#include <D3DApp.h>
#include <Entity.h>
#include <RenderSystem.h>

class RubikApp : public D3DApp
{
public:
	RubikApp() {}
	virtual void OnAppInitialized();

private:
	void CreateTextures();
	void CreateMesh();
	void CreateMaterials();
	void CreateEntities();

};
