/**
 * @file main.cpp
 * @brief Application entry point.
 */

#include "app/Application.h"

int main(int argc, char* argv[])
{
	neurus::Application app(argc, argv);
	return app.Run();
}
