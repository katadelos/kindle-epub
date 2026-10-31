#include <dlfcn.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {

using Execve = int (*)(const char*, char* const[], char* const[]);

constexpr std::size_t kMaxArguments = 4096;
constexpr std::size_t kMaxEnvironmentEntries = 65536;
// At most one agent, one boot classpath, and a "-cp" / value pair.
constexpr std::size_t kMaxAddedArguments = 4;
const char kAgentPrefix[] = "-agentpath:";
const char kBootClasspathPrefix[] = "-Xbootclasspath/a:";
const char kPreloadPrefix[] = "LD_PRELOAD=";

// This preload is built without exceptions. Own generated strings explicitly;
// malloc failures must become execve errors instead of terminating the launcher.
class ExecStringList {
public:
    explicit ExecStringList(std::size_t capacity)
        : values_(static_cast<char**>(std::calloc(capacity + 1, sizeof(char*)))),
          owned_(static_cast<bool*>(std::malloc(capacity * sizeof(bool)))), capacity_(capacity) {}
    ExecStringList(const ExecStringList&) = delete;
    ExecStringList& operator=(const ExecStringList&) = delete;

    ~ExecStringList() {
        const int savedError = errno;
        for (std::size_t index = 0; index < size_; ++index) {
            if (owned_[index]) {
                std::free(values_[index]);
            }
        }
        std::free(reinterpret_cast<void*>(values_));
        std::free(owned_);
        errno = savedError;
    }

    bool appendBorrowed(char* value) {
        if (!hasSpace()) {
            return false;
        }
        values_[size_] = value;
        owned_[size_] = false;
        ++size_;
        values_[size_] = nullptr;
        return true;
    }

    char* appendJoined(std::initializer_list<const char*> parts) {
        std::size_t bytes = 1;
        for (const char* part : parts) {
            const std::size_t length = std::strlen(part);
            if (length > std::numeric_limits<std::size_t>::max() - bytes) {
                errno = E2BIG;
                return nullptr;
            }
            bytes += length;
        }
        if (!hasSpace()) {
            return nullptr;
        }
        char* joined = static_cast<char*>(std::malloc(bytes));
        if (joined == nullptr) {
            errno = ENOMEM;
            return nullptr;
        }
        char* cursor = joined;
        for (const char* part : parts) {
            const std::size_t length = std::strlen(part);
            std::memcpy(cursor, part, length);
            cursor += length;
        }
        *cursor = '\0';
        values_[size_] = joined;
        owned_[size_] = true;
        ++size_;
        values_[size_] = nullptr;
        return joined;
    }

    char* const* data() const {
        return values_;
    }

private:
    bool hasSpace() const {
        if (values_ == nullptr || owned_ == nullptr) {
            errno = ENOMEM;
            return false;
        }
        if (size_ == capacity_) {
            errno = E2BIG;
            return false;
        }
        return true;
    }

    char** values_;
    bool* owned_;
    std::size_t size_ = 0;
    const std::size_t capacity_;
};

bool countStrings(char* const strings[], std::size_t limit, std::size_t& count) {
    count = 0;
    if (strings == nullptr) {
        return true;
    }
    while (count <= limit) {
        if (strings[count] == nullptr) {
            return true;
        }
        ++count;
    }
    errno = E2BIG;
    return false;
}

const char* environmentValue(char* const environment[], std::size_t count, const char* key) {
    const std::size_t length = std::strlen(key);
    for (std::size_t index = 0; index < count; ++index) {
        if (std::strncmp(environment[index], key, length) == 0 &&
            environment[index][length] == '=') {
            return environment[index] + length + 1;
        }
    }
    return "";
}

struct InjectionOptions {
    const char* preload;
    const char* classpath;
    const char* bootClasspath;
    const char* agent;

    bool empty() const {
        return *preload == '\0' && *classpath == '\0' && *bootClasspath == '\0' && *agent == '\0';
    }
};

