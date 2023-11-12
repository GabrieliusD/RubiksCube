#include "RenderObject.h"
#include "D3DApp.h"
#include <iostream>

RubikCube::RubikCube() :
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
    { DirectX::XMFLOAT3(0.0f, 0.0f, 1.0f) }}
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
    { DirectX::XMFLOAT3(0.0f, 0.0f, -1.0f) }}
    , m_Left{
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
    { 'L' },
    { DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f) }}
    , m_Right{
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
    { 'R' },
    { DirectX::XMFLOAT3(0.0f, -1.0f, 0.0f) } }
    , m_Up{
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
    { 'U' },
    { DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f) }}
    , m_Down{
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
    { 'D' },
    { DirectX::XMFLOAT3(-1.0f, 0.0f, 0.0f) }}
{
    Faces.push_back(m_Front);
    Faces.push_back(m_Back);
    Faces.push_back(m_Down);
    Faces.push_back(m_Up);
    Faces.push_back(m_Left);
    Faces.push_back(m_Right);
}

void RubikCube::Initialize(D3DApp* app, UINT cbIndex)
{
    //todo 
    //Store an index of the cube in render items
    //use that index to check which face the cube belongs to
    //If the face is found use the face object to get other cubes
    XMMATRIX world = XMMatrixTranslation(-2, 0, 0);
    // first row
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            std::unique_ptr<Cube> renderObjectCube = std::make_unique<Cube>();
            renderObjectCube->ConstantBufferIndex = cbIndex;
            renderObjectCube->Geometry = app->Geometries["Cube"].get();
            renderObjectCube->IndexCount = app->Geometries["Cube"]->mIndexCount;
            renderObjectCube->Mat = app->mMaterials["grass"].get();
            renderObjectCube->SelectedMat = app->mMaterials["selectedCube"].get();
            renderObjectCube->Bounds = renderObjectCube->Geometry->Bounds;
            renderObjectCube->Name = "Front" + std::to_string(3 * i + j);

            world = XMMatrixTranslation(i * 2 - 2, j * 2 - 2, -2);
            XMStoreFloat4x4(&renderObjectCube->World, world);

            renderObjectCube->mIdx = 0;
            renderObjectCube->mIdy = i;
            renderObjectCube->mIdz = j;
            CubePtr[0][i][j] = renderObjectCube.get();
            Cubes[0][i][j] = std::move(renderObjectCube);
            cbIndex++;
        }
    }

    // Middle face
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {

            std::unique_ptr<Cube> renderObjectCube = std::make_unique<Cube>();
            renderObjectCube->ConstantBufferIndex = cbIndex;
            renderObjectCube->Geometry = app->Geometries["Cube"].get();
            renderObjectCube->IndexCount = app->Geometries["Cube"]->mIndexCount;
            renderObjectCube->Mat = app->mMaterials["grass"].get();
            renderObjectCube->SelectedMat = app->mMaterials["selectedCube"].get();
            renderObjectCube->Bounds = renderObjectCube->Geometry->Bounds;
            renderObjectCube->Name = "Middle" + std::to_string(i + j);

            world = XMMatrixTranslation(i * 2 - 2, j * 2 - 2, 0);
            XMStoreFloat4x4(&renderObjectCube->World, world);

            renderObjectCube->mIdx = 1;
            renderObjectCube->mIdy = i;
            renderObjectCube->mIdz = j;
            CubePtr[1][i][j] = renderObjectCube.get();
            Cubes[1][i][j] = std::move(renderObjectCube);
            cbIndex++;
        }
    }

    // Back fac
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {

            std::unique_ptr<Cube> renderObjectCube = std::make_unique<Cube>();
            renderObjectCube->ConstantBufferIndex = cbIndex;
            renderObjectCube->Geometry = app->Geometries["Cube"].get();
            renderObjectCube->IndexCount = app->Geometries["Cube"]->mIndexCount;
            renderObjectCube->Mat = app->mMaterials["grass"].get();
            renderObjectCube->SelectedMat = app->mMaterials["selectedCube"].get();
            renderObjectCube->Bounds = renderObjectCube->Geometry->Bounds;
            renderObjectCube->Name = "Back" + std::to_string(i + j);

            world = XMMatrixTranslation(i * 2 - 2, j * 2 - 2, 2);
            XMStoreFloat4x4(&renderObjectCube->World, world);

            renderObjectCube->mIdx = 2;
            renderObjectCube->mIdy = i;
            renderObjectCube->mIdz = j;
            CubePtr[2][i][j] = renderObjectCube.get();
            Cubes[2][i][j] = std::move(renderObjectCube);
            cbIndex++;
        }
    }

}

