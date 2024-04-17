#include "RenderObject.h"
#include "D3DApp.h"
#include <iostream>
#include <random>

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
    { DirectX::XMFLOAT3(0.0f, 1.0f, 0.0f) }}
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
    { DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f) }}
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
    { DirectX::XMFLOAT3(-1.0f, 0.0f, 0.0f) }}
{
    Faces.push_back(m_Front);
    Faces.push_back(m_Back);
    Faces.push_back(m_Down);
    Faces.push_back(m_Up);
    Faces.push_back(m_Left);
    Faces.push_back(m_Right);
}

void RubikCube::Initialize(UINT &cbIndex)
{
    D3DApp* app = D3DApp::GetApp();
    //todo 
    //Store an index of the cube in render items
    //use that index to check which face the cube belongs to
    //If the face is found use the face object to get other cubes
    XMMATRIX world = XMMatrixTranslation(-2, 0, 0);
    float offsetY = 40;
    float offsetZ = -20;
    // first row
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            std::unique_ptr<Cube> renderObjectCube = std::make_unique<Cube>();
            renderObjectCube->constantBufferIndex = cbIndex;
            renderObjectCube->geometry = app->geometries["Cube"].get();
            renderObjectCube->indexCount = app->geometries["Cube"]->indexCount;
            renderObjectCube->material = app->materials["grass"].get();
            renderObjectCube->SelectedMat = app->materials["selectedCube"].get();
            renderObjectCube->bounds = renderObjectCube->geometry->bounds;
            renderObjectCube->name = "Front" + std::to_string(3 * i + j);

            world = XMMatrixTranslation(i * 2 - 2, j * 2 - 2 + offsetY, -2 + offsetZ);
            XMStoreFloat4x4(&renderObjectCube->world, world);

            renderObjectCube->idx = 0;
            renderObjectCube->idy = i;
            renderObjectCube->idz = j;
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
            renderObjectCube->constantBufferIndex = cbIndex;
            renderObjectCube->geometry = app->geometries["Cube"].get();
            renderObjectCube->indexCount = app->geometries["Cube"]->indexCount;
            renderObjectCube->material = app->materials["grass"].get();
            renderObjectCube->SelectedMat = app->materials["selectedCube"].get();
            renderObjectCube->bounds = renderObjectCube->geometry->bounds;
            renderObjectCube->name = "Middle" + std::to_string(i + j);

            world = XMMatrixTranslation(i * 2 - 2, j * 2 - 2 + offsetY, 0 + offsetZ);
            XMStoreFloat4x4(&renderObjectCube->world, world);

            renderObjectCube->idx = 1;
            renderObjectCube->idy = i;
            renderObjectCube->idz = j;
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
            renderObjectCube->constantBufferIndex = cbIndex;
            renderObjectCube->geometry = app->geometries["Cube"].get();
            renderObjectCube->indexCount = app->geometries["Cube"]->indexCount;
            renderObjectCube->material = app->materials["grass"].get();
            renderObjectCube->SelectedMat = app->materials["selectedCube"].get();
            renderObjectCube->bounds = renderObjectCube->geometry->bounds;
            renderObjectCube->name = "Back" + std::to_string(i + j);

            world = XMMatrixTranslation(i * 2 - 2, j * 2 - 2 + offsetY, 2 + offsetZ);
            XMStoreFloat4x4(&renderObjectCube->world, world);

            renderObjectCube->idx = 2;
            renderObjectCube->idy = i;
            renderObjectCube->idz = j;
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

void RubikCube::RotateFaceData(Face* tempFace, RotationDirection rotationDir)
{
    Face face = *tempFace;
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
            std::cout << "Face found: " << Faces[i].Siganture << std::endl;
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
                    CubePtr[i][j][k]->isSelected = false;
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

        CubePtr[x][y][z]->isSelected = true;
    }

    CubePtr[face.Center[0]][face.Center[1]][face.Center[2]]->isSelected = true;
}

