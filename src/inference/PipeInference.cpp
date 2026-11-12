#include "PipeInference.h"
#include "../dsp/CsoundEngine.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <array>

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/resource.h>
#endif

namespace
{
int readEnvInt(const char* name, int fallback, int minValue, int maxValue)
{
    if (auto* raw = std::getenv(name))
    {
        char* end = nullptr;
        const auto value = std::strtol(raw, &end, 10);
        if (end != raw)
            return juce::jlimit(minValue, maxValue, static_cast<int>(value));
    }

    return fallback;
}

void setEnvValue(const char* name, const juce::String& value)
{
    const auto valueStd = value.toStdString();
   #ifdef _WIN32
    _putenv_s(name, valueStd.c_str());
   #else
    setenv(name, valueStd.c_str(), 1);
   #endif
}

void setEnvIfUnset(const char* name, const juce::String& value)
{
    if (auto* existing = std::getenv(name); existing != nullptr && existing[0] != '\0')
        return;

    setEnvValue(name, value);
}

void capThreadEnv(const char* name, int limit)
{
    if (auto* raw = std::getenv(name))
    {
        char* end = nullptr;
        const auto current = std::strtol(raw, &end, 10);
        if (end != raw && current >= 1 && current <= limit)
            return;
    }

    setEnvValue(name, juce::String(limit));
}

void configureInferenceCpuBudget()
{
    const auto workerThreads = readEnvInt("T5YNTH_INFERENCE_CPU_THREADS", 2, 1, 16);
    const auto interopThreads = readEnvInt("T5YNTH_INFERENCE_INTEROP_THREADS", 1, 1, 16);

    for (auto* key : { "OMP_NUM_THREADS",
                       "MKL_NUM_THREADS",
                       "OPENBLAS_NUM_THREADS",
                       "NUMEXPR_NUM_THREADS",
                       "VECLIB_MAXIMUM_THREADS",
                       "BLIS_NUM_THREADS",
                       "TORCH_NUM_THREADS" })
    {
        capThreadEnv(key, workerThreads);
    }

    capThreadEnv("TORCH_NUM_INTEROP_THREADS", interopThreads);
    setEnvIfUnset("OMP_WAIT_POLICY", "PASSIVE");
    setEnvIfUnset("KMP_BLOCKTIME", "0");
    setEnvIfUnset("MKL_DYNAMIC", "TRUE");
    setEnvIfUnset("PYTHONUNBUFFERED", "1");
}

// Tell the backend which Csound the ENGINE is using, so its acceptance gate can
// compile against the same one.
//
// The gate cannot work this out for itself. Its own search walks up from the
// backend's path to a "Contents" directory (lco_write.py, _bundled_csound_libs),
// which finds nothing in a dev run from backend/pipe_inference.py, and nothing in
// the Linux tarball, where the PyInstaller backend sits in backend/_internal
// beside a libs/ directory no ancestor walk names. The gate would then fall back
// to a system-wide Csound — or none — and judge an orchestra by a vocabulary the
// engine does not have (ticket #25).
//
// The one thing that cannot be wrong is the library this module is bound to, so
// ask the engine (CsoundEngine::loadedLibraryPath: dladdr on its own
// &csoundCreate, the same rule it uses to find the Opcodes64 beside it). Not a
// symbol search across the process: on macOS a DAW's other Csound (Cabbage,
// csound~) could answer first. That serves a macOS bundle, the Linux tarball and
// VST3, and the .deb (where it yields the distro's libcsound64 with no
// Opcodes64 beside it, which is the correct answer) alike. A build without
// Csound, and Windows (which keeps its own csound64.dll walk), yield nothing and
// the gate's own search is unchanged.
void exportBundledCsoundPath()
{
    const auto lib = CsoundEngine::loadedLibraryPath();

    if (! lib.empty())
        setEnvValue ("T5YNTH_CSOUND_LIB", juce::String::fromUTF8 (lib.c_str()));
}

#ifdef _WIN32
DWORD inferencePriorityClass()
{
    auto* rawEnv = std::getenv("T5YNTH_INFERENCE_PRIORITY");
    if (rawEnv == nullptr)
        return BELOW_NORMAL_PRIORITY_CLASS;

    const juce::String raw { rawEnv };
    const auto value = raw.trim().toLowerCase();

    if (value == "normal")
        return 0;
    if (value == "idle")
        return IDLE_PRIORITY_CLASS;

    return BELOW_NORMAL_PRIORITY_CLASS;
}
#else
void lowerCurrentProcessPriority()
{
    const auto niceValue = readEnvInt("T5YNTH_INFERENCE_NICE", 10, 0, 19);
    if (niceValue > 0)
        setpriority(PRIO_PROCESS, 0, niceValue);
}
#endif
}

PipeInference::~PipeInference()
{
    shutdown();
}

juce::File PipeInference::findBundledBinary(const juce::File& backendDir) const
{
    // PyInstaller-bundled binary:
    //   POSIX   backendDir/dist/pipe_inference/pipe_inference
    //   Windows backendDir/dist/pipe_inference/pipe_inference.exe
    auto dist = backendDir.getChildFile("dist/pipe_inference/pipe_inference");
    if (isCompatibleBundledBinary(dist)) return dist;

   #ifdef _WIN32
    auto distExe = backendDir.getChildFile("dist/pipe_inference/pipe_inference.exe");
    if (isCompatibleBundledBinary(distExe)) return distExe;

    // Installed layout: binary next to backendDir
    // Windows: backend/pipe_inference.exe
    auto siblingExe = backendDir.getChildFile("pipe_inference.exe");
    if (isCompatibleBundledBinary(siblingExe)) return siblingExe;
   #else
    // Installed layout: binary next to backendDir
    // macOS app bundle: Contents/Resources/backend/pipe_inference
    // Linux:            backend/pipe_inference
    auto sibling = backendDir.getChildFile("pipe_inference");
    if (isCompatibleBundledBinary(sibling)) return sibling;
   #endif

    return {};  // not found — fall back to Python
}

juce::File PipeInference::findLibraryFile(const juce::File& backendDir) const
{
    // Dev tree: lco_library.json sits directly in backend/, next to
    // pipe_inference.py (LIBRARY_PATH in lco_write.py).
    auto sibling = backendDir.getChildFile("lco_library.json");
    if (sibling.existsAsFile()) return sibling;

    // PyInstaller 6.x onedir (pipe_inference.spec, no contents-directory
    // override — verified against the vendored 6.20.0) keeps the executable
    // at the dist root but nests every `datas` entry, this JSON included,
    // under `_internal/`. Installed-app layout (docs/RELEASE_PROCESS.md
    // embeds the dist output straight into backendDir) and Linux packaging
    // (installer/linux/stage_backend_bundle.sh) both confirm this same
    // `_internal/` nesting, so this is the sibling case's actual shape:
    auto internalSibling = backendDir.getChildFile("_internal/lco_library.json");
    if (internalSibling.existsAsFile()) return internalSibling;

    // Local PyInstaller test build, not yet packaged (backend/dist/pipe_
    // inference/ still holding its own dist layout):
    auto dist = backendDir.getChildFile("dist/pipe_inference/_internal/lco_library.json");
    if (dist.existsAsFile()) return dist;

    return {};
}

