#include "D3DApp.h"

int main()
{
	D3DApp myApp;
	myApp.InitWindow();
	myApp.InitDirectX();
	myApp.Run();
	return 0;
}