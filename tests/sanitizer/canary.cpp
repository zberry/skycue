// The sanitizer canary (scripts/dev/run-sanitizers.sh --canary): proves that a
// report from a child process lands in the sanitizer log directory, the way
// a skycued spawned by a shell test or test_web_server would. Built only on
// request (EXCLUDE_FROM_ALL) in an instrumented tree.
//
// Under ASan + UBSan it forks two children, one per sanitizer, because
// halt_on_error stops a child at its first report: one does a signed
// overflow on a value derived from argc (UBSan), the other an out-of-bounds
// heap read (ASan). Under TSan one child runs two threads writing one
// variable without synchronisation. The parent waits for each child and
// exits 0 whatever they did: only the logs tell.

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <cstdio>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SKYCUE_CANARY_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) && !defined(SKYCUE_CANARY_TSAN)
#define SKYCUE_CANARY_TSAN 1
#endif

#ifdef SKYCUE_CANARY_TSAN
#include <thread>
namespace {
int g_shared = 0;  // written by two threads with no lock: the TSan report
}
#endif

namespace {

#ifdef SKYCUE_CANARY_TSAN
int raceChild(int argc)
{
    std::thread a([argc] { g_shared = argc; });
    std::thread b([argc] { g_shared = argc + 1; });
    a.join();
    b.join();
    std::printf("canary child: %d\n", g_shared);
    return 0;
}
#else
int overflowChild(int argc)
{
    volatile int base = INT_MAX;
    const int sum = base + argc;  // signed overflow for any argc >= 1: UBSan
    std::printf("canary child: %d\n", sum);
    return 0;
}

int heapChild(int argc)
{
    int* heap = new int[4]{};
    volatile int index = 3 + argc;  // 4 with no arguments: one past the end
    const int past = heap[index];   // heap-buffer-overflow: ASan
    std::printf("canary child: %d\n", past);
    delete[] heap;
    return 0;
}
#endif

// Forks, runs fn in the child and waits for it.
void runChild(const char* name, int (*fn)(int), int argc)
{
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("canary: fork");
        return;
    }
    if (pid == 0)
        ::_exit(fn(argc));
    int status = 0;
    ::waitpid(pid, &status, 0);
    std::printf("canary: %s child exited, status %d\n", name, status);
}

}  // namespace

int main(int argc, char**)
{
#ifdef SKYCUE_CANARY_TSAN
    runChild("tsan", raceChild, argc);
#else
    runChild("ubsan", overflowChild, argc);
    runChild("asan", heapChild, argc);
#endif
    return 0;
}