juce::StringArray PipeInference::getCuratedInstrumentNames() const
{
    // backendDir_ is written on a background launch thread (MainPanel::
    // tryLoadInferenceModels), while this is called from the message thread
    // (SynthPanel::updateVisibility) — a real cross-thread race on the
    // juce::String backendDir_ holds, not merely a stale read. Snapshot it
    // under backendDirMutex_ (NOT stateMutex_ — see that mutex's doc comment)
    // and do the actual file I/O outside the lock.
    juce::File dir;
    {
        const std::lock_guard<std::mutex> dirLock(backendDirMutex_);
        dir = backendDir_;
    }
    if (! dir.isDirectory())
        return {};

    // findLibraryFile has already proved the file exists (each candidate is
    // probed with existsAsFile) and returns a default File when none does, so
    // re-probing here would only spend two more syscalls on the answer it just
    // gave. A file that vanishes between the probe and the stats below reads
    // as size 0, fails to parse, and is remembered as an empty list — which is
    // the truthful answer for a library that is no longer there.
    auto libFile = findLibraryFile(dir);
    if (libFile == juce::File())
        return {};

    // Same file, same timestamp, same size → the answer from last time. This
    // side of the memo is a handful of stats; the other side parses ~450 kB of
    // JSON, and this runs from SynthPanel::updateVisibility(), which resized()
    // calls — i.e. once per frame of a window drag — while an empty answer is
    // a real one (a library with nothing approved yet).
    //
    // SIZE is in the key, not decoration: the answer is remembered even when
    // the file did not parse, and a file caught mid-rewrite does not parse. On
    // Linux juce::File resolves mtime to the second, so a read landing in a
    // writer's truncation window would otherwise pin the panel to an empty
    // list for the rest of the session while a perfectly valid library sits on
    // disk. A half-written file has a different length from the finished one,
    // so the memo misses and the next call recovers. This covers the
    // truncate-then-write shape, which is what tools/lco_build_library.py used
    // to do and what any plain `open(path,'w')` does; that tool now renames
    // over the target instead, so its window is gone entirely. It does NOT
    // cover an in-place rewrite that keeps the length (`open(path,'r+')`) —
    // nothing in this repo writes that way.
    const auto path = libFile.getFullPathName();
    const auto stamp = libFile.getLastModificationTime();
    const auto size = libFile.getSize();

    if (path == curatedNamesFile_ && stamp == curatedNamesStamp_ && size == curatedNamesSize_)
        return curatedNames_;

    auto remember = [&](juce::StringArray result) -> juce::StringArray
    {
        curatedNamesFile_ = path;
        curatedNamesStamp_ = stamp;
        curatedNamesSize_ = size;
        curatedNames_ = result;
        return result;
    };

    auto parsed = juce::JSON::parse(libFile);
    auto* root = parsed.getDynamicObject();
    if (root == nullptr)
        return remember({});

    auto instruments = root->getProperty("instruments");
    auto* arr = instruments.getArray();
    if (arr == nullptr)
        return remember({});

    // "Curated" is BJ's AUTHORISATION, and it is only ever read here, never
    // re-derived. tools/lco_build_library.py composes the `curated` field from
    // the ONE record of that authorisation — the lexicon entry's `heard`, which
    // carries his own words, the date, and the page he heard it on — and writes
    // it into this file only for the entries he has actually taken.
    //
    // Both of the other ways to get this list have been tried and both were
    // wrong. Deriving it from something measurable — `params` plus
    // `anchor_code`, i.e. "the number reaches the code" — admitted seven
    // entries he never approved. Hand-writing `curated` as a second field
    // beside `heard` then contradicted the verdict in both directions at once:
    // six entries carried a blanket "built under the Authoring rules" text
    // (a claim about how they were BUILT, not a verdict) while five of their
    // own `heard` records said „nur einzelne Dateien gehört … Das ist kein
    // abgenommenes INSTRUMENT" or „NICHT als abgenommen behandeln", and
    // `singing_bowl`, which he had called „verfiziertes
    // instrument", had no such field and so never appeared. BJ, 2026-07-31:
    // „singing bowl fehlt, dafür irgend ein phantasiescheiss 'driven metal'".
    // A measured property is not a verdict, and a verdict with two homes has a
    // wrong answer in one of them.
    //
    // The NAME comes from the library's `name` field and is not derived here.
    // "fm fm3 fm_bell" tells a player nothing, so the key is not it; and
    // cutting the head off `why` produced descriptions rather than names — "a
    // wire under tension", "a metal body held ringing by moving air" — which is
    // an invention wearing the costume of data (BJ, 2026-07-30: "das sind NICHT
    // die Instrumente"). tools/lco_build_library.py writes `name` from the
    // entry's first surface form: curated, and the word that actually reaches
    // that body in a prompt.
    juce::StringArray names;
    for (auto& entry : *arr)
        if (auto* obj = entry.getDynamicObject())
        {
            if (obj->getProperty("curated").toString().isEmpty())
                continue;

            auto name = obj->getProperty("name").toString().trim();
            if (name.isEmpty())
                name = obj->getProperty("key").toString();
            if (name.isNotEmpty())
                names.add(name);
        }

    return remember(names);
}

bool PipeInference::isCompatibleBundledBinary(const juce::File& binary) const
{
    if (!binary.existsAsFile())
        return false;

    auto stream = binary.createInputStream();
    if (stream == nullptr)
    {
        juce::Logger::writeToLog("PipeInference: could not inspect bundled backend " + binary.getFullPathName());
        return false;
    }

    std::array<unsigned char, 4> magic { 0, 0, 0, 0 };
    if (stream->read(magic.data(), static_cast<int>(magic.size())) != static_cast<int>(magic.size()))
        return false;

   #ifdef _WIN32
    const bool compatible = magic[0] == 'M' && magic[1] == 'Z';
   #elif JUCE_MAC
    const bool compatible =
        (magic[0] == 0xfe && magic[1] == 0xed && magic[2] == 0xfa && magic[3] == 0xce) ||
        (magic[0] == 0xce && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) ||
        (magic[0] == 0xfe && magic[1] == 0xed && magic[2] == 0xfa && magic[3] == 0xcf) ||
        (magic[0] == 0xcf && magic[1] == 0xfa && magic[2] == 0xed && magic[3] == 0xfe) ||
        (magic[0] == 0xca && magic[1] == 0xfe && magic[2] == 0xba && magic[3] == 0xbe);
   #else
    const bool compatible = magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
   #endif

    if (!compatible)
    {
        juce::Logger::writeToLog("PipeInference: ignoring incompatible bundled backend "
                                  + binary.getFullPathName());
    }

    return compatible;
}

void PipeInference::prepareBundledBinary(const juce::File& backendDir, const juce::File& binary) const
{
   #ifndef _WIN32
    if (!binary.setExecutePermission(true))
    {
        juce::Logger::writeToLog("PipeInference: could not ensure execute bit on "
                                  + binary.getFullPathName());
    }
   #endif

   #if JUCE_MAC
    if (backendDir.getFullPathName().containsIgnoreCase(".app/Contents/Resources/backend"))
    {
        juce::ChildProcess proc;
        const juce::StringArray args { "xattr", "-dr", "com.apple.quarantine",
                                       backendDir.getFullPathName() };

        if (!proc.start(args, juce::ChildProcess::wantStdErr))
        {
            juce::Logger::writeToLog("PipeInference: could not start xattr cleanup for "
                                      + backendDir.getFullPathName());
            return;
        }

        proc.waitForProcessToFinish(5000);
        const auto output = proc.readAllProcessOutput().trim();

        if (proc.getExitCode() != 0 && output.isNotEmpty())
        {
            juce::Logger::writeToLog("PipeInference: xattr cleanup failed: " + output);
        }
    }
   #endif
}

juce::String PipeInference::maybeAugmentMacStandaloneError(const juce::File& backendDir,
                                                           const juce::String& detail) const
{
   #if JUCE_MAC
    const bool isBundledStandalone
        = backendDir.getFullPathName().containsIgnoreCase(".app/Contents/Resources/backend");

    if (isBundledStandalone
        && (detail.containsIgnoreCase("exec failed")
            || detail.containsIgnoreCase("permission denied")
            || detail.containsIgnoreCase("operation not permitted")
            || detail.containsIgnoreCase("code signature")
            || detail.containsIgnoreCase("library not loaded")))
    {
        return detail
             + "\n\nmacOS appears to be blocking the app bundle or one of its bundled "
               "libraries. The .pkg installer avoids this path and removes quarantine "
               "attributes after install.";
    }
   #endif

    return detail;
}

juce::String PipeInference::findPython(const juce::File& backendDir) const
{
    auto projectRoot = backendDir.getParentDirectory();

   #ifdef _WIN32
    for (auto& rel : { ".venv/Scripts/python.exe", "venv/Scripts/python.exe",
                       ".venv/Scripts/python3.exe", "venv/Scripts/python3.exe" })
   #else
    for (auto& rel : { ".venv/bin/python", "venv/bin/python",
                       ".venv/bin/python3", "venv/bin/python3" })
   #endif
    {
        auto py = projectRoot.getChildFile(rel);
        if (py.existsAsFile())
            return py.getFullPathName();
    }
   #ifndef _WIN32
    for (auto& path : { "/usr/bin/python3", "/usr/local/bin/python3" })
    {
        if (juce::File(path).existsAsFile())
            return path;
    }
   #endif
    return "python3";
}

bool PipeInference::isConnected() const
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
   #ifdef _WIN32
    return hChildStdinWr_ != INVALID_HANDLE_VALUE;
   #else
    return stdinFd_ >= 0;
   #endif
}

bool PipeInference::isChildAlive() const
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
#ifdef _WIN32
    if (hProcess_ == INVALID_HANDLE_VALUE) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(hProcess_, &exitCode)) return false;
    return exitCode == STILL_ACTIVE;
#else
    if (childPid_ <= 0) return false;
    int status = 0;
    pid_t result = waitpid(childPid_, &status, WNOHANG);
    return result == 0;  // 0 = still running
#endif
}

