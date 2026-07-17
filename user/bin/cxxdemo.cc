/* user/bin/cxxdemo.cc -> /cxxdemo.elf -- the first C++ program on EmbLinkOS.
 *
 * Deliberately exercises the things that actually BREAK on a fresh C++ port,
 * in dependency order, and reports each as a pass/fail line rather than
 * printing "hello" and calling it proof:
 *
 *   1. global constructors ran before main   (crt0 .init_array/.ctors)
 *   2. constructor ORDER within a TU         (the linker's array order)
 *   3. new/delete                            (operator new -> malloc -> sbrk)
 *   4. templates + a non-trivial class       (the compiler proper)
 *   5. function-local static init            (__cxa_guard_acquire -- a real
 *                                             libsupc++ symbol, so this fails
 *                                             to LINK if libstdc++ is absent)
 *   6. destructors at exit                   (__cxa_atexit + .fini_array)
 *
 * Freestanding-friendly on purpose: no <iostream> (it drags in locales, the
 * whole ios_base init, and static ctors of its own -- worth trying LATER, as
 * a separate step, once this baseline is green). Uses plain write(2) so a
 * failure here is unambiguously C++ and not stdio.
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <iostream>

static void say(const char *s) { write(1, s, strlen(s)); }
static void report(const char *what, bool ok) {
    say(ok ? "  ok   " : "  FAIL ");
    say(what);
    say("\n");
}

static int g_fails = 0;
static void check(const char *what, bool ok) { if (!ok) g_fails++; report(what, ok); }

/* --- 1 & 2: global constructors, and their order --------------------------- */
static int g_ctor_seq = 0;

struct Marker {
    int order;
    /* a real constructor with a side effect -- if crt0 never walks the init
     * array this stays 0 and `order` keeps its .bss zero */
    Marker() : order(++g_ctor_seq) {}
};
static Marker g_first;    /* should get order 1 */
static Marker g_second;   /* should get order 2 */

/* --- 4: a template + a class with a destructor ----------------------------- */
template <typename T>
struct Box {
    T v;
    explicit Box(T x) : v(x) {}
    T twice() const { return v + v; }
};

struct Tracked {
    static int live;
    Tracked()  { live++; }
    ~Tracked() { live--; }
};
int Tracked::live = 0;

/* --- 6: a destructor that must run at exit --------------------------------- */
struct AtExit {
    ~AtExit() { say("cxxdemo: global dtor ran at exit\n"); }
};
static AtExit g_atexit;

int main() {
    say("cxxdemo: C++ on EmbLinkOS\n");

    check("global constructors ran before main", g_ctor_seq == 2);
    check("constructor order within the TU",     g_first.order == 1 && g_second.order == 2);

    /* new/delete -> operator new -> malloc -> sbrk (the syscall that was
     * growing the WRONG process's heap until recently) */
    Tracked *t = new Tracked();
    check("new constructed the object", Tracked::live == 1);
    delete t;
    check("delete destructed it",       Tracked::live == 0);

    /* an array new, so the cookie/array path gets touched too */
    Tracked *arr = new Tracked[3];
    check("new[] constructed 3",  Tracked::live == 3);
    delete[] arr;
    check("delete[] destructed 3", Tracked::live == 0);

    /* templates: instantiated for two types */
    Box<int> bi(21);
    Box<double> bd(1.5);
    check("template<int>",    bi.twice() == 42);
    check("template<double>", bd.twice() == 3.0);

    /* function-local static: needs __cxa_guard_acquire/release from
     * libsupc++ -- this is the one that proves libstdc++ is really linked */
    for (int i = 0; i < 3; i++) {
        static Box<int> lazy(7);
        check("function-local static init", lazy.v == 7);
        break;
    }

    /* --- the STL, which is the actual point of having C++ --------------- */
    std::string s = "Emb";
    s += "Link";
    s.append("OS");
    check("std::string concat", s == "EmbLinkOS" && s.size() == 9);
    check("std::string find",   s.find("Link") == 3);

    std::vector<int> v;
    for (int i = 1; i <= 5; i++) v.push_back(i * i);   /* heap growth + realloc */
    int sum = 0;
    for (int x : v) sum += x;                          /* range-for */
    check("std::vector + range-for", v.size() == 5 && sum == 55);

    std::vector<std::string> vs{ "beta", "alpha" };    /* init-list, non-POD elems */
    check("vector<string> init-list", vs.size() == 2 && vs[1] == "alpha");

    /* --- iostream: the heavyweight. Its ios_base::Init static ctor must
     * have run (another .ctors entry, from libstdc++ itself) for cout to be
     * usable at all -- so this doubles as proof the ctor walk handles the
     * LIBRARY's own constructors, not just ours. */
    std::cout << "cxxdemo: iostream says hello from " << s
              << " (" << v.size() << " squares, sum " << sum << ")" << std::endl;

    char buf[64];
    snprintf(buf, sizeof buf, "cxxdemo: %s (%d failure(s))\n",
             g_fails ? "FAIL" : "all green", g_fails);
    say(buf);
    return g_fails ? 1 : 0;   /* exit code -> `test cxx` asserts 0 */
}