void RubikCube::BuildRenderObjects(UINT cbIndex)
{

}

std::vector<std::unique_ptr<RenderObject>> RubikCube::GetRenderObjects()
{
    std::vector<std::unique_ptr<RenderObject>> TempCubes;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            for (int k = 0; k < 3; k++)
            {
                if(Cubes[i][j][k])
                    TempCubes.push_back(std::move(Cubes[i][j][k]));
            }
        }
    }
    return TempCubes;
}

void RubikCube::RotateFace(FaceDirection faceDir, RotationDirection rotationDir)
{
    Face face = *m_SelectedFace;
    const int size = 8;
    const int shift = rotationDir == RotationDirection::Clockwise ? 2 : 6 ;   // For clockwise rotation right-shift by 2 is equal to left-shift by 6. For counter clockwise rotation left-shift by 2.
    const int gcd = 2;                                                          // gcd(2, 8) = gcd(6, 8) = 2
    for (int i = 0; i < gcd; i++) {
        auto tmp = (CubePtr[face.CubiesAround[i][0]][face.CubiesAround[i][1]][face.CubiesAround[i][2]]);
        int j = i;

        while (true) {
            int k = j + shift;
            if (k >= size) {
                k = k - size;
            }
            if (k == i) {
                break;
            }

            CubePtr[face.CubiesAround[j][0]][face.CubiesAround[j][1]][face.CubiesAround[j][2]] = (CubePtr[face.CubiesAround[k][0]][face.CubiesAround[k][1]][face.CubiesAround[k][2]]);
            j = k;
        }
        CubePtr[face.CubiesAround[j][0]][face.CubiesAround[j][1]][face.CubiesAround[j][2]] = (tmp);
    }
}

void RubikCube::GetAdjecantCubes(int idx, int idy, int idz)
{
    //find a face the selected cube belongs to

    for (int i = 0; i != Faces.size(); i++)
    {
        size_t ids[3] = {idx, idy, idz };
        if (Faces[i].Center[0] == idx && Faces[i].Center[1] == idy && Faces[i].Center[2] == idz)
        {
            std::cout << "Face found" << std::endl;
            m_SelectedFace = &Faces[i];
            SelectCubesFromFace(Faces[i]);
            //face found
        }
    }

    
}

void RubikCube::DeselectCubes()
{
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            for (int k = 0; k < 3; k++)
            {
                if (CubePtr[i][j][k])
                {
                    CubePtr[i][j][k]->mIsSelected = false;
                }
            }
        }
    }
}

void RubikCube::SelectCubesFromFace(Face& face)
{
    for (int i = 0; i < 8; i++)
    {
        int x = face.CubiesAround[i][0];
        int y = face.CubiesAround[i][1];
        int z = face.CubiesAround[i][2];

        CubePtr[x][y][z]->mIsSelected = true;
    }

    CubePtr[face.Center[0]][face.Center[1]][face.Center[2]]->mIsSelected = true;
}

void RubikCube::RotateSelectedFace(RotationDirection rotationDir)
{
    if (m_SelectedFace)
    {
        for (int i = 0; i < 8; i++)
        {
            int x = m_SelectedFace->CubiesAround[i][0];
            int y = m_SelectedFace->CubiesAround[i][1];
            int z = m_SelectedFace->CubiesAround[i][2];



            switch (rotationDir)
            {
            case RotationDirection::Clockwise:
                XMMATRIX tempWorld = XMLoadFloat4x4(&CubePtr[x][y][z]->World);
                XMVECTOR trans;
                XMVECTOR scale;
                XMVECTOR rot;
                XMMatrixDecompose(&scale, &rot, &trans, tempWorld);
                XMFLOAT3 transStored;

                tempWorld = tempWorld * XMMatrixRotationAxis(XMLoadFloat3(&m_SelectedFace->Axis), DirectX::XM_PIDIV2);
                XMStoreFloat4x4(&CubePtr[x][y][z]->World, tempWorld);
                
                break;
            default: break;
            }
        }
    }
}