bool PipeInference::tryRestart()
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    juce::Logger::writeToLog("PipeInference: attempting restart...");
    shutdown();
    if (backendDir_.exists())
        return launch(backendDir_);
    return false;
}

juce::String PipeInference::handleStatusTimeout(const char* what)
{
    const bool alive = isChildAlive();
    juce::Logger::writeToLog(juce::String("PipeInference: no status byte while waiting for ")
                             + what
                             + (alive ? juce::String::fromUTF8(" (child alive \xe2\x80\x94 restarting "
                                                               "to resynchronise the pipe)")
                                      : juce::String(" (subprocess died)")));
    const bool restarted = tryRestart();
    if (! alive)
        return restarted ? juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restarted, try again")
                         : juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
    return restarted ? juce::String("Timeout waiting for ") + what
                           + juce::String::fromUTF8(" \xe2\x80\x94 backend restarted")
                     : juce::String("Timeout waiting for ") + what
                           + juce::String::fromUTF8(" \xe2\x80\x94 restart failed");
}

bool PipeInference::launch(const juce::File& backendDir)
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    // Atomic guard: only one launch() may run at a time
    bool expected = false;
    if (!launching_.compare_exchange_strong(expected, true))
    {
        juce::Logger::writeToLog("PipeInference: launch already in progress, skipping");
        return ready_.load();
    }

    // If subprocess is already running, nothing to do
    if (ready_.load() && isChildAlive())
    {
        juce::Logger::writeToLog("PipeInference: already running, skipping launch");
        launching_ = false;
        return true;
    }
    // Kill any orphaned subprocess before starting a new one
    shutdown();

    {
        const std::lock_guard<std::mutex> dirLock(backendDirMutex_);
        backendDir_ = backendDir;
    }

    // Resolve executable: bundled binary (PyInstaller) or Python + script
    auto bundled = findBundledBinary(backendDir);
    juce::String execPath;
    juce::String scriptPath;  // empty when using bundled binary

    if (bundled.existsAsFile())
    {
        prepareBundledBinary(backendDir, bundled);
        execPath = bundled.getFullPathName();
        juce::Logger::writeToLog("PipeInference: launching bundled " + execPath);
    }
    else
    {
        auto script = backendDir.getChildFile("pipe_inference.py");
        if (!script.existsAsFile())
        {
            const auto dist = backendDir.getChildFile("dist/pipe_inference/pipe_inference");
           #ifdef _WIN32
            const auto distExe = backendDir.getChildFile("dist/pipe_inference/pipe_inference.exe");
            const auto siblingExe = backendDir.getChildFile("pipe_inference.exe");
           #else
            const auto sibling = backendDir.getChildFile("pipe_inference");
           #endif

            if (dist.existsAsFile()
               #ifdef _WIN32
                || distExe.existsAsFile()
                || siblingExe.existsAsFile()
               #else
                || sibling.existsAsFile()
               #endif
            )
            {
                lastError_ = "Bundled backend has incompatible binary format for this platform";
            }
            else
            {
                lastError_ = "Backend not found in " + backendDir.getFullPathName();
            }
            juce::Logger::writeToLog("PipeInference: " + lastError_);
            launching_ = false;
            return false;
        }
        execPath = findPython(backendDir);
        scriptPath = script.getFullPathName();
        juce::Logger::writeToLog("PipeInference: launching " + execPath + " " + scriptPath);
    }

    configureInferenceCpuBudget();
    exportBundledCsoundPath();
    auto stderrLog = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("T5ynth/Logs/backend_stderr.log");

#ifdef _WIN32
    // ── Windows: CreatePipe + CreateProcess ──────────────────────────
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hChildStdinRd = INVALID_HANDLE_VALUE;
    HANDLE hChildStdinWr = INVALID_HANDLE_VALUE;
    HANDLE hChildStdoutRd = INVALID_HANDLE_VALUE;
    HANDLE hChildStdoutWr = INVALID_HANDLE_VALUE;
    HANDLE hChildStderrWr = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &sa, 0)
        || !CreatePipe(&hChildStdinRd, &hChildStdinWr, &sa, 0))
    {
        lastError_ = "CreatePipe failed (error " + juce::String((int)GetLastError()) + ")";
        juce::Logger::writeToLog("PipeInference: " + lastError_);
        if (hChildStdinRd != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinRd);
        if (hChildStdinWr != INVALID_HANDLE_VALUE) CloseHandle(hChildStdinWr);
        if (hChildStdoutRd != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutRd);
        if (hChildStdoutWr != INVALID_HANDLE_VALUE) CloseHandle(hChildStdoutWr);
        launching_ = false;
        return false;
    }

    // Parent-side handles must NOT be inherited
    SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0);

    stderrLog.getParentDirectory().createDirectory();
    std::wstring stderrPath(stderrLog.getFullPathName().toWideCharPointer());
    hChildStderrWr = CreateFileW(stderrPath.c_str(),
                                 GENERIC_WRITE,
                                 FILE_SHARE_READ,
                                 &sa,
                                 CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (hChildStderrWr == INVALID_HANDLE_VALUE)
    {
        juce::Logger::writeToLog("PipeInference: could not open backend stderr log "
                                  + stderrLog.getFullPathName()
                                  + " (error " + juce::String((int)GetLastError()) + ")");
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.hStdInput = hChildStdinRd;
    si.hStdOutput = hChildStdoutWr;
    si.hStdError = hChildStderrWr != INVALID_HANDLE_VALUE ? hChildStderrWr
                                                          : GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};

    // Build command line
    juce::String cmdLine;
    if (scriptPath.isEmpty())
        cmdLine = "\"" + execPath + "\"";
    else
        cmdLine = "\"" + execPath + "\" \"" + scriptPath + "\"";

    auto cmdWide = cmdLine.toWideCharPointer();
    std::wstring cmdBuf(cmdWide);

    // Kill-on-close job for the backend. Without it a host that dies without
    // reaching shutdown() (crash, force-quit, debugger stop) leaves the backend
    // running: it only notices a dead parent at stdin EOF, and while a request
    // is in flight it never gets back there — a macOS orphan measured
    // 2026-07-22 held a core and grew for four hours. pipe_inference.py's own
    // watchdog polls getppid(), which on Windows keeps returning the dead
    // parent's id, so this platform needs the guard from the OUTSIDE. Closing
    // the handle kills everything in the job, and process death closes it.
    // CREATE_SUSPENDED so the child cannot run — or spawn — before it is in the
    // job; it is resumed right after the assignment below.
    if (hJob_ != nullptr) { CloseHandle(hJob_); hJob_ = nullptr; }
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
        {
            juce::Logger::writeToLog("PipeInference: job object could not be configured (error "
                                     + juce::String((int)GetLastError())
                                     + juce::String::fromUTF8(") \xe2\x80\x94 the backend may "
                                                              "outlive a crashed host"));
            CloseHandle(hJob);
            hJob = nullptr;
        }
    }

    const DWORD creationFlags = CREATE_NO_WINDOW | CREATE_SUSPENDED | inferencePriorityClass();

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                        creationFlags, nullptr, nullptr, &si, &pi))
    {
        lastError_ = "CreateProcess failed (error " + juce::String((int)GetLastError()) + ")";
        if (hChildStderrWr != INVALID_HANDLE_VALUE)
            lastError_ << "\nLog: " << stderrLog.getFullPathName();
        juce::Logger::writeToLog("PipeInference: " + lastError_);
        CloseHandle(hChildStdinRd);
        CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd);
        CloseHandle(hChildStdoutWr);
        if (hChildStderrWr != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWr);
        if (hJob != nullptr) CloseHandle(hJob);
        launching_ = false;
        return false;
    }

    // Into the job while still suspended, then let it run. A failed assignment
    // is not fatal — the backend simply loses the crash-orphan guard — but it
    // must not leave the process suspended forever.
    if (hJob != nullptr && !AssignProcessToJobObject(hJob, pi.hProcess))
    {
        juce::Logger::writeToLog("PipeInference: backend could not be put in the kill-on-close job (error "
                                 + juce::String((int)GetLastError())
                                 + juce::String::fromUTF8(") \xe2\x80\x94 it may outlive "
                                                          "a crashed host"));
        CloseHandle(hJob);
        hJob = nullptr;
    }
    ResumeThread(pi.hThread);

    // Close child-side handles (now owned by child process)
    CloseHandle(hChildStdinRd);
    CloseHandle(hChildStdoutWr);
    if (hChildStderrWr != INVALID_HANDLE_VALUE) CloseHandle(hChildStderrWr);
    CloseHandle(pi.hThread);

    hChildStdinWr_ = hChildStdinWr;
    hChildStdoutRd_ = hChildStdoutRd;
    hProcess_ = pi.hProcess;
    hJob_ = hJob;

