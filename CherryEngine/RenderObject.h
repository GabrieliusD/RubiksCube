#pragma once
#include "d3dUtil.h"
#include "Geometry.h"
struct RenderObject
{
	DirectX::XMFLOAT4X4 world = MathHelper::Identity4x4();
	Geometry* geometry = nullptr;
	UINT constantBufferIndex = -1;
	D3D12_PRIMITIVE_TOPOLOGY primitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	UINT indexCount = 0;
	Material* material;
    DirectX::BoundingBox bounds;
    std::string name;

    virtual int GetMatIndex() { return material->MatCBIndex; }
};

struct Cube : public RenderObject
{
    int idx, idy, idz;
    bool isSelected = false;
    Material* SelectedMat;
    virtual int GetMatIndex() override { 
        if (isSelected)
        {
            return SelectedMat->MatCBIndex;
        }
        else
        {
            material->MatCBIndex;
        }
    }
};

class RubikCube {
public:
    enum FaceDirection
    {
        front,
        back,
        left,
        right,
        up,
        down
    };

    enum RotationDirection
    {
        Clockwise,
        Anticlockwise
    };

    struct Face {
        size_t CubiesAround[8][3];
        size_t Center[3];
        const std::string Siganture;
        DirectX::XMFLOAT3 Axis;
    };

    RubikCube();


public:
    void Initialize(UINT& cbIndex);
    void BuildRenderObjects(UINT cbIndex);
    std::vector<std::unique_ptr<RenderObject>> GetRenderObjects();
    void RotateFaceData(Face* face, RotationDirection rotationDir);
    void GetAdjecantCubes(int idx, int idy, int idz);
    void DeselectCubes();
    void SelectCubesFromFace(Face& face);
    void RotateFaceVisual(Face* face, RotationDirection rotationDir);
    void CheckWinCondition();
    void Scramble(std::string ScrambleOrder);
    std::string GenerateRandomScrambleString(int size);
    Face& FindFaceByLetter(std::string faceLetter);
    void Rotate(RotationDirection rotationDir);

    inline bool GetVictory() { return mVictory; }
    inline void SetVictory(bool victory) { mVictory = victory;  }

    void SaveGame(std::string saveName);
    void LoadGame(std::string loadName);

    void Reset();

    std::vector<Cube*> GetAllCubies();
private:
    Face m_Front, m_Back, m_Left, m_Right, m_Up, m_Down;
    Face* m_SelectedFace;
    std::vector<Face> Faces;
    std::unique_ptr<RenderObject> Cubes[3][3][3];
    Cube* CubePtr[3][3][3];
    bool mVictory;

private:
    void ResetCubeData();
    void ResetCubePos();
};


