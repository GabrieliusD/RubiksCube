#include "RubikManager.h"
#include <RenderSystem.h>
#include <D3DApp.h>

RubikManager::RubikManager() :
    m_Front{
        {
            { 0, 0, 0 },
            { 0, 0, 1 },
            { 0, 0, 2 },
            { 0, 1, 2 },
            { 0, 2, 2 },
            { 0, 2, 1 },
            { 0, 2, 0 },
            { 0, 1, 0 }
        },
        { 0, 1, 1 },
    { 'F' },
    { DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f) } }
    , m_Back{
        {
            { 2, 0, 2 },
            { 2, 0, 1 },
            { 2, 0, 0 },
            { 2, 1, 0 },
            { 2, 2, 0 },
            { 2, 2, 1 },
            { 2, 2, 2 },
            { 2, 1, 2 }
        },
        { 2, 1, 1 },
    { 'B' },
    { DirectX::XMFLOAT3(0.0f, 0.0f, -1.0f) } }
    , m_Down{
        {
            {2, 0, 0},
            {1, 0, 0},
            {0, 0, 0},
            {0, 1, 0},
            {0, 2, 0},
            {1, 2, 0},
            {2, 2, 0},
            {2, 1, 0}
        },
        { 1, 1, 0 },
    { 'D' },
    { DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f) } }
    , m_Up{
        {
            {0, 0, 2},
            {1, 0, 2},
            {2, 0, 2},
            {2, 1, 2},
            {2, 2, 2},
            {1, 2, 2},
            {0, 2, 2},
            {0, 1, 2}
        },
        { 1, 1, 2 },
    { 'U' },
    { DirectX::XMFLOAT3(0.0f, -1.0f, 0.0f) } }
    , m_Right{
        {
            {2, 0, 0},
            {2, 0, 1},
            {2, 0, 2},
            {1, 0, 2},
            {0, 0, 2},
            {0, 0, 1},
            {0, 0, 0},
            {1, 0, 0}
        },
        { 1, 0, 1 },
    { 'R' },
    { DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f) } }
    , m_Left{
        {
            {1, 2, 2},
            {2, 2, 2},
            {2, 2, 1},
            {2, 2, 0},
            {1, 2, 0},
            {0, 2, 0},
            {0, 2, 1},
            {0, 2, 2}
        },
        { 1, 2, 1 },
    { 'L' },
    { DirectX::XMFLOAT3(-1.0f, 0.0f, 0.0f) } }
{

    Initialize();
}

void RubikManager::Initialize()
{
    D3DApp* app = D3DApp::GetApp();
    auto renderSystem = app->GetRenderSystem();
    auto& coordinator = app->GetScene().GetCoordinator();
    float offsetY = 0;
    float offsetZ = 10;
    for (int x = 0; x < 3; x++)
    {
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                auto test = coordinator.CreateEntity();
                Transform transform;
                transform.scale = XMFLOAT3(1, 1, 1);
                transform.position = XMFLOAT3(i * 2 - 2, j * 2 - 2 + offsetY,x * -2 + offsetZ);
                coordinator.AddComponent<Transform>(test, transform);
                Renderable renderable;
                renderable.geometry = app->geometries["Cube"].get();
                renderable.material = renderSystem->GetMaterial("grass");
                coordinator.AddComponent<Renderable>(test, renderable);
            }
        }
    }
}