#else
    // ── POSIX: pipe + fork + exec ───────────────────────────────────
    int pipeIn[2];   // parent writes to pipeIn[1], child reads from pipeIn[0]
    int pipeOut[2];  // child writes to pipeOut[1], parent reads from pipeOut[0]

    if (pipe(pipeIn) < 0 || pipe(pipeOut) < 0)
    {
        juce::Logger::writeToLog("PipeInference: pipe() failed");
        return false;
    }

    childPid_ = fork();
    if (childPid_ < 0)
    {
        juce::Logger::writeToLog("PipeInference: fork() failed");
        return false;
    }

    if (childPid_ == 0)
    {
        // Child process
        close(pipeIn[1]);
        close(pipeOut[0]);
        dup2(pipeIn[0], STDIN_FILENO);
        dup2(pipeOut[1], STDOUT_FILENO);

        // Redirect stderr to log file for post-mortem diagnosis
        auto logDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("T5ynth/Logs");
        logDir.createDirectory();
        auto logPath = logDir.getChildFile("backend_stderr.log").getFullPathName().toStdString();
        int logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logFd >= 0) { dup2(logFd, STDERR_FILENO); close(logFd); }

        lowerCurrentProcessPriority();

        close(pipeIn[0]);
        close(pipeOut[1]);

        auto execStr = execPath.toStdString();
        if (scriptPath.isEmpty())
            execlp(execStr.c_str(), execStr.c_str(), nullptr);
        else
        {
            auto scriptStr = scriptPath.toStdString();
            execlp(execStr.c_str(), execStr.c_str(), scriptStr.c_str(), nullptr);
        }

        auto message = std::string("PipeInference: exec failed for ")
                     + execStr + ": " + std::strerror(errno) + "\n";
        ::write(STDERR_FILENO, message.c_str(), message.size());
        _exit(1);
    }

    // Parent process
    close(pipeIn[0]);
    close(pipeOut[1]);
    stdinFd_ = pipeIn[1];
    stdoutFd_ = pipeOut[0];
#endif

    // Wait for ready signal (\x02 + uint16 len + JSON) with 120s timeout
    // (loading two pipelines can take 30-60s)
    char readyByte = 0;
    if (!readExact(&readyByte, 1, 120000))
    {
        // Read stderr log for diagnosis
        juce::String stderrContent;
        if (stderrLog.existsAsFile())
            stderrContent = stderrLog.loadFileAsString().trimEnd();

        if (stderrContent.isNotEmpty())
        {
            lastError_ = maybeAugmentMacStandaloneError(
                backendDir,
                stderrContent.fromLastOccurrenceOf("\n", false, false).trim());
            lastError_ << "\nLog: " << stderrLog.getFullPathName();
        }
        else if (!isChildAlive())
            lastError_ = "Backend process crashed on startup";
        else
            lastError_ = "Timeout waiting for backend (120s)";

        if (!lastError_.contains("Log: "))
            lastError_ << "\nLog: " << stderrLog.getFullPathName();

        juce::Logger::writeToLog("PipeInference: " + lastError_);
        shutdown();
        launching_ = false;
        return false;
    }

    if (readyByte == '\x00')
    {
        // Backend sent an error message: \x00 + uint32 length + UTF-8 message
        uint32_t errLen = 0;
        if (readExact(&errLen, 4, 5000) && errLen > 0 && errLen < 100000)
        {
            std::vector<char> errBuf(errLen + 1, 0);
            if (readExact(errBuf.data(), errLen, 5000))
                lastError_ = juce::String::fromUTF8(errBuf.data(), static_cast<int>(errLen));
            else
                lastError_ = "Backend error (could not read message)";
        }
        else
            lastError_ = "Backend error (no message)";

        juce::Logger::writeToLog("PipeInference: " + lastError_);
        shutdown();
        launching_ = false;
        return false;
    }

    if (readyByte != '\x02')
    {
        lastError_ = "Backend protocol error (unexpected byte: " + juce::String((int)readyByte) + ")";
        juce::Logger::writeToLog("PipeInference: " + lastError_);
        shutdown();
        launching_ = false;
        return false;
    }

    // Reset all per-launch state ahead of parsing so a restart that succeeds
    // in starting Python but fails to deliver a valid info JSON can't carry
    // stale entries from the previous launch into the new one. The atomic
    // ready_ flag flips true only on full success, but the public getters
    // still observe these members regardless — clearing here keeps them
    // honest.
    availableDevices_.clear();
    availableModels_.clear();
    defaultDevice_.clear();
    defaultModel_.clear();
    modelMetadata_.clear();

    // Read device info JSON (uint16 length + JSON bytes). Every step here is a
    // hard requirement for ready_ — a backend that sends \x02 but stalls or
    // sends a malformed payload must NOT flip the plugin to "ready" with empty
    // metadata, because the UI would then read default ditBlocks / sampler
    // settings for every model with no cue that the handshake was truncated.
    uint16_t infoLen = 0;
    if (!readExact(&infoLen, 2, 5000))
    {
        lastError_ = "Backend ready signal received but device/model JSON length did not arrive";
        juce::Logger::writeToLog("PipeInference: " + lastError_);
        shutdown();
        launching_ = false;
        return false;
    }

    if (infoLen == 0)
    {
        lastError_ = "Backend ready signal received but device/model JSON was empty";
        juce::Logger::writeToLog("PipeInference: " + lastError_);
        shutdown();
        launching_ = false;
        return false;
    }

    std::vector<char> infoBuf(infoLen + 1, 0);
    if (!readExact(infoBuf.data(), infoLen, 5000))
    {
        lastError_ = "Backend ready signal received but device/model JSON did not arrive";
        juce::Logger::writeToLog("PipeInference: " + lastError_);
        shutdown();
        launching_ = false;
        return false;
    }

    auto infoJson = juce::JSON::parse(juce::String::fromUTF8(infoBuf.data(), infoLen));
    if (!infoJson.isObject())
    {
        lastError_ = "Backend ready signal received but device/model JSON failed to parse";
        juce::Logger::writeToLog("PipeInference: " + lastError_);
        shutdown();
        launching_ = false;
        return false;
    }

    if (auto* arr = infoJson.getProperty("devices", {}).getArray())
    {
        for (auto& d : *arr)
            availableDevices_.add(d.toString());
    }
    defaultDevice_ = infoJson.getProperty("default", "cpu").toString();

    if (auto* arr = infoJson.getProperty("models", {}).getArray())
    {
        for (auto& m : *arr)
            availableModels_.add(m.toString());
    }
    defaultModel_ = infoJson.getProperty("default_model", "").toString();

    if (auto* metaObj = infoJson.getProperty("model_metadata", {}).getDynamicObject())
    {
        for (auto& prop : metaObj->getProperties())
        {
            ModelMetadata md;
            if (auto* inner = prop.value.getDynamicObject())
            {
                const auto blocksVar = inner->getProperty("dit_blocks");
                if (blocksVar.isInt() || blocksVar.isDouble())
                {
                    const int parsed = static_cast<int>(blocksVar);
                    if (parsed > 0)
                        md.ditBlocks = parsed;
                }
                const auto samplerVar = inner->getProperty("sampler_type");
                if (samplerVar.isString())
                {
                    const auto str = samplerVar.toString();
                    if (str.isNotEmpty())
                        md.samplerType = str;
                }
            }
            modelMetadata_[prop.name.toString()] = md;
        }
    }

    juce::Logger::writeToLog("PipeInference: devices=" + availableDevices_.joinIntoString(",")
                              + " default=" + defaultDevice_
                              + " models=" + availableModels_.joinIntoString(",")
                              + " default_model=" + defaultModel_
                              + " metadata_entries=" + juce::String((int) modelMetadata_.size()));

    ready_ = true;
    launching_ = false;
    juce::Logger::writeToLog("PipeInference: ready");
    return true;
}

void PipeInference::shutdown()
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    ready_ = false;
#ifdef _WIN32
    if (hChildStdinWr_ != INVALID_HANDLE_VALUE) { CloseHandle(hChildStdinWr_); hChildStdinWr_ = INVALID_HANDLE_VALUE; }
    if (hChildStdoutRd_ != INVALID_HANDLE_VALUE) { CloseHandle(hChildStdoutRd_); hChildStdoutRd_ = INVALID_HANDLE_VALUE; }
    if (hProcess_ != INVALID_HANDLE_VALUE)
    {
        TerminateProcess(hProcess_, 0);
        WaitForSingleObject(hProcess_, 3000);
        CloseHandle(hProcess_);
        hProcess_ = INVALID_HANDLE_VALUE;
    }
    // After the process is gone: closing the job is the backstop for anything
    // TerminateProcess missed (a child the backend spawned), and it must not be
    // left open for a relaunch to inherit.
    if (hJob_ != nullptr) { CloseHandle(hJob_); hJob_ = nullptr; }
