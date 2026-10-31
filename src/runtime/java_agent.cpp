#include <errno.h>
#include <jni.h>
#include <jvmti.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <memory>
#include <utility>

namespace {
const size_t kMaxClassBytes = size_t{2} * 1024 * 1024;
const size_t kMaxClassNameBytes = 1024;
const long kClassHeaderBytes = 10;
const size_t kMarkerPathReserve = 40;
const char kProbeOption[] = "probe:";
const char kReplaceOption[] = "replace:";
const char kProbeClass[] = "org/katadelos/epub/runtime/Probe";
// The agent is configured before enabling callbacks; these buffers remain
// unchanged until the VM unloads it.
char agentPath[PATH_MAX];
char target[kMaxClassNameBytes + 1];
using ClassBytes = std::unique_ptr<unsigned char, decltype(&free)>;
struct ClassFile {
    ClassBytes bytes{nullptr, &free};
    size_t length{0};
};
ClassFile originalClass;
ClassFile replacementClass;
bool probing;

bool joined(char* destination, size_t capacity, const char* directory, const char* name) {
    const int length = snprintf(destination, capacity, "%s/%s", directory, name);
    return length > 0 && static_cast<size_t>(length) < capacity;
}

// Keep the filesystem path and file contents as the C strings used by stdio.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool writeMarker(const char* path, const char* text) {
    char temporary[PATH_MAX];
    const int length =
        snprintf(temporary, sizeof(temporary), "%s.%ld.pending", path, static_cast<long>(getpid()));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(temporary)) {
        return false;
    }
    FILE* file = fopen(temporary, "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t textLength = strlen(text);
    bool success = fwrite(text, 1, textLength, file) == textLength;
    if (fclose(file) != 0) {
        success = false;
    }
    if (success && rename(temporary, path) == 0) {
        return true;
    }
    unlink(temporary);
    return false;
}

// The marker filename and its contents are distinct strings at each call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void writeState(const char* name, const char* value) {
    char path[PATH_MAX];
    if (joined(path, sizeof(path), agentPath, name)) {
        writeMarker(path, value);
    }
}

bool readClass(const char* name, ClassFile& result) {
    char path[PATH_MAX];
    if (!joined(path, sizeof(path), agentPath, name)) {
        return false;
    }
    const std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path, "rb"), &fclose);
    if (!file || fseek(file.get(), 0, SEEK_END) != 0) {
        return false;
    }
    const long size = ftell(file.get());
    if (size < kClassHeaderBytes || static_cast<unsigned long>(size) > kMaxClassBytes ||
        fseek(file.get(), 0, SEEK_SET) != 0) {
        return false;
    }
    const size_t length = static_cast<size_t>(size);
    ClassBytes bytes(static_cast<unsigned char*>(malloc(length)), &free);
    if (!bytes || fread(bytes.get(), 1, length, file.get()) != length ||
        memcmp(bytes.get(), "\xca\xfe\xba\xbe", 4) != 0) {
        return false;
    }
    result.bytes = std::move(bytes);
    result.length = length;
    return true;
}

bool readTarget() {
    char path[PATH_MAX];
    if (!joined(path, sizeof(path), agentPath, "class-name")) {
        return false;
    }
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    const size_t length = fread(target, 1, sizeof(target), file);
    const bool complete = feof(file) != 0;
    fclose(file);
    if (!complete || length < 2 || length >= sizeof(target) || target[length - 1] != '\n') {
        return false;
    }
    target[length - 1] = 0;
    for (size_t i = 0; i + 1 < length; ++i) {
        const char character = target[i];
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '_' ||
                           character == '$' || character == '/';
        if (!valid) {
            return false;
        }
    }
    return target[0] != '/' && strstr(target, "//") == nullptr;
}

void JNICALL classLoad(jvmtiEnv* jvmti,
                       JNIEnv* /*environment*/,
                       jclass redefined,
                       jobject /*loader*/,
                       const char* name,
                       jobject /*protectionDomain*/,
                       jint length,
                       const unsigned char* data,
                       jint* newLength,
                       unsigned char** newData) {
    if (name == nullptr || redefined != nullptr) {
        return;
    }
    if (probing) {
        if (strcmp(name, kProbeClass) == 0) {
            writeMarker(agentPath, "class-file-load-hook\n");
        }
        return;
    }
    if (strcmp(name, target) != 0) {
        return;
    }
    if (length < 0 || static_cast<size_t>(length) != originalClass.length ||
        memcmp(data, originalClass.bytes.get(), originalClass.length) != 0) {
        writeState("agent-rejected", "original-class-bytes-mismatch\n");
        syslog(LOG_ERR, "kindle-epub: Java class bytes differ from discovered original: %s", name);
        return;
    }
    unsigned char* transformed = nullptr;
    if (jvmti->Allocate(static_cast<jlong>(replacementClass.length), &transformed) !=
        JVMTI_ERROR_NONE) {
        writeState("agent-rejected", "allocation-failed\n");
        return;
    }
    memcpy(transformed, replacementClass.bytes.get(), replacementClass.length);
    *newLength = static_cast<jint>(replacementClass.length);
    *newData = transformed;
    writeState("agent-applied", "original-class-bytes-matched\n");
    syslog(LOG_INFO, "kindle-epub: adapted discovered Java class %s", name);
}
} // namespace

extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* virtualMachine,
                                               char* options,
                                               void* /*reserved*/) {
    const char* path = nullptr;
    if (options != nullptr && strncmp(options, kProbeOption, sizeof(kProbeOption) - 1) == 0) {
        probing = true;
        path = options + sizeof(kProbeOption) - 1;
    } else if (options != nullptr &&
               strncmp(options, kReplaceOption, sizeof(kReplaceOption) - 1) == 0) {
        path = options + sizeof(kReplaceOption) - 1;
    }
    if (path == nullptr || path[0] != '/' || strlen(path) >= PATH_MAX - kMarkerPathReserve ||
        strchr(path, '\n') != nullptr || strchr(path, '\r') != nullptr) {
        return JNI_ERR;
    }
    strcpy(agentPath, path);
    if (probing) {
        // The caller also removes its unique marker before invoking the probe.
        unlink(agentPath);
    } else {
        const char* markers[] = {"agent-ready", "agent-applied", "agent-rejected"};
        for (auto& marker : markers) {
            char stale[PATH_MAX];
            if (joined(stale, sizeof(stale), agentPath, marker)) {
                unlink(stale);
            }
        }
        if (!readTarget() || !readClass("original.class", originalClass) ||
            !readClass("replacement.class", replacementClass)) {
            return JNI_ERR;
        }
    }
    jvmtiEnv* jvmti = nullptr;
    if (virtualMachine->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_0) != JNI_OK) {
        return JNI_ERR;
    }
    jvmtiCapabilities capabilities = {};
    capabilities.can_generate_all_class_hook_events = 1;
    if (jvmti->AddCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
        return JNI_ERR;
    }
    jvmtiEventCallbacks callbacks = {};
    callbacks.ClassFileLoadHook = classLoad;
    if (jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JVMTI_ERROR_NONE ||
        jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr) !=
            JVMTI_ERROR_NONE) {
        return JNI_ERR;
    }
    if (!probing) {
        writeState("agent-ready", "class-file-load-hook-enabled\n");
    }
    return JNI_OK;
}

extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM* /*virtualMachine*/) {
    originalClass.bytes.reset();
    replacementClass.bytes.reset();
}
