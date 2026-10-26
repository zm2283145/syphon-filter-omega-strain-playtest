#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <typeinfo>
#include <memory>
#include <functional>
#include <map>
#include <chrono>
#include <unistd.h>

struct Base { virtual ~Base() = default; };
struct Derived : Base { int v = 7; };
struct PortError : std::runtime_error { using std::runtime_error::runtime_error; };

static FILE* g_log;
static int g_fail;
#define CHECK(c, msg) do { bool ok_ = (c); std::fprintf(g_log, "%s %s\n", ok_ ? "PASS" : "FAIL", msg); std::fflush(g_log); if (!ok_) ++g_fail; } while (0)

__attribute__((noinline)) static void thrower(int depth) {
    std::vector<std::string> guard{"a", "b"};
    if (depth == 0) throw PortError("not ported: 0x0024cde4");
    thrower(depth - 1);
}

int main() {
    g_log = std::fopen("/download0/cxx_selftest.log", "w");
    if (!g_log) g_log = stdout;
    std::fprintf(g_log, "ps5 C++ self-test\n");
    try { thrower(5); CHECK(false, "throw reached catch"); }
    catch (const std::runtime_error& e) { CHECK(std::string(e.what()) == "not ported: 0x0024cde4", "catch derived by base ref"); }
    try { std::vector<int> v; (void)v.at(3); CHECK(false, "at() threw"); }
    catch (const std::out_of_range&) { CHECK(true, "libc++ throws out_of_range"); }
    try { throw 42; } catch (int x) { CHECK(x == 42, "throw int"); }
    try { try { throw PortError("inner"); } catch (...) { throw; } } catch (const PortError& e) { CHECK(true, "rethrow"); }
    std::unique_ptr<Base> b = std::make_unique<Derived>();
    CHECK(dynamic_cast<Derived*>(b.get()) && dynamic_cast<Derived*>(b.get())->v == 7, "dynamic_cast");
    CHECK(std::string(typeid(*b).name()).find("Derived") != std::string::npos, "typeid name");
    std::atomic<int> count{0}; std::mutex m; std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) ts.emplace_back([&] { for (int k = 0; k < 1000; ++k) { std::lock_guard<std::mutex> l(m); ++count; } });
    for (auto& t : ts) t.join();
    CHECK(count == 4000, "std::thread + mutex");
    std::thread tex([&] { try { throw PortError("thread"); } catch (const std::exception&) { ++count; } });
    tex.join();
    CHECK(count == 4001, "exception on worker thread");
    std::error_code ec;
    CHECK(std::filesystem::exists("/app0/eboot.bin", ec), "filesystem::exists /app0/eboot.bin");
    std::filesystem::create_directories("/download0/selftest/dir", ec);
    CHECK(!ec && std::filesystem::is_directory("/download0/selftest/dir"), "filesystem::create_directories");
    try { std::filesystem::file_size("/app0/does_not_exist"); CHECK(false, "fs throws"); }
    catch (const std::filesystem::filesystem_error&) { CHECK(true, "filesystem_error thrown"); }
    std::ostringstream os; os << "x=" << 1.5 << " " << 0x10;
    CHECK(os.str() == "x=1.5 16", "ostringstream");
    auto t0 = std::chrono::steady_clock::now(); std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(15), "steady_clock/sleep_for");
    std::fprintf(g_log, "RESULT %s (%d failures)\n", g_fail ? "FAIL" : "OK", g_fail);
    std::fflush(g_log);
    for (;;) sleep(60);
}