#else
    if (stdinFd_ >= 0) { close(stdinFd_); stdinFd_ = -1; }
    if (stdoutFd_ >= 0) { close(stdoutFd_); stdoutFd_ = -1; }
    if (childPid_ > 0)
    {
        kill(childPid_, SIGTERM);
        // Wait up to 3s for graceful exit, then force-kill
        for (int i = 0; i < 30; ++i)
        {
            if (waitpid(childPid_, nullptr, WNOHANG) != 0)
                break;
            juce::Thread::sleep(100);
        }
        kill(childPid_, SIGKILL);
        waitpid(childPid_, nullptr, 0);
        childPid_ = -1;
    }
#endif
}

bool PipeInference::readExact(void* dest, int numBytes, int timeoutMs)
{
    auto* ptr = static_cast<char*>(dest);
    int remaining = numBytes;
    auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);

    while (remaining > 0)
    {
        if (juce::Time::getMillisecondCounter() > deadline)
            return false;

       #ifdef _WIN32
        if (hChildStdoutRd_ == INVALID_HANDLE_VALUE) return false;
        DWORD bytesRead = 0;
        if (!ReadFile(hChildStdoutRd_, ptr, static_cast<DWORD>(remaining), &bytesRead, nullptr))
            return false;
        if (bytesRead == 0) return false;  // pipe broken
        ptr += bytesRead;
        remaining -= static_cast<int>(bytesRead);
       #else
        if (stdoutFd_ < 0) return false;
        auto n = read(stdoutFd_, ptr, static_cast<size_t>(remaining));
        if (n > 0)
        {
            ptr += n;
            remaining -= static_cast<int>(n);
        }
        else if (n == 0)
        {
            return false;  // EOF — child died
        }
        else
        {
            if (errno == EINTR) continue;
            juce::Thread::sleep(1);
        }
       #endif
    }
    return true;
}

bool PipeInference::writeExact(const void* src, int numBytes)
{
    if (!isConnected()) return false;
    auto* ptr = static_cast<const char*>(src);
    int remaining = numBytes;
    while (remaining > 0)
    {
       #ifdef _WIN32
        DWORD bytesWritten = 0;
        if (!WriteFile(hChildStdinWr_, ptr, static_cast<DWORD>(remaining), &bytesWritten, nullptr))
            return false;
        ptr += bytesWritten;
        remaining -= static_cast<int>(bytesWritten);
       #else
        auto n = write(stdinFd_, ptr, static_cast<size_t>(remaining));
        if (n > 0)
        {
            ptr += n;
            remaining -= static_cast<int>(n);
        }
        else if (n < 0 && errno != EINTR)
        {
            return false;
        }
       #endif
    }
    return true;
}

PipeInference::Result PipeInference::generate(const Request& request)
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    Result result;

    // Check if subprocess is alive, auto-restart if dead
    if (ready_ && !isChildAlive())
    {
        juce::Logger::writeToLog("PipeInference: subprocess died, restarting...");
        if (!tryRestart())
        {
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
            return result;
        }
        juce::Logger::writeToLog("PipeInference: restarted successfully");
    }

    if (!ready_ || !isConnected())
    {
        result.errorMessage = "Inference not ready";
        return result;
    }

    // Build JSON request
    auto json = juce::DynamicObject::Ptr(new juce::DynamicObject());
    json->setProperty("prompt_a", request.promptA);
    json->setProperty("prompt_b", request.promptB);
    json->setProperty("alpha", request.alpha);
    json->setProperty("magnitude", request.magnitude);
    json->setProperty("noise_sigma", request.noiseSigma);
    json->setProperty("duration", request.durationSeconds);
    json->setProperty("start_pos", request.startPosition);
    json->setProperty("steps", request.steps);
    json->setProperty("cfg_scale", request.cfgScale);
    json->setProperty("seed", request.seed);
    if (request.device.isNotEmpty())
        json->setProperty("device", request.device);
    if (request.model.isNotEmpty())
        json->setProperty("model", request.model);
    // SA3 modality (Music/SFX). The Music and SFX slots can be backed by the SAME
    // single-checkpoint SA3 model (medium/large), so the domain travels here rather
    // than in the model id. Omitted when empty → the backend falls back to its
    // dir-name sniff (small-music/small-sfx), so non-SA3 and older requests are
    // byte-identical to before.
    if (request.trackType.isNotEmpty())
        json->setProperty("track_type", request.trackType);

    // Per-preset modality-routing epoch. ALWAYS serialised (explicit on the wire); the
    // backend acts on it only for SA3 and treats an ABSENT field as legacy (pre-2.5.0).
    json->setProperty("modality_epoch", request.modalityEpoch);

    // Research-mode prompt-injection fields. Always serialised so that the bundled
    // backend sees them; older backends ignore unknown fields.
    json->setProperty("injection_mode", request.injectionMode);
    json->setProperty("injection_transition_at", request.injectionTransitionAt);
    json->setProperty("late_phase_alpha", request.latePhaseAlpha);
    json->setProperty("split_start", request.splitStart);
    json->setProperty("split_end",   request.splitEnd);

    // Serialize dimension offsets from DimensionExplorer
    if (!request.dimensionOffsets.empty())
    {
        juce::Array<juce::var> offsets;
        for (auto& [idx, val] : request.dimensionOffsets)
        {
            juce::Array<juce::var> pair;
            pair.add(idx);
            pair.add(val);
            offsets.add(juce::var(pair));
        }
        json->setProperty("dimension_offsets", juce::var(offsets));
    }

    // Serialize semantic axes
    if (!request.semanticAxes.empty())
    {
        auto axesObj = juce::DynamicObject::Ptr(new juce::DynamicObject());
        for (auto& [key, val] : request.semanticAxes)
            axesObj->setProperty(key, val);
        json->setProperty("semantic_axes", juce::var(axesObj.get()));
        json->setProperty("axes_amount", request.axesAmount);
    }

    // Resynth / init_audio (i2i): only emitted when a source buffer is present.
    // Wire format matches the backend's _decode_init_audio_request: raw float32
    // little-endian PCM, base64, planar (channel-major — all of channel 0, then
    // channel 1). Absent → backend takes the plain text-only path.
    if (request.initAudio.getNumSamples() > 0 && request.initAudio.getNumChannels() > 0)
    {
        const int ch = request.initAudio.getNumChannels();
        const int n  = request.initAudio.getNumSamples();
        std::vector<float> planar (static_cast<size_t>(ch) * static_cast<size_t>(n));
        for (int c = 0; c < ch; ++c)
            std::memcpy(planar.data() + static_cast<size_t>(c) * static_cast<size_t>(n),
                        request.initAudio.getReadPointer(c),
                        static_cast<size_t>(n) * sizeof(float));
        json->setProperty("init_audio_b64",
                          juce::Base64::toBase64(planar.data(), planar.size() * sizeof(float)));
        json->setProperty("init_audio_sr", static_cast<int>(request.initAudioSampleRate + 0.5));
        json->setProperty("init_audio_channels", ch);
        json->setProperty("init_noise_level", request.initNoiseLevel);
    }

    auto jsonStr = juce::JSON::toString(juce::var(json.get()), true);
    jsonStr = jsonStr.removeCharacters("\n\r") + "\n";

    // Write request
    if (!writeExact(jsonStr.toRawUTF8(), static_cast<int>(jsonStr.getNumBytesAsUTF8())))
    {
        // Write failed — subprocess likely dead
        if (tryRestart())
        {
            result.errorMessage = juce::String::fromUTF8("Inference restarted \xe2\x80\x94 try again");
        }
        else
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
        return result;
    }

    // Read response status byte (3 min timeout for long first-request warm loads)
    char status = 0;
    if (!readExact(&status, 1, 180000))
    {
        // Timeout or dead process
        if (!isChildAlive())
        {
            juce::Logger::writeToLog("PipeInference: subprocess died during generation");
            tryRestart();
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restarted, try again");
        }
        else
            result.errorMessage = "Timeout waiting for response";
        return result;
    }

    if (status == '\x00')
    {
        juce::uint32 msgLen = 0;
        if (readExact(&msgLen, 4))
        {
            std::vector<char> msg(msgLen + 1, 0);
            readExact(msg.data(), static_cast<int>(msgLen));
            result.errorMessage = juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen));
        }
        else
            result.errorMessage = "Unknown error";
        return result;
    }

    if (status != '\x01')
    {
        result.errorMessage = "Unexpected response: " + juce::String((int)status);
        return result;
    }

    // Read header: flag(i32), samples(i32), channels(i32), sampleRate(i32), seed(i32), timeMs(f32)
    struct { int32_t flag, samples, channels, sampleRate, seed; float timeMs; } header;
    if (!readExact(&header, sizeof(header)))
    {
        result.errorMessage = "Failed to read header";
        return result;
    }

    int totalFloats = header.samples * header.channels;
    std::vector<float> pcm(static_cast<size_t>(totalFloats));
    if (!readExact(pcm.data(), totalFloats * static_cast<int>(sizeof(float))))
    {
        result.errorMessage = "Failed to read PCM data (" + juce::String(totalFloats * 4) + " bytes)";
        return result;
    }

    // Build AudioBuffer [channels, samples]
    result.audio.setSize(header.channels, header.samples);
    for (int ch = 0; ch < header.channels; ++ch)
    {
        auto* dest = result.audio.getWritePointer(ch);
        auto* src = pcm.data() + static_cast<size_t>(ch) * static_cast<size_t>(header.samples);
        std::memcpy(dest, src, static_cast<size_t>(header.samples) * sizeof(float));
    }

    result.success = true;
    result.sampleRate = header.sampleRate > 0 ? static_cast<double>(header.sampleRate) : 44100.0;
    result.seed = header.seed;
    result.generationTimeMs = header.timeMs;

    // Read embedding stats (uint16 num_dims + float32[dims] A/B/baseline)
    uint16_t numDims = 0;
    if (readExact(&numDims, 2, 5000) && numDims > 0)
    {
        result.embeddingA.resize(numDims);
        result.embeddingB.resize(numDims);
        result.embeddingBaseline.resize(numDims);
        readExact(result.embeddingA.data(), numDims * static_cast<int>(sizeof(float)), 5000);
        readExact(result.embeddingB.data(), numDims * static_cast<int>(sizeof(float)), 5000);
        readExact(result.embeddingBaseline.data(), numDims * static_cast<int>(sizeof(float)), 5000);
    }

    return result;
}

