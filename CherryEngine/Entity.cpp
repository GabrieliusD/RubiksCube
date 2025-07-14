#include "Entity.h"

Coordinator& Coordinator::GetSingelton()
{
	static Coordinator coordinator;
	coordinator.Init();
	return coordinator;
}