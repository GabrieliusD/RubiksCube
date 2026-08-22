#pragma once

#include "D3DApp.h"

#if defined(_WIN32)
#define CHERRY_GAME_EXPORT extern "C" __declspec(dllexport)
#else
#define CHERRY_GAME_EXPORT extern "C"
#endif

using CreateApplicationFn = D3DApp*(*)();