void PipeInference::setAuthorProviderConfig(const AuthorProviderConfig& config)
{
    const std::lock_guard<std::mutex> lock(authorConfigMutex_);
    authorProviderConfig_ = config;
}

PipeInference::ApiSpend PipeInference::authorApiSpend() const
{
    const std::lock_guard<std::mutex> lock(apiSpendMutex_);
    return apiSpend_;
}

void PipeInference::addAuthorProviderFields(juce::DynamicObject::Ptr json) const
{
    AuthorProviderConfig config;
    {
        const std::lock_guard<std::mutex> lock(authorConfigMutex_);
        config = authorProviderConfig_;
    }
    // Provider empty (the default, local model) sends nothing — the entire
    // point is that a local-only user's wire bytes never change.
    if (config.provider.isEmpty())
        return;
    json->setProperty("coder_provider", config.provider);
    json->setProperty("coder_api_key", config.apiKey.trim());
    json->setProperty("coder_api_model", config.apiModel.trim());
    if (config.apiBase.isNotEmpty())
        json->setProperty("coder_api_base", config.apiBase.trim());
}

PipeInference::TranslateResult PipeInference::translate(const juce::String& text,
                                                        const juce::String& device)
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    TranslateResult result;

    // Empty input → empty translation; no subprocess round-trip.
    if (text.trim().isEmpty())
    {
        result.success = true;
        return result;
    }

    // Auto-restart if the subprocess died (mirrors generate()).
    if (ready_ && !isChildAlive())
    {
        juce::Logger::writeToLog("PipeInference: subprocess died, restarting...");
        if (!tryRestart())
        {
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
            return result;
        }
        juce::Logger::writeToLog("PipeInference: restarted successfully");
    }

    if (!ready_ || !isConnected())
    {
        result.errorMessage = "Inference not ready";
        return result;
    }

    auto json = juce::DynamicObject::Ptr(new juce::DynamicObject());
    json->setProperty("mode", "translate");
    json->setProperty("prompt_a", text);
    if (device.isNotEmpty())
        json->setProperty("device", device);
    // No model key: the backend resolves the ONE language author itself —
    // local (_resolve_coder_model_dir) unless authorProviderConfig_ names an
    // external API (addAuthorProviderFields), same as authorCsoundOrchestra().
    addAuthorProviderFields(json);

    auto jsonStr = juce::JSON::toString(juce::var(json.get()), true);
    jsonStr = jsonStr.removeCharacters("\n\r") + "\n";

    if (!writeExact(jsonStr.toRawUTF8(), static_cast<int>(jsonStr.getNumBytesAsUTF8())))
    {
        if (tryRestart())
            result.errorMessage = juce::String::fromUTF8("Inference restarted \xe2\x80\x94 try again");
        else
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
        return result;
    }

    // The first translate lazily loads the translation model (several
    // seconds), so allow a generous status-byte timeout like generate().
    char status = 0;
    if (!readExact(&status, 1, 180000))
    {
        result.errorMessage = handleStatusTimeout("translation");
        return result;
    }

    if (status == '\x03')   // text result
    {
        juce::uint32 msgLen = 0;
        if (!readExact(&msgLen, 4))
        {
            result.errorMessage = "Failed to read translation length";
            return result;
        }
        if (msgLen > 0)
        {
            std::vector<char> msg(msgLen + 1, 0);
            if (!readExact(msg.data(), static_cast<int>(msgLen)))
            {
                result.errorMessage = "Failed to read translation text";
                return result;
            }
            result.text = juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen));
        }
        result.success = true;
        return result;
    }

    if (status == '\x00')   // error
    {
        juce::uint32 msgLen = 0;
        if (readExact(&msgLen, 4))
        {
            std::vector<char> msg(msgLen + 1, 0);
            readExact(msg.data(), static_cast<int>(msgLen));
            result.errorMessage = juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen));
        }
        else
            result.errorMessage = "Unknown error";
        return result;
    }

    result.errorMessage = "Unexpected response: " + juce::String((int)status);
    return result;
}

PipeInference::InterpretResult PipeInference::interpret(const juce::String& systemPrompt,
                                                        const juce::String& userText,
                                                        int maxNewTokens,
                                                        const juce::String& device)
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    InterpretResult result;

    // Empty input → empty interpretation; no subprocess round-trip.
    if (userText.trim().isEmpty())
    {
        result.success = true;
        return result;
    }

    // Auto-restart if the subprocess died (mirrors translate()).
    if (ready_ && !isChildAlive())
    {
        juce::Logger::writeToLog("PipeInference: subprocess died, restarting...");
        if (!tryRestart())
        {
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
            return result;
        }
        juce::Logger::writeToLog("PipeInference: restarted successfully");
    }

    if (!ready_ || !isConnected())
    {
        result.errorMessage = "Inference not ready";
        return result;
    }

    auto json = juce::DynamicObject::Ptr(new juce::DynamicObject());
    json->setProperty("mode", "interpret");
    json->setProperty("system_prompt", systemPrompt);
    json->setProperty("prompt_a", userText);
    // maxNewTokens <= 0 means NO CAP: omit the key entirely so the backend's
    // run_instruct takes its uncapped branch and sizes the reply to the real
    // model context (ctx - used - 16, pipe_inference.py:1049-1067). The backend
    // deliberately has no default cap of its own and documents this call site as
    // the owner of that decision — so sending a number here is the ONLY way a
    // hard output limit can enter the system, and a truncated reply is a silent
    // mid-sentence cut, not an error.
    if (maxNewTokens > 0)
        json->setProperty("max_new_tokens", maxNewTokens);
    if (device.isNotEmpty())
        json->setProperty("device", device);
    // No model key: as in translate() above, the backend resolves the one
    // language author itself unless authorProviderConfig_ names an external API.
    addAuthorProviderFields(json);

    auto jsonStr = juce::JSON::toString(juce::var(json.get()), true);
    jsonStr = jsonStr.removeCharacters("\n\r") + "\n";

    if (!writeExact(jsonStr.toRawUTF8(), static_cast<int>(jsonStr.getNumBytesAsUTF8())))
    {
        if (tryRestart())
            result.errorMessage = juce::String::fromUTF8("Inference restarted \xe2\x80\x94 try again");
        else
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
        return result;
    }

    // First interpret lazily loads the instruct model (several seconds).
    char status = 0;
    if (!readExact(&status, 1, 180000))
    {
        result.errorMessage = handleStatusTimeout("interpretation");
        return result;
    }

    if (status == '\x03')   // text result
    {
        juce::uint32 msgLen = 0;
        if (!readExact(&msgLen, 4))
        {
            result.errorMessage = "Failed to read interpretation length";
            return result;
        }
        if (msgLen > 0)
        {
            std::vector<char> msg(msgLen + 1, 0);
            if (!readExact(msg.data(), static_cast<int>(msgLen)))
            {
                result.errorMessage = "Failed to read interpretation text";
                return result;
            }
            result.text = juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen));
        }
        result.success = true;
        return result;
    }

    if (status == '\x00')   // error
    {
        juce::uint32 msgLen = 0;
        if (readExact(&msgLen, 4))
        {
            std::vector<char> msg(msgLen + 1, 0);
            readExact(msg.data(), static_cast<int>(msgLen));
            result.errorMessage = juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen));
        }
        else
            result.errorMessage = "Unknown error";
        return result;
    }

    result.errorMessage = "Unexpected response: " + juce::String((int)status);
    return result;
}

