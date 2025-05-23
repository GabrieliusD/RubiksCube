#pragma once
#include <EntryPoint.h>
#include <Entity.h>
#include <RenderSystem.h>

extern Coordinator gCoordinator;

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

D3DApp* CreateApplication()
{
	return new RubikApp();
}