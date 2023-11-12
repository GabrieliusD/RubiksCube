#pragma once
#include "d3dUtil.h"
#include "Geometry.h"
struct RenderObject
{
	DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
	Geometry* Geometry = nullptr;
	UINT ConstantBufferIndex = -1;
	D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	UINT IndexCount = 0;
	Material* Mat;
    DirectX::BoundingBox Bounds;
    std::string Name;

    virtual int GetMatIndex() { return Mat->MatCBIndex; }
};

struct Cube : public RenderObject
{
    int mIdx, mIdy, mIdz;
    bool mIsSelected = false;
    Material* SelectedMat;
    virtual int GetMatIndex() override { 
        if (mIsSelected)
        {
            return SelectedMat->MatCBIndex;
        }
        else
        {
            Mat->MatCBIndex;
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
    Face m_Front, m_Back, m_Left, m_Right, m_Up, m_Down;
    Face* m_SelectedFace;
    std::vector<Face> Faces;
    std::unique_ptr<RenderObject> Cubes[3][3][3];
    Cube* CubePtr[3][3][3];

public:
    void Initialize(class D3DApp* app, UINT cbIndex);
    void BuildRenderObjects(UINT cbIndex);
    std::vector<std::unique_ptr<RenderObject>> GetRenderObjects();
    void RotateFace(FaceDirection face, RotationDirection rotationDir);
    void GetAdjecantCubes(int idx, int idy, int idz);
    void DeselectCubes();
    void SelectCubesFromFace(Face& face);
    void RotateSelectedFace(RotationDirection rotationDir);
};


