#include "Rubik.h"
#include <GeometryGenerator.h>
#include "RubikManager.h"

void RubikApp::OnAppInitialized()
{
	mRenderSystem->ResetCommandList();
	CreateTextures();
	CreateMesh();
	CreateMaterials();
	mRenderSystem->CmdListCloseAndExecute();
	mRenderSystem->FlushCommandQueue();

	CreateEntities();
}

void RubikApp::CreateTextures()
{
	mRenderSystem->CreateTexture("woodCrateTex", L"Textures/rubicPallet.dds");
}

void RubikApp::CreateMesh()
{
	Vertex vertices[] = {
		//right
		{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Red), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT2(0.0f, 0.25f)}, //bottom right
		{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Red), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT2(0.0f, 0.0f)}, //top right
		{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Red), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT2(0.25f, 0.0f)}, //top left
		{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::Red), XMFLOAT3(-1.0f, 0.0f, 0.0f), XMFLOAT2(0.25f, 0.25f)}, //bottom left
		//front
		{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::Green), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.5f, 0.0f)}, //top left
		{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Green), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.25f, 0.25f)}, //bottom right
		{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::Green), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.25f, 0.0f)}, //top right
		{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Green), XMFLOAT3(0.0f, 0.0f, -1.0f), XMFLOAT2(0.5f, 0.25f)}, //bottom left
		//bottom
		{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Yellow), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.5f, 0.25f)}, //bottom right
		{XMFLOAT3(-1.0f,-1.0f,-1.0f), XMFLOAT4(Colors::Yellow), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.75f, 0.0f)}, //top left
		{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Yellow), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.5f, 0.0f)}, //top right
		{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Yellow), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.75f, 0.25f)}, //bottom left
		//left
		{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Orange), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(0.75f, 0.25f)}, //bottom right
		{XMFLOAT3(1.0f,-1.0f,-1.0f),  XMFLOAT4(Colors::Orange), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(1.0f, 0.0f)}, //top left
		{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::Orange), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(0.75f, 0.0f)}, //top right
		{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Orange), XMFLOAT3(1.0f, 0.0f, 0.0f), XMFLOAT2(1.0f, 0.25f)}, //bottom left
		//top
		{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::White), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.25f)}, //top left
		{XMFLOAT3(1.0f, 1.0f,-1.0f),  XMFLOAT4(Colors::White), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.5f)}, //bottom left
		{XMFLOAT3(-1.0f, 1.0f,-1.0f), XMFLOAT4(Colors::White), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.25f, 0.5f)}, //bottom right
		{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::White), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.25f, 0.25f)}, //top right
		//back
		{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT4(Colors::Blue), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.25f, 0.5f)}, //bottom left
		{XMFLOAT3(-1.0f,-1.0f, 1.0f), XMFLOAT4(Colors::Blue), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.25f, 0.25f)}, //top left
		{XMFLOAT3(1.0f,-1.0f, 1.0f),  XMFLOAT4(Colors::Blue), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.5f, 0.25f)}, //top right
		{XMFLOAT3(1.0f, 1.0f, 1.0f),  XMFLOAT4(Colors::Blue), XMFLOAT3(0.0f, 0.0f, 1.0f), XMFLOAT2(0.5f, 0.5f)}, //bottom right
	};

	const UINT64 vbByteSize = 24 * sizeof(Vertex);

	//indices
	std::uint32_t indices[] = {
		// right face
		0, 1, 2,
		0, 2, 3,
		// front face
		4, 6, 5,
		4, 5, 7,
		// bottom face
		8, 10, 9,
		8, 9, 11,
		// left face
		12, 14, 13,
		12, 13, 15,
		// top face
		19, 18, 16,
		16, 18, 17,
		// back face
		20, 22, 21,
		20, 23, 22
	};

	const UINT ibByteSize = 36 * sizeof(std::uint32_t);

	auto commandList = mRenderSystem->GetCommandList();
	auto device = mRenderSystem->GetDevice();

	std::unique_ptr<Geometry> Cube(new Geometry());
	Cube->vertexBuffer = d3dUtil::CreateDefaultBuffer(device.Get(), commandList.Get(), vertices, vbByteSize, Cube->vertexUploadBuffer);
	Cube->indexBuffer = d3dUtil::CreateDefaultBuffer(device.Get(), commandList.Get(), indices, ibByteSize, Cube->indexUploadBuffer);
	Cube->ibFormat = DXGI_FORMAT_R32_UINT;
	Cube->ibByteSize = ibByteSize;
	Cube->strideInBytes = sizeof(Vertex);
	Cube->vbByteSize = vbByteSize;
	Cube->indexCount = 36;

	BoundingBox bounds;
	XMFLOAT3 vMinf3(-1.0f, -1.0f, -1.0f);
	XMFLOAT3 vMaxf3(1.0f, 1.0f, 1.0f);
	XMVECTOR vMin = XMLoadFloat3(&vMinf3);
	XMVECTOR vMax = XMLoadFloat3(&vMaxf3);
	XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
	XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));

	Cube->bounds = bounds;
	geometries.emplace("Cube", std::move(Cube));
	GeometryGenerator geometryGenerator;
	GeometryGenerator::MeshData sphereMesh = geometryGenerator.CreateSphere(100, 5, 5);

	std::unique_ptr<Geometry> sphereGeo = std::make_unique<Geometry>();
	std::vector<Vertex> sphereVertices(sphereMesh.Vertices.size());
	for (int i = 0; i != sphereVertices.size(); i++)
	{
		sphereVertices[i].Pos = sphereMesh.Vertices[i].Position;
		sphereVertices[i].Normal = sphereMesh.Vertices[i].Normal;
		sphereVertices[i].Color = XMFLOAT4(Colors::White);
		sphereVertices[i].TexC = sphereMesh.Vertices[i].TexC;
	}

	float sphereVbByteSize = sphereMesh.Vertices.size() * sizeof(Vertex);
	float sphereIbByteSize = sphereMesh.GetIndices16().size() * sizeof(std::uint16_t);

	sphereGeo->vertexBuffer = d3dUtil::CreateDefaultBuffer(device.Get(), commandList.Get(), sphereVertices.data(), sphereVbByteSize, sphereGeo->vertexUploadBuffer);
	sphereGeo->indexBuffer = d3dUtil::CreateDefaultBuffer(device.Get(), commandList.Get(), sphereMesh.GetIndices16().data(), sphereIbByteSize, sphereGeo->indexUploadBuffer);
	sphereGeo->ibFormat = DXGI_FORMAT_R16_UINT;
	sphereGeo->ibByteSize = sphereIbByteSize;
	sphereGeo->strideInBytes = sizeof(Vertex);
	sphereGeo->vbByteSize = sphereVbByteSize;
	sphereGeo->indexCount = sphereMesh.GetIndices16().size();
	geometries.emplace("Sphere", std::move(sphereGeo));
}

