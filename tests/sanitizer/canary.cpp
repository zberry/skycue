// The sanitizer canary (scripts/run-sanitizers.sh --canary): proves that a
// report from a child process lands in the sanitizer log directory, the way
// a skycued spawned by a shell test or test_web_server would. Built only on
// request (EXCLUDE_FROM_ALL) in an instrumented tree.
//
// The child does a signed overflow on a value derived from argc (UBSan) and
// an out-of-bounds heap read (ASan), or, under TSan, two threads writing one
// variable without synchronisation. The parent waits and exits 0 whatever
// the child did: only the log tells.

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

int child(int argc)
{
#ifdef SKYCUE_CANARY_TSAN
    std::thread a([argc] { g_shared = argc; });
    std::thread b([argc] { g_shared = argc + 1; });
    a.join();
    b.join();
    std::printf("canary child: %d\n", g_shared);
#else
    volatile int base = INT_MAX;
    const int sum = base + argc;  // signed overflow for any argc >= 1: UBSan
    std::printf("canary child: %d\n", sum);
    int* heap = new int[4]{};
    volatile int index = 3 + argc;  // 4 with no arguments: one past the end
    const int past = heap[index];   // heap-buffer-overflow: ASan
    std::printf("canary child: %d\n", past);
    delete[] heap;
#endif
    return 0;
}

}  // namespace

int main(int argc, char**)
{
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("canary: fork");
        return 0;
    }
    if (pid == 0)
        ::_exit(child(argc));
    int status = 0;
    ::waitpid(pid, &status, 0);
    std::printf("canary: child exited, status %d\n", status);
    return 0;
}