PipeInference::CsoundAuthorResult PipeInference::authorCsoundOrchestra(const juce::String& text,
                                                                       const juce::String& correction,
                                                                       const juce::String& previous,
                                                                       const juce::var& synthParams,
                                                                       std::function<void(int, const juce::String&)> onThinking,
                                                                       std::function<void(int, const juce::String&)> onBody,
                                                                       std::function<void(int, int, const juce::String&)> onAttempt)
{
    // Same lock/restart/timeout discipline as interpret() (mode "csound" on
    // the wire) — the backend's {ok, orchestra, reading, params_text, spec,
    // error} response is decoded here into CsoundAuthorResult's typed fields,
    // so the caller (PromptPanel) never has to touch raw JSON.
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    CsoundAuthorResult result;

    if (text.trim().isEmpty())
    {
        // Deliberate divergence from interpret() (which returns success on
        // empty input): authoring without a prompt is a caller error, not a
        // no-op.
        result.errorMessage = "Empty prompt";
        return result;
    }

    // Auto-restart if the subprocess died (mirrors interpret()).
    if (ready_ && !isChildAlive())
    {
        juce::Logger::writeToLog("PipeInference: subprocess died, restarting...");
        if (!tryRestart())
        {
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
            return result;
        }
        juce::Logger::writeToLog("PipeInference: restarted successfully");
    }

    if (!ready_ || !isConnected())
    {
        result.errorMessage = "Inference not ready";
        return result;
    }

    auto json = juce::DynamicObject::Ptr(new juce::DynamicObject());
    json->setProperty("mode", "csound");
    json->setProperty("text", text);
    // Omitted when empty, so a first authoring's wire bytes are unchanged.
    // `previous` rides only alongside a correction: on its own it would put a
    // patch in front of the model with no reason to change it.
    if (correction.trim().isNotEmpty())
    {
        json->setProperty("correction", correction.trim());
        if (previous.trim().isNotEmpty())
            json->setProperty("previous", previous.trim());
    }
    // Asking for the interim frames is what makes the backend send them. A build
    // that does not know \x04 never sets this and never receives one — an
    // unrecognised status byte would not spoil one request but desynchronise the
    // pipe for the rest of the session.
    if (onThinking || onBody || onAttempt)
        json->setProperty("stream", true);
    // The synth's own knobs, with their current values and their ranges. Sent
    // ONLY when the player allowed the author to set them — absent means the
    // author is never told they exist, so it does not spend reasoning on knobs
    // it may not touch, and an older backend that ignores the field behaves
    // exactly as it does today.
    if (auto* shelf = synthParams.getArray(); shelf != nullptr && ! shelf->isEmpty())
        json->setProperty("synth_params", synthParams);
    addAuthorProviderFields(json);

    auto jsonStr = juce::JSON::toString(juce::var(json.get()), true);
    jsonStr = jsonStr.removeCharacters("\n\r") + "\n";

    if (!writeExact(jsonStr.toRawUTF8(), static_cast<int>(jsonStr.getNumBytesAsUTF8())))
    {
        if (tryRestart())
            result.errorMessage = juce::String::fromUTF8("Inference restarted \xe2\x80\x94 try again");
        else
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
        return result;
    }

    // First call lazily loads the instruct model (several seconds) — a
    // generous timeout.
    //
    // Interim \x04 frames may arrive first, any number of them: the author's
    // reasoning as it is written. They do not end the request, so each one
    // restarts the wait for the frame that does — which is also why the timeout
    // is per-frame and not a budget for the whole authoring.
    char status = 0;
    for (;;)
    {
        if (!readExact(&status, 1, 180000))
        {
            result.errorMessage = handleStatusTimeout("the Csound orchestra");
            return result;
        }
        if (status != '\x04')
            break;

        juce::uint32 partLen = 0;
        if (!readExact(&partLen, 4))
        {
            result.errorMessage = "Failed to read Csound progress length";
            return result;
        }
        juce::String partJson;
        if (partLen > 0)
        {
            std::vector<char> part(partLen + 1, 0);
            // The payload must be consumed even if nobody is listening, or the
            // remaining bytes would be read as the next frame's status.
            if (!readExact(part.data(), static_cast<int>(partLen)))
            {
                result.errorMessage = "Failed to read Csound progress";
                return result;
            }
            partJson = juce::String::fromUTF8(part.data(), static_cast<int>(partLen));
        }
        if (partJson.isNotEmpty())
        {
            // Dispatch by kind; a kind this build does not know is ignored by
            // contract (§4.6), never treated as an error.
            const auto part = juce::JSON::parse(partJson);
            const auto kind = part.getProperty("kind", juce::var()).toString();
            const int  att  = static_cast<int>(part.getProperty("attempt", juce::var(1)));
            if (kind == "thinking" && onThinking)
                onThinking(att, part.getProperty("text", juce::var()).toString());
            else if (kind == "body" && onBody)
                onBody(att, part.getProperty("text", juce::var()).toString());
            else if (kind == "attempt" && onAttempt)
            {
                juce::StringArray errs;
                if (auto* arr = part.getProperty("errors", juce::var()).getArray())
                    for (auto& e : *arr)
                        errs.add(e.toString());
                onAttempt(att, static_cast<int>(part.getProperty("max", juce::var(0))),
                          errs.joinIntoString("; "));
            }
        }
    }

    if (status == '\x03')   // text result: the {ok, orchestra, reading, spec, error} JSON
    {
        juce::uint32 msgLen = 0;
        if (!readExact(&msgLen, 4))
        {
            result.errorMessage = "Failed to read Csound response length";
            return result;
        }
        juce::String responseJson;
        if (msgLen > 0)
        {
            std::vector<char> msg(msgLen + 1, 0);
            if (!readExact(msg.data(), static_cast<int>(msgLen)))
            {
                result.errorMessage = "Failed to read Csound response";
                return result;
            }
            responseJson = juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen));
        }
        if (responseJson.isEmpty())
        {
            result.errorMessage = "Empty Csound response";
            return result;
        }

        const auto parsed = juce::JSON::parse(responseJson);
        const bool ok = static_cast<bool>(parsed.getProperty("ok", juce::var(false)));

        // Read BEFORE the ok/else split, because a failed authoring is a paid
        // authoring: the repair loop bills every round it took before giving
        // up, and those are the expensive generations. Absent on the wire while
        // the local model authors, and absent from an older backend entirely —
        // in both cases the last known total simply stands.
        if (const auto spend = parsed.getProperty("api_spend", juce::var()); spend.isObject())
        {
            ApiSpend s;
            s.calls        = static_cast<int>(spend.getProperty("calls", juce::var(0)));
            s.tokenCalls   = static_cast<int>(spend.getProperty("token_calls", juce::var(0)));
            s.costCalls    = static_cast<int>(spend.getProperty("cost_calls", juce::var(0)));
            s.inputTokens  = static_cast<juce::int64>(spend.getProperty("input", juce::var(0)));
            s.outputTokens = static_cast<juce::int64>(spend.getProperty("output", juce::var(0)));
            s.cost         = static_cast<double>(spend.getProperty("cost", juce::var(0.0)));
            const std::lock_guard<std::mutex> lock(apiSpendMutex_);
            apiSpend_ = s;
        }

        if (ok)
        {
            result.orchestra  = parsed.getProperty("orchestra", juce::var()).toString();
            result.reading    = parsed.getProperty("reading", juce::var()).toString();
            result.paramsText = parsed.getProperty("params_text", juce::var()).toString();
            result.authorModel = parsed.getProperty("author_model", juce::var()).toString();

            // The authoring trace (lco_write.py's "consultation"/"repairs").
            // Every field is optional on the wire: a backend older than this
            // build simply leaves them absent, and the panel then shows the
            // stations it has rather than empty ones — the trace degrades, the
            // orchestra does not.
            auto toStrings = [](const juce::var& v)
            {
                juce::StringArray out;
                if (auto* arr = v.getArray())
                    for (auto& e : *arr)
                        out.add(e.toString());
                return out;
            };
            result.thinking = parsed.getProperty("thinking", juce::var()).toString();
            const auto consultation = parsed.getProperty("consultation", juce::var());
            const auto named = consultation.getProperty("named", juce::var());
            result.namedInstruments = toStrings(named.getProperty("instruments", juce::var()));
            result.namedAdjectives  = toStrings(named.getProperty("adjectives", juce::var()));
            result.namedMotions     = toStrings(named.getProperty("motions", juce::var()));
            const auto opened = consultation.getProperty("opened", juce::var());
            result.openedInstruments = toStrings(opened.getProperty("instruments", juce::var()));
            result.openedAdjectives  = toStrings(opened.getProperty("adjectives", juce::var()));
            result.openedMotions     = toStrings(opened.getProperty("motions", juce::var()));
            result.libraryEntryCount = static_cast<int>(consultation.getProperty("library_size", juce::var(0)));
            result.repairs            = toStrings(parsed.getProperty("repairs", juce::var()));
            // Only present when the request carried the shelf; a backend that
            // does not know the field simply omits it and nothing is set.
            result.settings           = parsed.getProperty("settings", juce::var());
            // Absent from a backend older than the knob contract, which then
            // simply means "this instrument has no knobs".
            result.controls           = parsed.getProperty("controls", juce::var());
            result.attempts           = static_cast<int>(parsed.getProperty("attempts", juce::var(0)));

            result.success    = result.orchestra.isNotEmpty();
            if (!result.success)
                result.errorMessage = "Empty orchestra in Csound response";
        }
        else
        {
            // Honest failure from build_csound_response (parse/assembler error
            // surviving the one retry) — LLM-first, no fallback: never
            // synthesize a default orchestra here.
            result.errorMessage = parsed.getProperty("error", juce::var("Unknown Csound authoring error")).toString();
        }
        return result;
    }

    if (status == '\x00')   // error
    {
        juce::uint32 msgLen = 0;
        if (readExact(&msgLen, 4))
        {
            std::vector<char> msg(msgLen + 1, 0);
            readExact(msg.data(), static_cast<int>(msgLen));
            result.errorMessage = juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen));
        }
        else
            result.errorMessage = "Unknown error";
        return result;
    }

    result.errorMessage = "Unexpected response: " + juce::String((int)status);
    return result;
}

