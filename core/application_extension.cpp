#include "application_extension.hpp"

#include <cstring>

namespace bored::signalscope {

namespace {

bool validApplicationId(const char* id) {
    if (id == nullptr) return false;
    const size_t length = std::strlen(id);
    if (length == 0U || length > 31U) return false;
    for (size_t index = 0U; index < length; ++index) {
        const char c = id[index];
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!valid) return false;
    }
    return true;
}

}  // namespace

bool ApplicationExtensionRegistry::registerExtension(const ApplicationExtension& extension) {
    if (registered_ || !validApplicationId(extension.id) || extension.writeStatusJson == nullptr ||
        (extension.mutation_direction_mask & ~kMutationDirectionBoth) != 0U) {
        return false;
    }
    extension_ = extension;
    registered_ = true;
    return true;
}

const ApplicationExtension* ApplicationExtensionRegistry::extension() const {
    return registered_ ? &extension_ : nullptr;
}

void __attribute__((weak)) registerSignalScopeApplication(ApplicationExtensionRegistry& /*registry*/) {}

}  // namespace bored::signalscope
