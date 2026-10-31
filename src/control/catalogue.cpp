#include "activation.hpp"
#include "system.hpp"

#include <array>
#include <cjson/cJSON.h>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>

namespace epubctl {
namespace {

struct OwnedRows {
    const char* table;
    const char* predicate;
};

const std::array<OwnedRows, 5> kOwnedRows{{
    {"associations",
     "handlerId='org.kindle.epub.extractor' OR contentId IN "
     "('MT:application/epub+zip','GL:*.epub','GL:*.EPUB')"},
    {"properties", "handlerId='org.kindle.epub.extractor'"},
    {"handlerIds", "handlerId='org.kindle.epub.extractor'"},
    {"mimetypes", "ext IN ('epub','EPUB') OR mimetype='MT:application/epub+zip'"},
    {"extenstions", "ext IN ('epub','EPUB') OR mimetype='MT:application/epub+zip'"},
}};

std::string appregSql(const std::string& sql) {
    return checkedCommand({"/var/run/eink-sdk/usr/bin/sqlite3", "/var/local/appreg.db"},
                          ".bail on\n.timeout 10000\n" + sql + '\n');
}

std::string snapshotCatalogue() {
    std::string sql;
    for (const auto& rows : kOwnedRows) {
        sql += ".mode insert " + std::string(rows.table) + "\nSELECT * FROM " + rows.table +
               " WHERE " + rows.predicate + " ORDER BY rowid;\n";
    }
    return appregSql(sql);
}

std::string snapshotPath(const std::string& name) {
    return std::string(kStateDirectory) + "/activation/appreg-" + name + ".sql";
}

} // namespace

void selectCatalogue(RuntimeSelection& selection) {
    if (selection.backend == CatalogueBackend::Java) {
        return;
    }
    selection.applicationHandler = trim(
        appregSql("SELECT handlerId FROM associations WHERE interface='application' AND "
                  "contentId='MT:application/x-mobi8-ebook' AND (defaultAssoc='true' OR NOT EXISTS "
                  "(SELECT 1 FROM associations WHERE interface='application' AND "
                  "contentId='MT:application/x-mobi8-ebook' AND defaultAssoc='true'));"));
    if (selection.applicationHandler.empty() ||
        selection.applicationHandler.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
            std::string::npos) {
        throw std::runtime_error("No unique stock MOBI8 application handler is registered.");
    }
    const auto javaProperties = trim(appregSql(
        "SELECT count(*) FROM properties WHERE handlerId='" + selection.applicationHandler +
        "' AND name IN ('jar','class','mainClass') AND value != '';"));
    if (javaProperties != "0") {
        if (selection.javaMode == JavaMode::None) {
            throw std::runtime_error(
                "JAVA_FREE firmware still routes MOBI8 books to Java; native reader "
                "registration is required.");
        }
        return;
    }
    const auto service = trim(appregSql("SELECT value FROM properties WHERE handlerId='" +
                                        selection.applicationHandler + "' AND name='lipcId';"));
    if (service != "com.lab126.KPPMainApp" || selection.kppLibrary.empty()) {
        throw std::runtime_error(
            "The stock application association has no supported Java or native KPP "
            "reader route.");
    }
}

void registerCatalogue(const RuntimeSelection& selection) {
    if (selection.backend == CatalogueBackend::Java) {
        return;
    }
    ensureDirectory(std::string(kStateDirectory) + "/activation");
    writeText(snapshotPath("before"), snapshotCatalogue());
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> registration(cJSON_Parse(R"([
        {"def":"mimetype","assoc":{"mimetype":"MT:application/epub+zip","extensions":["epub","EPUB"]}},
        {"def":"handlerId","assoc":{"handlerId":"org.kindle.epub.extractor","props":{"lib":"","entry":""}}},
        {"def":"association","assoc":{"interface":"extractor","handlerId":"org.kindle.epub.extractor","contentIds":["GL:*.epub","GL:*.EPUB"],"default":"true"}},
        {"def":"association","assoc":{"interface":"application","handlerId":"","contentIds":["MT:application/epub+zip"],"default":"true"}}
    ])"),
                                                                       &cJSON_Delete);
    if (registration == nullptr) {
        throw std::bad_alloc();
    }
    auto* handler =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(registration.get(), 1), "assoc");
    auto* properties = cJSON_GetObjectItemCaseSensitive(handler, "props");
    auto* application =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(registration.get(), 3), "assoc");
    const bool plugin = selection.backend == CatalogueBackend::Plugin;
    const auto library = plugin ? selection.nativeDir + "/libkindle-epub-extractor.so"
                                : "/usr/lib/ccat/libEBridge.so";
    if (cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(properties, "lib"),
                             library.c_str()) == nullptr ||
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(properties, "entry"),
                             plugin ? "load_kindle_epub_extractor" : "load_bridge") == nullptr ||
        cJSON_AddStringToObject(properties,
                                plugin ? "libUnload" : "lipcId",
                                plugin ? "false" : "org.kindle.epub.scanner") == nullptr ||
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(application, "handlerId"),
                             selection.applicationHandler.c_str()) == nullptr) {
        throw std::bad_alloc();
    }
    std::string contents;
    for (const auto* entry = registration->child; entry != nullptr; entry = entry->next) {
        const std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(entry),
                                                                &cJSON_free);
        if (text == nullptr) {
            throw std::bad_alloc();
        }
        if (!contents.empty()) {
            contents += ",\n";
        }
        contents += text.get();
    }
    const auto installPath = std::string(kRunDirectory) + "/epub.install";
    writeText(installPath, contents + '\n');
    checkedCommand({"register", installPath});
    writeText(snapshotPath("installed"), snapshotCatalogue());
}

void restoreCatalogue() {
    if (!isFile(snapshotPath("before"))) {
        return;
    }
    std::string current;
    try {
        current = snapshotCatalogue();
        writeText(snapshotPath("current"), current);
    } catch (const std::exception&) {
        throw std::runtime_error("AppReg could not be read; recovery state retained.");
    }
    if (isFile(snapshotPath("installed")) && current != readText(snapshotPath("installed"))) {
        std::cerr << "AppReg EPUB associations changed externally; leaving those rows untouched.\n";
        throw std::runtime_error(
            "AppReg ownership conflict needs resolution before another activation.");
    }
    std::string sql = "BEGIN IMMEDIATE;\n";
    for (const auto& rows : kOwnedRows) {
        sql += "DELETE FROM " + std::string(rows.table) + " WHERE " + rows.predicate + ";\n";
    }
    sql += readText(snapshotPath("before")) + "COMMIT;\n";
    try {
        appregSql(sql);
    } catch (const std::exception&) {
        throw std::runtime_error("AppReg restoration failed; recovery state retained.");
    }
    for (const auto* name : {"before", "installed", "current"}) {
        removeFile(snapshotPath(name));
    }
}

} // namespace epubctl
