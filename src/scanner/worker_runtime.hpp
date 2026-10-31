#ifndef KINDLE_EPUB_CATALOG_RUNTIME_HPP
#define KINDLE_EPUB_CATALOG_RUNTIME_HPP
#include <string>
namespace epub_catalog {
class ImportRequest;
class DeleteRequest;
std::string importFile(const ImportRequest& request);
void deleteFile(const DeleteRequest& request);
void checkRuntime();
int serve(bool daemonize);
} // namespace epub_catalog
#endif
