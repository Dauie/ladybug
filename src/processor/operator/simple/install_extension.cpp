#include "processor/operator/simple/install_extension.h"

#include "common/external_access.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include <format>

#if defined(LBUG_NO_EXTERNAL_ACCESS)
namespace lbug::extension {
// extension_installer.cpp (cpp-httplib and OpenSSL) is not built without external access, but
// MSVC still emits ExtensionInstaller's vtable wherever its header is used.
bool ExtensionInstaller::install() {
    common::throwIfExternalAccessDisabled("INSTALL EXTENSION");
    return false;
}
} // namespace lbug::extension
#endif

namespace lbug {
namespace processor {

using namespace lbug::common;
using namespace lbug::extension;

void InstallExtension::setOutputMessage(bool installed, storage::MemoryManager* memoryManager) {
    if (info.forceInstall) {
        appendMessage(std::format("Extension: {} updated from the repo: {}.", info.name, info.repo),
            memoryManager);
        return;
    }
    if (installed) {
        appendMessage(
            std::format("Extension: {} installed from the repo: {}.", info.name, info.repo),
            memoryManager);
    } else {
        appendMessage(
            std::format(
                "Extension: {} is already installed.\nTo update it, you can run: UPDATE {}.",
                info.name, info.name),
            memoryManager);
    }
}

void InstallExtension::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
#if defined(LBUG_NO_EXTERNAL_ACCESS)
    // The binder refuses INSTALL in this build, and the installer is not compiled.
    (void)clientContext;
    common::throwIfExternalAccessDisabled("INSTALL EXTENSION");
#else
    ExtensionInstaller installer{info, *clientContext};
    bool installResult = installer.install();
    setOutputMessage(installResult, storage::MemoryManager::Get(*clientContext));
    if (info.forceInstall) {
        DASSERT(installResult);
    }
#endif
}

} // namespace processor
} // namespace lbug
