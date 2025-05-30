#pragma once
#include <string>
#include <vector>
#include <memory>
#include <d3dUtil.h>
#include <Cube.h>

class RubikManager
{
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

    RubikManager();


public:
    void Initialize();
private:
    Face m_Front, m_Back, m_Left, m_Right, m_Up, m_Down;
    Face* m_SelectedFace;
};