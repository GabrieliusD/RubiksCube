// Sandbox.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>

enum FaceDirection
{
    front,
    back,
    top,
    down,
    left,
    right
};

struct Face {
    size_t CubiesAround[8][3];
    size_t Center[3];
    //glm::vec3 Axis;
    const std::string Siganture;
};

class RubikCube {
public:
    RubikCube();
    Face m_Front, m_Back, m_Left, m_Right, m_Up, m_Down;

};

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
        //{ glm::vec3(1.0f, 0.0f, 0.0f) },
        { 'F' } }
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
        //{ glm::vec3(-1.0f, 0.0f, 0.0f) },
        { 'B' } }
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
        //{ glm::vec3(0.0f, 0.0f, 1.0f) },
        { 'L' } }
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
        //{ glm::vec3(0.0f, 0.0f, -1.0f) },
        { 'R' } }
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
        //{ glm::vec3(0.0f, 1.0f, 0.0f) },
        { 'U' } }
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
        //{ glm::vec3(0.0f, -1.0f, 0.0f) },
        { 'D' } }
{}

enum EColor
{
    B, G, R, Y, O, P
};


struct Cube
{
    
};
char Cubes[3][3][3];
int CubiesAround[6][8];



void swap(char& a, char& b)
{
    char temp = a;
    a = b;
    b = temp;
}

char tile[3][3] = { { 'A', 'A', 'A' },
                    { 'B', 'B', 'B' },
                    { 'C', 'C', 'C' } };




void EmplaceCharacter()
{
    Cubes[0][0][0] = 'A';
    Cubes[0][0][1] = 'A';
    Cubes[0][0][2] = 'A';
    // Second r[]
    Cubes[0][1][0] = 'A';
    Cubes[0][1][1] = 'A';
    Cubes[0][1][2] = 'A';
    // Third ro[]
    Cubes[0][2][0] = 'A';
    Cubes[0][2][1] = 'A';
    Cubes[0][2][2] = 'A';
               
    // Back fac
    // First ro
    Cubes[2][0][0] = 'B';
    Cubes[2][0][1] = 'B';
    Cubes[2][0][2] = 'B';
               
    // Second r[]
    Cubes[2][1][0] = 'B';
    Cubes[2][1][1] = 'B';
    Cubes[2][1][2] = 'B';
               
    // Third ro[]
    Cubes[2][2][0] = 'B';
    Cubes[2][2][1] = 'B';
    Cubes[2][2][2] = 'B';
               
               
    // Middle f[]
    // First ro[]
    Cubes[1][0][0] = 'C';
    Cubes[1][0][1] = 'C';
    Cubes[1][0][2] = 'C';
               
    // Second r[]
    Cubes[1][1][0] = 'C';
    Cubes[1][1][1] = 'C';
    Cubes[1][1][2] = 'C';
               
    // Third ro[]
    Cubes[1][2][0] = 'C';
    Cubes[1][2][1] = 'C';
    Cubes[1][2][2] = 'C';

}

void RotateFace(RubikCube& owner, Face& face)
{
    // GCD left-shift array shift
    const int size = 8;
    const int shift = 2;   // For clockwise rotation right-shift by 2 is equal to left-shift by 6. For counter clockwise rotation left-shift by 2.
    const int gcd = 2;                                                          // gcd(2, 8) = gcd(6, 8) = 2
    for (int i = 0; i < gcd; i++) {
        char tmp = Cubes[face.CubiesAround[i][0]][face.CubiesAround[i][1]][face.CubiesAround[i][2]];
        int j = i;

        while (true) {
            int k = j + shift;
            if (k >= size) {
                k = k - size;
            }
            if (k == i) {
                break;
            }

            Cubes[face.CubiesAround[j][0]][face.CubiesAround[j][1]][face.CubiesAround[j][2]] = Cubes[face.CubiesAround[k][0]][face.CubiesAround[k][1]][face.CubiesAround[k][2]];
            j = k;
        }
        Cubes[face.CubiesAround[j][0]][face.CubiesAround[j][1]][face.CubiesAround[j][2]] = tmp;
    }
}

class Tester
{
public:
    void Hello() { std::cout << "Hello" << std::endl; }
};

template<typename T>
class MyCustomTemplate
{
    T MyVariable;

public:
    T GetState() { return MyVariable; }

    void DoStuff() {
        MyVariable.Hello();
    }
};

int main()
{
    RubikCube rubikCube;
    EmplaceCharacter();

    MyCustomTemplate<Tester> myCustomTemplate;
    myCustomTemplate.DoStuff();

    std::cout << "hi" << std::endl;
}



// Run program: Ctrl + F5 or Debug > Start Without Debugging menu
// Debug program: F5 or Debug > Start Debugging menu

// Tips for Getting Started: 
//   1. Use the Solution Explorer window to add/manage files
//   2. Use the Team Explorer window to connect to source control
//   3. Use the Output window to see build output and other messages
//   4. Use the Error List window to view errors
//   5. Go to Project > Add New Item to create new code files, or Project > Add Existing Item to add existing code files to the project
//   6. In the future, to open this project again, go to File > Open > Project and select the .sln file