void RubikCube::RotateFaceVisual(Face* face, RotationDirection rotationDir)
{
    if (face)
    {
        for (int i = 0; i < 8; i++)
        {
            int x = face->CubiesAround[i][0];
            int y = face->CubiesAround[i][1];
            int z = face->CubiesAround[i][2];

            XMMATRIX tempWorld = XMLoadFloat4x4(&CubePtr[x][y][z]->world);
            XMVECTOR trans;
            XMVECTOR scale;
            XMVECTOR rot;
            XMMatrixDecompose(&scale, &rot, &trans, tempWorld);

            switch (rotationDir)
            {
            case RotationDirection::Clockwise:
                tempWorld = tempWorld * XMMatrixRotationAxis(XMLoadFloat3(&face->Axis), DirectX::XM_PIDIV2);
                XMStoreFloat4x4(&CubePtr[x][y][z]->world, tempWorld);
                break;
            case RotationDirection::Anticlockwise:
                tempWorld = tempWorld * XMMatrixRotationAxis(XMLoadFloat3(&face->Axis), -DirectX::XM_PIDIV2);
                XMStoreFloat4x4(&CubePtr[x][y][z]->world, tempWorld);
                break;
            default: break;
            }
        }
    }
}

void RubikCube::CheckWinCondition()
{
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            for (int k = 0; k < 3; k++)
            {
                auto cube = CubePtr[i][j][k];
                if (cube->idx != i || cube->idy != j || cube->idz != k)
                {
                    std::cout << "not all cubes are in the correct place" << std::endl;
                    return;
                }
            }
        }
    }

    SetVictory(true);
}

void RubikCube::Scramble(std::string ScrambleOrder)
{
    //' anticlockwise
    //U'4D2 F B L R 

    std::vector<std::string> Commands;
    std::string TempCommand = "";
    bool FirstLetter = false;
    for (int i = 0; i < ScrambleOrder.size(); i++)
    {
        char Letter = ScrambleOrder[i];
        if (std::isalpha(Letter) && FirstLetter)
        {
            FirstLetter = false;
            Commands.push_back(TempCommand);
            TempCommand.clear();
        }
        if (std::isalpha(Letter) && !FirstLetter)
        {
            FirstLetter = true;
            TempCommand += Letter;
            continue;
        }
        if (Letter == '\'')
        {
            TempCommand += Letter;
            continue;
        }
        if (std::isdigit(Letter))
        {
            TempCommand += Letter;
            continue;
        }

    }

    Commands.push_back(TempCommand);

    for (std::string command : Commands)
    {
        std::cout << command << std::endl;
        std::string letter = "";
        RotationDirection rotDir = Clockwise;
        int numRotations = 1;

        letter += command[0];
        Face& face = FindFaceByLetter(letter);
        if (command.length() == 2 && command[1] == '\'')
        {
            rotDir = Anticlockwise;
        }
        else if (command.length() == 2 && std::isdigit(command[1]))
        {
            std::string num{ command[1] };
            numRotations = std::stoi(num);
        }
        if (command.length() == 3 && std::isdigit(command[2]))
        {
            std::string num{ command[2] };
            numRotations = std::stoi(num);
        }

        for (int i = 0; i < numRotations; i++)
        {
            RotateFaceVisual(&face, rotDir);
            RotateFaceData(&face, rotDir);
        }
    }
}

std::string RubikCube::GenerateRandomScrambleString(int size)
{
    std::string faceLetters = "FRULD";
    int lastIndex = faceLetters.size() - 1;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> letterDistr(0, lastIndex);
    std::uniform_int_distribution<> backwardsDistr(0, 1);
    std::uniform_int_distribution<> rotationDistr(0, 3);

    std::string scramble;

    for (int i = 0; i != size; i++)
    {
        int randomLetter = letterDistr(gen);
        bool backwards = backwardsDistr(gen) == 0 ? true : false;
        int numRotations = rotationDistr(gen);

        scramble += faceLetters[randomLetter];
        scramble += backwards ? "'" : "";
        scramble += std::to_string(numRotations);
    }

    return scramble;
}

RubikCube::Face& RubikCube::FindFaceByLetter(std::string faceLetter)
{
    for (Face& face : Faces)
    {
        if (face.Siganture == faceLetter)
        {
            return face;
        }
    }

    return Face();
}

void RubikCube::Rotate(RotationDirection rotationDir)
{
    if (!m_SelectedFace) return;
    RotateFaceVisual(m_SelectedFace, rotationDir);
    RotateFaceData(m_SelectedFace, rotationDir);
    CheckWinCondition();
}

void RubikCube::Reset()
{

}

std::vector<Cube*> RubikCube::GetAllCubies()
{
    std::vector<Cube*> cubies;
    for (int i = 0; i < 3; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            for (int k = 0; k < 3; k++)
            {
                auto cube = CubePtr[i][j][k];
                cubies.push_back(cube);
            }
        }
    }
    return cubies;
}

void RubikCube::ResetCubeData()
{
}

void RubikCube::ResetCubePos()
{
}





