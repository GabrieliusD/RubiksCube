#pragma once

#if defined(_WIN32)
#if defined(CHERRYENGINE_EXPORTS)
#define CHERRY_API __declspec(dllexport)
#else
#define CHERRY_API __declspec(dllimport)
#endif
#else
#define CHERRY_API
#endif
