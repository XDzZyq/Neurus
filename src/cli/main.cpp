#include "cli/Cli.h"

#include "platform/PlatformProcess.h"

int main(int argc, char* argv[])
{
	return neurus::cli::RunCli(neurus::CommandLineArguments(argc, argv));
}
