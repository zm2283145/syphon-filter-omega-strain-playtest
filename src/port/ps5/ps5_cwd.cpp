// Port-only (132ps5c, research/PS5.md "Paths"): the working folder on the PS5. A homebrew title's sandbox refuses
// chdir (EPERM, first hardware run) and the runtime's getcwd is not usable (the call faulted), yet the shared play host
// works in a working folder as on the PC (config/disc_us.json, local/..., mods). So the platform layer keeps the
// working folder itself: the link wraps (--wrap, cmake/ps5/OmegaPs5Targets.cmake) every C library call that takes a
// path, and a relative path is made absolute against this folder before the real call. chdir only checks that the
// folder exists and records it; getcwd returns it. Absolute paths pass through unchanged.
//
// Imports: a native title's loader resolves only what libkernel (and the loaded Sce modules) export; the functions the
// payload SDK's stubs put in libkernel_sys or libScePosixForWebKit stay null (132ps5c hw5: fchmodat, link, openat,
// pathconf, readlink, symlink, unlinkat, among the 11 unresolved). So those wrappers are complete here, over libkernel's
// open / chmod / rmdir / unlink / stat, and never reach a __real_ call:
// - openat / unlinkat / fchmodat with a directory descriptor: the title remembers the absolute path of every directory
//   it opens (keyed by descriptor, checked against the descriptor's st_dev / st_ino), which is how libc++'s
//   filesystem::remove_all walks a tree;
// - link / symlink: the file system makes no links (EPERM, as POSIX allows for a file system without them);
// - readlink: there are no symbolic links, so an existing path is not one (EINVAL);
// - pathconf: the FreeBSD values (PATH_MAX 1024, NAME_MAX 255, LINK_MAX 32767, ...), after the path is checked;
// - opendir / fdopendir / readdir / closedir: libSceLibcInternal's opendir calls fstatfs on the directory (FreeBSD's
//   union-mount check), which this process may not make (EPERM: 132ps5c hw8, directory_iterator on the memory card
//   folder), so the title lists folders itself: open(O_DIRECTORY) and libkernel's getdents;
// - statvfs: libSceLibcInternal's statvfs resolves, but it calls statfs, which a native title does not have (132ps5c
//   hw6: it jumped to a heap address from filesystem::space, FolderAgentCard::free_bytes, as the front end left the
//   intro movie). The title opens the path and asks libkernel's _fstatfs, then fills statvfs as FreeBSD's libc does.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "ps5_log.hpp"

extern "C" {
// FreeBSD 11's struct statfs (the payload SDK's <sys/mount.h>, which needs kernel headers to include), and libkernel's
// export of fstatfs under its underscore name.
struct ps5_statfs {
    uint32_t f_version, f_type;
    uint64_t f_flags, f_bsize, f_iosize, f_blocks, f_bfree;
    int64_t f_bavail;
    uint64_t f_files;
    int64_t f_ffree;
    uint64_t f_syncwrites, f_asyncwrites, f_syncreads, f_asyncreads, f_spare[10];
    uint32_t f_namemax, f_owner;
    int32_t f_fsid[2];
    char f_charspare[80], f_fstypename[16], f_mntfromname[88], f_mntonname[88];
};
static_assert(sizeof(ps5_statfs) == 472);
int _fstatfs(int fd, ps5_statfs* out);
int __real_access(const char*, int);
FILE* __real_fopen(const char*, const char*);
FILE* __real_freopen(const char*, const char*, FILE*);
int __real_lstat(const char*, struct stat*);
int __real_mkdir(const char*, mode_t);
int __real_open(const char*, int, ...);
int __real_remove(const char*);
int __real_rename(const char*, const char*);
int __real_stat(const char*, struct stat*);
int __real_truncate(const char*, off_t);
int __real_unlink(const char*);
int __real_utimes(const char*, const struct timeval*);
}

