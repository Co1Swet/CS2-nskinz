#pragma once

#include "Core/AppState.hpp"

#include <filesystem>
#include <optional>

namespace nskinzcs2
{
// Cross-process snapshot used by the isolated UI host.  The injected runtime
// resolves the catalog once from CS2's VPK and publishes it beside the DLL;
// the UI process never needs to inspect or call into cs2.exe.
class CatalogStore
{
public:
    static bool Save(const std::filesystem::path& path, const model::Catalog& catalog);
    static std::optional<model::Catalog> Load(const std::filesystem::path& path);
};
}