void RubikApp::CreateMaterials()
{
	mRenderSystem->CreateMaterial("grass", XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f), XMFLOAT3(0.05f, 0.05f, 0.05f), 0.3f);
	mRenderSystem->CreateMaterial("water", XMFLOAT4(0.0f, 0.2f, 0.6f, 1.0f), XMFLOAT3(0.1f, 0.1f, 0.1f), 0.0f);
	mRenderSystem->CreateMaterial("sky", XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f), XMFLOAT3(0.1f, 0.1f, 0.1f), 0.0f);
	mRenderSystem->CreateMaterial("selectedCube", XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f), XMFLOAT3(0.1f, 0.1f, 0.1f), 0.0f);
}

void RubikApp::CreateEntities()
{
	auto& coordinator = GetScene().GetCoordinator();
	auto test = coordinator.CreateEntity();
	Transform transform;
	transform.scale = XMFLOAT3(1, 1, 1);
	transform.position = XMFLOAT3(2, 2, 10);
	coordinator.AddComponent<Transform>(test, transform);
	Renderable renderable;
	renderable.geometry = geometries["Cube"].get();
	renderable.material = mRenderSystem->GetMaterial("grass");
	coordinator.AddComponent<Renderable>(test, renderable);

	RubikManager rubikManager;
}
