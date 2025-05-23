#include "D3DApp.h"

int main()
{
	D3DApp* app = CreateApplication();
	app->Run();
	delete app;
}