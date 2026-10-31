#ifndef KINDLE_EPUB_MOBI8_MODEL_HPP
#define KINDLE_EPUB_MOBI8_MODEL_HPP

#include "mobi8/abi.hpp"

namespace kindle_epub {

Mobi8SDK::Result createBookManager(Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager>& output,
                                   const char* path);

} // namespace kindle_epub

#endif