bool startsWith(const char* value, const char* prefix) {
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool isClasspathOption(const char* argument) {
    return std::strcmp(argument, "-cp") == 0 || std::strcmp(argument, "-classpath") == 0;
}

bool appendMissingOptions(ExecStringList& output,
                          char* const arguments[],
                          std::size_t count,
                          const InjectionOptions& options,
                          const char* environmentClasspath) {
    bool hasAgent = false;
    bool hasBootClasspath = false;
    bool hasClasspath = false;
    for (std::size_t index = 1; index < count; ++index) {
        const char* argument = arguments[index];
        if (startsWith(argument, kAgentPrefix) &&
            std::strcmp(argument + sizeof(kAgentPrefix) - 1, options.agent) == 0) {
            hasAgent = true;
        }
        hasBootClasspath = hasBootClasspath || startsWith(argument, kBootClasspathPrefix);
        if (isClasspathOption(argument)) {
            hasClasspath = true;
        }
    }

    if (*options.agent != '\0' && !hasAgent &&
        output.appendJoined({kAgentPrefix, options.agent}) == nullptr) {
        return false;
    }
    if (*options.bootClasspath != '\0' && !hasBootClasspath &&
        output.appendJoined({kBootClasspathPrefix, options.bootClasspath}) == nullptr) {
        return false;
    }
    if (*options.classpath != '\0' && !hasClasspath) {
        const char* previousClasspath = *environmentClasspath != '\0' ? environmentClasspath : ".";
        if (!output.appendBorrowed(const_cast<char*>("-cp")) ||
            output.appendJoined({options.classpath, ":", previousClasspath}) == nullptr) {
            return false;
        }
    }

    return true;
}

bool prepareArguments(ExecStringList& output,
                      char* const arguments[],
                      std::size_t count,
                      const InjectionOptions& options,
                      const char* environmentClasspath) {
    if (!output.appendBorrowed(arguments[0]) ||
        !appendMissingOptions(output, arguments, count, options, environmentClasspath)) {
        return false;
    }
    for (std::size_t index = 1; index < count; ++index) {
        char* argument = arguments[index];
        if (*options.bootClasspath != '\0' && startsWith(argument, kBootClasspathPrefix)) {
            if (output.appendJoined({argument, ":", options.bootClasspath}) == nullptr) {
                return false;
            }
            continue;
        }
        if (!output.appendBorrowed(argument)) {
            return false;
        }
        if (*options.classpath == '\0' || !isClasspathOption(argument)) {
            continue;
        }
        ++index;
        if (index == count) {
            errno = EINVAL;
            return false;
        }
        if (output.appendJoined({options.classpath, ":", arguments[index]}) == nullptr) {
            return false;
        }
    }
    return true;
}

bool isPreloadSeparator(char character) {
    return character == ':' || character == ' ' || character == '\t';
}

// LD_PRELOAD accepts spaces and colons. Compact tokens in place, retaining
// their order and first occurrence when an already-injected VM starts again.
void deduplicatePreloads(char* list) {
    char* input = list;
    char* output = list;
    while (*input != '\0') {
        while (isPreloadSeparator(*input)) {
            ++input;
        }
        char* token = input;
        while (*input != '\0' && !isPreloadSeparator(*input)) {
            ++input;
        }
        const auto length = static_cast<std::size_t>(input - token);
        if (length == 0) {
            break;
        }
        bool duplicate = false;
        for (char* previous = list; previous < output;) {
            char* end = previous;
            while (end < output && *end != ':') {
                ++end;
            }
            if (static_cast<std::size_t>(end - previous) == length &&
                std::memcmp(previous, token, length) == 0) {
                duplicate = true;
                break;
            }
            previous = end + 1;
        }
        // Consume the separator before an overlapping in-place write.
        if (*input != '\0') {
            ++input;
        }
        if (!duplicate) {
            if (output != list) {
                *output++ = ':';
            }
            std::memmove(output, token, length);
            output += length;
        }
    }
    *output = '\0';
}

bool prepareEnvironment(ExecStringList& output,
                        char* const environment[],
                        std::size_t count,
                        const char* preload) {
    for (std::size_t index = 0; index < count; ++index) {
        if (*preload == '\0' || !startsWith(environment[index], kPreloadPrefix)) {
            if (!output.appendBorrowed(environment[index])) {
                return false;
            }
        }
    }
    if (*preload != '\0') {
        const char* previous = environmentValue(environment, count, "LD_PRELOAD");
        char* merged = output.appendJoined({kPreloadPrefix, preload, ":", previous});
        if (merged == nullptr) {
            return false;
        }
        deduplicatePreloads(merged + sizeof(kPreloadPrefix) - 1);
    }
    return true;
}

} // namespace

// Stock startup replaces LD_PRELOAD. Intercept its final VM exec and preserve
// stock VM flags, classpath, bootclasspath, agents and native preloads.
extern "C" __attribute__((visibility("default"))) int
execve(const char* path, char* const arguments[], char* const environment[]) {
    static const auto original = reinterpret_cast<Execve>(dlsym(RTLD_NEXT, "execve"));
    if (original == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    if (path == nullptr) {
        errno = EFAULT;
        return -1;
    }
    if (std::strcmp(path, "/usr/java/bin/cvm") != 0 &&
        std::strcmp(path, "/usr/java/bin/java") != 0) {
        return original(path, arguments, environment);
    }

    std::size_t environmentCount = 0;
    if (!countStrings(environment, kMaxEnvironmentEntries, environmentCount)) {
        return -1;
    }
    const InjectionOptions options = {
        environmentValue(environment, environmentCount, "KINDLE_EPUB_FRAMEWORK_PRELOAD"),
        environmentValue(environment, environmentCount, "KINDLE_EPUB_JAVA_CLASSPATH"),
        environmentValue(environment, environmentCount, "KINDLE_EPUB_JAVA_BOOTCLASSPATH"),
        environmentValue(environment, environmentCount, "KINDLE_EPUB_JAVA_AGENT")};
    if (options.empty()) {
        return original(path, arguments, environment);
    }
    std::size_t argumentCount = 0;
    if (!countStrings(arguments, kMaxArguments, argumentCount)) {
        return -1;
    }
    if (argumentCount == 0) {
        errno = EINVAL;
        return -1;
    }

    // Counts were bounded above, so these capacities cannot overflow. Each
    // append still checks capacity before touching either array.
    ExecStringList preparedArguments(argumentCount + kMaxAddedArguments);
    ExecStringList preparedEnvironment(environmentCount + 1);
    if (!prepareArguments(preparedArguments,
                          arguments,
                          argumentCount,
                          options,
                          environmentValue(environment, environmentCount, "CLASSPATH")) ||
        !prepareEnvironment(preparedEnvironment, environment, environmentCount, options.preload)) {
        return -1;
    }
    return original(path, preparedArguments.data(), preparedEnvironment.data());
}
