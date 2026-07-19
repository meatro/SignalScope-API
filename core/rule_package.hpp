#pragma once

#include <cstddef>

namespace bored::signalscope {

class MutationEngine;

class RulePackageLoader {
public:
    static bool loadCsv(char* text, size_t length, MutationEngine& engine, size_t* loaded_count = nullptr);
};

}  // namespace bored::signalscope
