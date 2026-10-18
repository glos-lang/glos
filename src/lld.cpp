#include <lld/Common/Driver.h>

extern "C" {
#include "basic.h"
#include "lld.h"
}

LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)
LLD_HAS_DRIVER(coff)

int run_lld(const char **cmdline, size_t cmdline_count) {
    llvm::ArrayRef args(cmdline, cmdline_count);

#ifdef PLATFORM_X86_64_LINUX
    return lld::lldMain(args, llvm::outs(), llvm::errs(), {{lld::Gnu, &lld::elf::link}}).retCode;
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    return lld::lldMain(args, llvm::outs(), llvm::errs(), {{lld::Darwin, &lld::macho::link}}).retCode;
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
    return lld::lldMain(args, llvm::outs(), llvm::errs(), {{lld::WinLink, &lld::coff::link}}).retCode;
#endif // PLATFORM_X86_64_WINDOWS
}