PipeInference::AnalyzeResult PipeInference::analyze(const juce::AudioBuffer<float>& audio,
                                                    double sampleRate,
                                                    int topk,
                                                    const juce::String& device)
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    AnalyzeResult result;

    const int ch = audio.getNumChannels();
    const int n  = audio.getNumSamples();
    if (ch <= 0 || n <= 0)
    {
        result.errorMessage = "analyze: empty audio buffer";
        return result;
    }

    // Auto-restart if the subprocess died (mirrors translate()).
    if (ready_ && !isChildAlive())
    {
        juce::Logger::writeToLog("PipeInference: subprocess died, restarting...");
        if (!tryRestart())
        {
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
            return result;
        }
        juce::Logger::writeToLog("PipeInference: restarted successfully");
    }

    if (!ready_ || !isConnected())
    {
        result.errorMessage = "Inference not ready";
        return result;
    }

    auto json = juce::DynamicObject::Ptr(new juce::DynamicObject());
    json->setProperty("mode", "analyze");

    // Audio travels on the init_audio wire keys (identical to generate()'s resynth
    // path / the backend's _decode_init_audio_request): raw float32 little-endian
    // PCM, base64, planar (channel-major).
    std::vector<float> planar (static_cast<size_t>(ch) * static_cast<size_t>(n));
    for (int c = 0; c < ch; ++c)
        std::memcpy(planar.data() + static_cast<size_t>(c) * static_cast<size_t>(n),
                    audio.getReadPointer(c),
                    static_cast<size_t>(n) * sizeof(float));
    json->setProperty("init_audio_b64",
                      juce::Base64::toBase64(planar.data(), planar.size() * sizeof(float)));
    json->setProperty("init_audio_sr", static_cast<int>(sampleRate + 0.5));
    json->setProperty("init_audio_channels", ch);
    json->setProperty("topk", topk);
    if (device.isNotEmpty())
        json->setProperty("device", device);

    auto jsonStr = juce::JSON::toString(juce::var(json.get()), true);
    jsonStr = jsonStr.removeCharacters("\n\r") + "\n";

    if (!writeExact(jsonStr.toRawUTF8(), static_cast<int>(jsonStr.getNumBytesAsUTF8())))
    {
        if (tryRestart())
            result.errorMessage = juce::String::fromUTF8("Inference restarted \xe2\x80\x94 try again");
        else
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restart failed");
        return result;
    }

    // First analyze lazily loads CLAP (several seconds; downloads on first ever use).
    char status = 0;
    if (!readExact(&status, 1, 180000))
    {
        if (!isChildAlive())
        {
            juce::Logger::writeToLog("PipeInference: subprocess died during analyze");
            tryRestart();
            result.errorMessage = juce::String::fromUTF8("Inference crashed \xe2\x80\x94 restarted, try again");
        }
        else
            result.errorMessage = "Timeout waiting for analysis";
        return result;
    }

    if (status == '\x03')   // text (JSON) result
    {
        juce::uint32 msgLen = 0;
        if (!readExact(&msgLen, 4))
        {
            result.errorMessage = "Failed to read analysis length";
            return result;
        }
        if (msgLen > 0)
        {
            std::vector<char> msg(msgLen + 1, 0);
            if (!readExact(msg.data(), static_cast<int>(msgLen)))
            {
                result.errorMessage = "Failed to read analysis text";
                return result;
            }
            auto parsed = juce::JSON::parse(juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen)));
            result.tags     = parsed.getProperty("tags", juce::var("")).toString();
            result.spectral = parsed.getProperty("spectral", juce::var("")).toString();
        }
        result.success = true;
        return result;
    }

    if (status == '\x00')   // error
    {
        juce::uint32 msgLen = 0;
        if (readExact(&msgLen, 4))
        {
            std::vector<char> msg(msgLen + 1, 0);
            readExact(msg.data(), static_cast<int>(msgLen));
            result.errorMessage = juce::String::fromUTF8(msg.data(), static_cast<int>(msgLen));
        }
        else
            result.errorMessage = "Unknown error";
        return result;
    }

    result.errorMessage = "Unexpected response: " + juce::String((int)status);
    return result;
}

PipeInference::ModelMetadata PipeInference::getModelMetadata(const juce::String& modelName) const
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    auto it = modelMetadata_.find(modelName);
    if (it == modelMetadata_.end())
        return {};
    return it->second;
}

bool PipeInference::preload(const juce::String& model, const juce::String& device)
{
    const std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    if (!ready_ || !isConnected()) return false;

    auto json = juce::DynamicObject::Ptr(new juce::DynamicObject());
    json->setProperty("mode", "preload");
    json->setProperty("model", model);
    if (device.isNotEmpty())
        json->setProperty("device", device);

    auto jsonStr = juce::JSON::toString(juce::var(json.get()), true);
    jsonStr = jsonStr.removeCharacters("\n\r") + "\n";

    if (!writeExact(jsonStr.toRawUTF8(), static_cast<int>(jsonStr.getNumBytesAsUTF8())))
        return false;

    // Read and discard the preload response (empty audio)
    char status = 0;
    if (!readExact(&status, 1, 120000)) return false;

    if (status == '\x01')
    {
        // Read and discard header + 0-sample PCM + embedding footer
        struct { int32_t flag, samples, channels, sampleRate, seed; float timeMs; } header;
        if (!readExact(&header, sizeof(header))) return false;
        int totalFloats = header.samples * header.channels;
        if (totalFloats > 0)
        {
            std::vector<float> discard(static_cast<size_t>(totalFloats));
            readExact(discard.data(), totalFloats * static_cast<int>(sizeof(float)));
        }
        uint16_t numDims = 0;
        readExact(&numDims, 2, 5000);
        if (numDims > 0)
        {
            std::vector<float> discard(numDims * 3);
            readExact(discard.data(), numDims * 3 * static_cast<int>(sizeof(float)), 5000);
        }
        return true;
    }
    else if (status == '\x00')
    {
        // Error — read and discard error message
        juce::uint32 msgLen = 0;
        if (readExact(&msgLen, 4))
        {
            std::vector<char> msg(msgLen + 1, 0);
            readExact(msg.data(), static_cast<int>(msgLen));
        }
    }
    return false;
}
