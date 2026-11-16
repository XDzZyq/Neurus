#include "cli/CliMain.hpp"

#include "platform/PlatformProcess.hpp"

int main(int argc, char* argv[])
{
	return neurus::cli::RunCli(neurus::CommandLineArguments(argc, argv));
}
