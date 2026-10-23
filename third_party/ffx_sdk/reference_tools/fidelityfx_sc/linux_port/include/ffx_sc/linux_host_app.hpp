#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ffx_sc
{
    namespace fs = std::filesystem;

    struct CompilationRequest
    {
        std::vector<std::string>  commandLineTokens;
        std::vector<std::wstring> wideArguments;
    };

    class LinuxHostApp
    {
    public:
        LinuxHostApp();

        [[nodiscard]] std::optional<int> bootstrapFromProcessArguments(int argc, const char* const* argv);
        [[nodiscard]] CompilationRequest  prepareCompilation() const;
        [[nodiscard]] int                 runCompilationPipeline(const CompilationRequest& request) const;

    private:
        std::vector<std::string>  m_rawArguments;
        std::vector<std::wstring> m_wideArguments;
    };
}
