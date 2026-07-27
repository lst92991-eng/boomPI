#include "rkaudio_preprocess.h"

namespace {

using InitFunction =
    void* (*)(int, int, int, int, RKAUDIOParam*);
using ProcessFunction =
    int (*)(void*, short*, short*, int, int*);
using DestroyFunction = void (*)(void*);

InitFunction volatile kInitSymbol = &rkaudio_preprocess_init;
ProcessFunction volatile kProcessSymbol = &rkaudio_preprocess_short;
DestroyFunction volatile kDestroySymbol = &rkaudio_preprocess_destory;

}  // namespace

int main() noexcept {
  return kInitSymbol != nullptr && kProcessSymbol != nullptr &&
                 kDestroySymbol != nullptr
             ? 0
             : 1;
}
