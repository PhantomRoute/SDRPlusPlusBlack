#pragma once
#include <filesystem>
#include <string>
#include <system_error>

namespace utils {

    // Path for a scratch file in the platform's temp directory: /tmp (or
    // $TMPDIR) on unix, %TEMP% on Windows. Debug captures used to hardcode
    // /tmp, which silently did nothing on Windows because the open failed.
    // Falls back to a bare relative name if the temp directory can't be
    // determined, since these are diagnostics and must never throw.
    inline std::string tempFilePath(const std::string& name) {
        std::error_code ec;
        auto dir = std::filesystem::temp_directory_path(ec);
        if (ec) { return name; }
        return (dir / name).string();
    }

}