namespace {

std::mutex g_mutex;
std::string g_cwd = "/";

// The lexically normal absolute form (".", ".." and repeated slashes folded; no symlinks are read).
std::string normal(const std::string& path) {
    std::string out;
    std::size_t at = 0;
    while (at <= path.size()) {
        std::size_t end = path.find('/', at);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(at, end - at);
        if (part == "..") {
            const std::size_t slash = out.rfind('/');
            out.erase(slash == std::string::npos ? 0 : slash);
        } else if (!part.empty() && part != ".") {
            out += '/';
            out += part;
        }
        at = end + 1;
    }
    return out.empty() ? "/" : out;
}

std::string absolute(const char* path) {
    if (!path) return {};
    if (path[0] == '/') return path;
    std::lock_guard lock(g_mutex);
    return normal(g_cwd + "/" + path);
}

// The absolute paths of the directories the title has open, by descriptor (with the identity they had when opened, so
// a descriptor number reused by another file is not mistaken for the directory).
struct OpenDirectory {
    std::string path;
    dev_t device;
    ino_t inode;
};
std::mutex g_directory_mutex;
std::map<int, OpenDirectory> g_directories;

// Records (or forgets) descriptor `fd` just opened for absolute path `path`.
void note_open(int fd, const char* path) {
    if (fd < 0 || !path) return;
    struct stat st{};
    const bool directory = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode);
    std::lock_guard lock(g_directory_mutex);
    if (directory)
        g_directories[fd] = OpenDirectory{path, st.st_dev, st.st_ino};
    else
        g_directories.erase(fd);
}

// The absolute path `path` names relative to directory descriptor `fd` (AT_FDCWD: the working folder). False with errno
// set as openat would (EBADF, ENOTDIR) when `fd` is not a directory the title opened.
bool at_path(int fd, const char* path, std::string& out);

// A relative path's absolute form in `buffer` (an absolute one as given).
const char* resolve(const char* path, std::string& buffer) {
    if (!path || path[0] == '/' || path[0] == 0) return path;
    buffer = absolute(path);
    return buffer.c_str();
}

bool at_path(int fd, const char* path, std::string& out) {
    if (!path) {
        errno = EFAULT;
        return false;
    }
    if (path[0] == 0) {
        errno = ENOENT;
        return false;
    }
    if (path[0] == '/' || fd == AT_FDCWD) {
        out = absolute(path);
        return true;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0) return false;  // EBADF
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    std::lock_guard lock(g_directory_mutex);
    const auto found = g_directories.find(fd);
    if (found == g_directories.end() || found->second.device != st.st_dev || found->second.inode != st.st_ino) {
        errno = EBADF;  // a directory opened outside the title's wrappers: its path is unknown
        return false;
    }
    out = normal(found->second.path + "/" + path);
    return true;
}

// The title's directory stream (handed out as DIR*): the descriptor and the getdents buffer.
struct DirStream {
    int fd;
    std::vector<char> buffer;
    int length = 0;
    int position = 0;
};

}  // namespace

