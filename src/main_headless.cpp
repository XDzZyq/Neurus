#include "app/HeadlessApplication.h"

#include "platform/PlatformProcess.h"

#include <exception>
#include <iostream>

/**
 * @brief Entry point of the headless (windowless) Neurus renderer.
 *
 * Driven exclusively through the `neurus` CLI: `neurus open` spawns this binary
 * with `--session`, and every later command reaches it over loopback.
 */
int main(int argc, char* argv[])
{
	try
	{
		neurus::HeadlessApplication app(neurus::CommandLineArguments(argc, argv));
		return app.Run();
	}
	catch (const std::exception& e)
	{
		std::cerr << "[neurus_headless] Fatal: " << e.what() << "\n";
		return 1;
	}
}
