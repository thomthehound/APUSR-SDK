#include "ffx_sc/linux_host_app.hpp"

#include "ffx_sc/entry.hpp"
#include "ffx_sc/utils.hpp"

#include <iostream>

namespace ffx_sc
{
    namespace
    {
        void logTrace(std::string_view message)
        {
            std::cerr << "[ffx-sc-linux] " << message << '\n';
        }
    } // namespace

    LinuxHostApp::LinuxHostApp() = default;

    std::optional<int> LinuxHostApp::bootstrapFromProcessArguments(int argc, const char* const* argv)
    {
        m_rawArguments.clear();
        m_wideArguments.clear();

        if (argc <= 1)
        {
            logTrace("No arguments supplied; printing usage.");
            const int rc = RunFfxShaderCompilerCatching(0, nullptr);
            return rc;
        }

        m_rawArguments.reserve(static_cast<size_t>(argc - 1));
        m_wideArguments.reserve(static_cast<size_t>(argc - 1));

        for (int i = 1; i < argc; ++i)
        {
            m_rawArguments.emplace_back(argv[i]);
            m_wideArguments.emplace_back(UTF8ToWChar(argv[i]));
        }

        if (!m_rawArguments.empty() && (m_rawArguments.front() == "--help" || m_rawArguments.front() == "-h"))
        {
            logTrace("Help requested; printing usage.");
            const int rc = RunFfxShaderCompilerCatching(0, nullptr);
            return rc;
        }

        return std::nullopt;
    }

    CompilationRequest LinuxHostApp::prepareCompilation() const
    {
        CompilationRequest request{};
        request.commandLineTokens = m_rawArguments;
        request.wideArguments     = m_wideArguments;
        return request;
    }

    int LinuxHostApp::runCompilationPipeline(const CompilationRequest& request) const
    {
        std::vector<const wchar_t*> argv{};
        argv.reserve(request.wideArguments.size());
        for (const auto& arg : request.wideArguments)
        {
            argv.push_back(arg.c_str());
        }

        logTrace("Invoking FidelityFX shader compiler core.");
        return RunFfxShaderCompilerCatching(static_cast<int>(argv.size()), argv.data());
    }
}