extern "C" {

int __wrap_chdir(const char* path) {
    if (!path || !*path) {
        errno = ENOENT;
        return -1;
    }
    const std::string folder = absolute(path);
    struct stat st{};
    if (__real_stat(folder.c_str(), &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    std::lock_guard lock(g_mutex);
    g_cwd = normal(folder);
    return 0;
}

char* __wrap_getcwd(char* buffer, size_t size) {
    std::string cwd;
    {
        std::lock_guard lock(g_mutex);
        cwd = g_cwd;
    }
    if (!buffer) {
        if (size == 0) size = cwd.size() + 1;
        if (size < cwd.size() + 1) {
            errno = ERANGE;
            return nullptr;
        }
        buffer = static_cast<char*>(malloc(size));
        if (!buffer) {
            errno = ENOMEM;
            return nullptr;
        }
    } else if (size < cwd.size() + 1) {
        errno = size ? ERANGE : EINVAL;
        return nullptr;
    }
    memcpy(buffer, cwd.c_str(), cwd.size() + 1);
    return buffer;
}

char* __wrap_realpath(const char* path, char* resolved) {
    if (!path) {
        errno = EINVAL;
        return nullptr;
    }
    const std::string full = absolute(path);
    struct stat st{};
    if (__real_stat(full.c_str(), &st) != 0) return nullptr;
    if (full.size() + 1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return nullptr;
    }
    if (!resolved) resolved = static_cast<char*>(malloc(PATH_MAX));
    if (!resolved) {
        errno = ENOMEM;
        return nullptr;
    }
    memcpy(resolved, full.c_str(), full.size() + 1);
    return resolved;
}

int __wrap_access(const char* p, int mode) {
    std::string b;
    return __real_access(resolve(p, b), mode);
}
int __wrap_fchmodat(int fd, const char* p, mode_t mode, int flag) {
    if (flag & ~AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    std::string full;
    if (!at_path(fd, p, full)) return -1;
    if (flag & AT_SYMLINK_NOFOLLOW) {
        struct stat st{};
        if (__real_lstat(full.c_str(), &st) != 0) return -1;
        if (S_ISLNK(st.st_mode)) {
            errno = EOPNOTSUPP;
            return -1;
        }
    }
    return chmod(full.c_str(), mode);
}
FILE* __wrap_fopen(const char* p, const char* mode) {
    std::string b;
    return __real_fopen(resolve(p, b), mode);
}
FILE* __wrap_freopen(const char* p, const char* mode, FILE* f) {
    std::string b;
    return __real_freopen(resolve(p, b), mode, f);
}
int __wrap_link(const char* a, const char* b) {
    std::string x, y;
    struct stat st{};
    if (!a || !b) {
        errno = EFAULT;
        return -1;
    }
    if (__real_lstat(resolve(a, x), &st) != 0) return -1;
    if (__real_lstat(resolve(b, y), &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    errno = EPERM;  // the file system makes no hard links
    return -1;
}
int __wrap_lstat(const char* p, struct stat* st) {
    std::string b;
    return __real_lstat(resolve(p, b), st);
}
int __wrap_mkdir(const char* p, mode_t mode) {
    std::string b;
    return __real_mkdir(resolve(p, b), mode);
}
// mkstemp is not exported by the PS5's system modules (its import is null: 132ps5c hw4, the GL driver's shader cache
// faulted calling it), so the title makes the file itself: the template's trailing XXXXXX from a counter and the
// clock, created with O_CREAT | O_EXCL.
int __wrap_mkstemp(char* pattern) {
    if (!pattern) {
        errno = EINVAL;
        return -1;
    }
    const std::size_t n = strlen(pattern);
    if (n < 6 || strcmp(pattern + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static const char digits[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static std::uint64_t counter = 0;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        std::uint64_t v;
        {
            std::lock_guard lock(g_mutex);
            v = (++counter * 0x9E3779B97F4A7C15ull) ^ std::uint64_t(now.tv_nsec) ^ (std::uint64_t(now.tv_sec) << 20);
        }
        for (int k = 0; k < 6; ++k, v /= 36) pattern[n - 6 + k] = digits[v % 36];
        std::string b;
        const int fd = __real_open(resolve(pattern, b), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    errno = EEXIST;
    return -1;
}
int __wrap_open(const char* p, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    std::string b;
    const char* full = resolve(p, b);
    const int fd = __real_open(full, flags, mode);
    note_open(fd, full);
    return fd;
}
int __wrap_openat(int fd, const char* p, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    std::string full;
    if (!at_path(fd, p, full)) return -1;
    const int opened = __real_open(full.c_str(), flags, mode);
    note_open(opened, full.c_str());
    return opened;
}
DIR* __wrap_fdopendir(int fd) {
    struct stat st{};
    if (fstat(fd, &st) != 0) return nullptr;  // EBADF
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return nullptr;
    }
    auto* d = new (std::nothrow) DirStream{fd, {}};
    if (!d) {
        errno = ENOMEM;
        return nullptr;
    }
    // At least a directory block (FreeBSD's getdirentries needs DIRBLKSIZ or more) and the file system's block size.
    const std::size_t size = st.st_blksize > 32768 ? std::size_t(st.st_blksize) : 32768;
    try {
        d->buffer.resize(size);
    } catch (...) {
        delete d;
        errno = ENOMEM;
        return nullptr;
    }
    return reinterpret_cast<DIR*>(d);
}
DIR* __wrap_opendir(const char* p) {
    if (!p) {
        errno = EFAULT;
        return nullptr;
    }
    std::string b;
    const char* full = resolve(p, b);
    const int fd = __real_open(full, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return nullptr;
    note_open(fd, full);
    DIR* d = __wrap_fdopendir(fd);
    if (!d) {
        const int saved = errno;
        close(fd);
        errno = saved;
    }
    return d;
}
struct dirent* __wrap_readdir(DIR* dir) {
    auto* d = reinterpret_cast<DirStream*>(dir);
    if (!d) {
        errno = EBADF;
        return nullptr;
    }
    for (;;) {
        if (d->position >= d->length) {
            const int n = getdents(d->fd, d->buffer.data(), static_cast<int>(d->buffer.size()));
            if (n <= 0) {  // 0: the end (errno untouched); -1: errno from getdents
                if (n < 0) {
                    static bool logged = false;
                    const int saved = errno;
                    if (!logged) omega::ps5::log("readdir: getdents failed, errno %d", saved);
                    logged = true;
                    errno = saved;
                }
                d->length = d->position = 0;
                return nullptr;
            }
            d->length = n;
            d->position = 0;
        }
        auto* entry = reinterpret_cast<struct dirent*>(d->buffer.data() + d->position);
        if (entry->d_reclen == 0 || d->position + entry->d_reclen > d->length) {  // a malformed record: stop
            d->length = d->position = 0;
            errno = EIO;
            return nullptr;
        }
        d->position += entry->d_reclen;
        if (entry->d_fileno != 0) return entry;  // 0: a deleted entry's slot
    }
}
int __wrap_closedir(DIR* dir) {
    auto* d = reinterpret_cast<DirStream*>(dir);
    if (!d) {
        errno = EBADF;
        return -1;
    }
    {
        std::lock_guard lock(g_directory_mutex);
        g_directories.erase(d->fd);
    }
    const int result = close(d->fd);
    delete d;
    return result;
}
long __wrap_pathconf(const char* p, int name) {
    std::string b;
    struct stat st{};
    if (__real_stat(resolve(p, b), &st) != 0) return -1;
    switch (name) {
    case _PC_LINK_MAX: return 32767;
    case _PC_MAX_CANON: return 255;
    case _PC_MAX_INPUT: return 255;
    case _PC_NAME_MAX: return 255;
    case _PC_PATH_MAX: return PATH_MAX;
    case _PC_PIPE_BUF: return 512;
    case _PC_CHOWN_RESTRICTED: return 1;
    case _PC_NO_TRUNC: return 1;
    case _PC_VDISABLE: return 0xff;
    default: errno = EINVAL; return -1;
    }
}
ssize_t __wrap_readlink(const char* p, char* out, size_t size) {
    (void)out;
    (void)size;
    std::string b;
    struct stat st{};
    if (__real_lstat(resolve(p, b), &st) != 0) return -1;
    errno = S_ISLNK(st.st_mode) ? EOPNOTSUPP : EINVAL;  // no symbolic links on this file system
    return -1;
}
int __wrap_remove(const char* p) {
    std::string b;
    return __real_remove(resolve(p, b));
}
int __wrap_rename(const char* a, const char* b) {
    std::string x, y;
    return __real_rename(resolve(a, x), resolve(b, y));
}
int __wrap_stat(const char* p, struct stat* st) {
    std::string b;
    return __real_stat(resolve(p, b), st);
}
int __wrap_statvfs(const char* p, struct statvfs* st) {
    if (!p || !st) {
        errno = EFAULT;
        return -1;
    }
    std::string b;
    const int fd = __real_open(resolve(p, b), O_RDONLY);
    if (fd < 0) return -1;
    {
        std::lock_guard lock(g_directory_mutex);
        g_directories.erase(fd);
    }
    ps5_statfs sf{};
    const int result = _fstatfs(fd, &sf);
    const int saved = errno;
    close(fd);
    // 132ps5c hw7: the memory card reported no room (FolderAgentCard::free_bytes is this call's f_bavail * f_frsize),
    // so every query is logged (the first 8, then whenever the answer changes).
    {
        static std::mutex log_mutex;
        static unsigned logged = 0;
        static int64_t last_bavail = -1;
        std::lock_guard lock(log_mutex);
        if (logged < 2 || (result == 0 && sf.f_bavail != last_bavail)) {
            ++logged;
            last_bavail = result == 0 ? sf.f_bavail : -1;
            omega::ps5::log("statvfs %s: _fstatfs %d errno %d version 0x%08x type %u bsize %llu iosize %llu blocks %llu "
                            "bfree %llu bavail %lld fs '%.16s' on '%.88s'",
                            p, result, result ? saved : 0, sf.f_version, sf.f_type,
                            (unsigned long long)sf.f_bsize, (unsigned long long)sf.f_iosize,
                            (unsigned long long)sf.f_blocks, (unsigned long long)sf.f_bfree, (long long)sf.f_bavail,
                            sf.f_fstypename, sf.f_mntonname);
        }
    }
    if (result != 0 || sf.f_blocks == 0) {
        // PORT-ONLY fallback: the kernel gave no usable answer. Rather than a full disk (the front end's memory card
        // would refuse every save), report a file system of the PS2 memory card's capacity (8 MiB), all of it free.
        static bool fallback_logged = false;
        if (!fallback_logged)
            omega::ps5::log("statvfs %s: no usable answer (_fstatfs %d, errno %d, blocks %llu); reporting 8 MiB free "
                            "(the PS2 card's capacity, port-only fallback; logged once)",
                            p, result, result ? saved : 0, (unsigned long long)sf.f_blocks);
        fallback_logged = true;
        *st = {};
        st->f_bsize = st->f_frsize = 1024;
        st->f_blocks = st->f_bfree = st->f_bavail = 8192;
        st->f_files = st->f_ffree = st->f_favail = 4096;
        st->f_namemax = 255;
        return 0;
    }
    // f_bsize 0 (a file system reporting only its transfer size): count in the transfer size instead.
    if (sf.f_bsize == 0) sf.f_bsize = sf.f_iosize ? sf.f_iosize : 512;
    if (sf.f_iosize == 0) sf.f_iosize = sf.f_bsize;
    // As FreeBSD's libc statvfs: counts in fragments (f_frsize = statfs f_bsize), f_bsize the transfer size.
    *st = {};
    st->f_bsize = sf.f_iosize;
    st->f_frsize = sf.f_bsize;
    st->f_blocks = sf.f_blocks;
    st->f_bfree = sf.f_bfree;
    st->f_bavail = sf.f_bavail < 0 ? 0 : static_cast<fsblkcnt_t>(sf.f_bavail);
    st->f_files = sf.f_files;
    st->f_ffree = sf.f_ffree < 0 ? 0 : static_cast<fsfilcnt_t>(sf.f_ffree);
    st->f_favail = st->f_ffree;
    st->f_fsid = static_cast<unsigned long>(static_cast<uint32_t>(sf.f_fsid[0]));
    st->f_namemax = sf.f_namemax;
    st->f_flag = ((sf.f_flags & 0x1) ? ST_RDONLY : 0) | ((sf.f_flags & 0x8) ? ST_NOSUID : 0);  // MNT_RDONLY, MNT_NOSUID
    return 0;
}
int __wrap_symlink(const char* target, const char* p) {
    if (!target || !p) {
        errno = EFAULT;
        return -1;
    }
    std::string b;
    struct stat st{};
    if (__real_lstat(resolve(p, b), &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    errno = EPERM;  // the file system makes no symbolic links
    return -1;
}
int __wrap_truncate(const char* p, off_t size) {
    std::string b;
    return __real_truncate(resolve(p, b), size);
}
int __wrap_unlink(const char* p) {
    std::string b;
    return __real_unlink(resolve(p, b));
}
int __wrap_unlinkat(int fd, const char* p, int flag) {
    if (flag & ~AT_REMOVEDIR) {
        errno = EINVAL;
        return -1;
    }
    std::string full;
    if (!at_path(fd, p, full)) return -1;
    return (flag & AT_REMOVEDIR) ? rmdir(full.c_str()) : __real_unlink(full.c_str());
}
int __wrap_utimes(const char* p, const struct timeval* times) {
    std::string b;
    return __real_utimes(resolve(p, b), times);
}

}  // extern "C"
