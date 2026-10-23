#include "ffx_sc/linux_host_app.hpp"

#include <cstdlib>
#include <iostream>

int main(int argc, const char* const* argv)
{
    ffx_sc::LinuxHostApp host{};

    if (auto exitCode = host.bootstrapFromProcessArguments(argc, argv))
    {
        return *exitCode;
    }

    const auto request = host.prepareCompilation();
    const int  result  = host.runCompilationPipeline(request);
    if (result != EXIT_SUCCESS)
    {
        std::cerr << "FidelityFX_SC_linux failed with exit code " << result << "\n";
        return result;
    }

    std::cerr << "FidelityFX_SC_linux completed successfully." << '\n';
    return EXIT_SUCCESS;
}
