#pragma once

#ifdef CE_BUILD_DLL
#define CHERRY_ENGINE_API __declspec(dllexport)
#else
#define CHERRY_ENGINE_API __declspec(dllimport)
#endif