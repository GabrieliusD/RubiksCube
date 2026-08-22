#include "Rubik.h"
#include <GameModuleAPI.h>

CHERRY_GAME_EXPORT D3DApp* CreateApplication()
{
	return new RubikApp();
}
