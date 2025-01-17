/* -*- Mode: C; indent-tabs-mode: nil; tab-width: 4 -*-
 *
 * Copyright (C) 2015-2017 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define __USE_GNU

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <vector>
#include <unistd.h>

#ifndef SNAPCRAFT_LIBNAME_DEF
#define SNAPCRAFT_LIBNAME_DEF "libsnapcraft-preload.so"
#endif

#define LITERAL_STRLEN(s) (sizeof (s) - 1)

#define LOG_0 (0)
#define LOG_1 (1)
#define LOG_2 (2)
#define LOG_3 (3)

namespace
{
const std::string SNAPCRAFT_LIBNAME = SNAPCRAFT_LIBNAME_DEF;
const std::string SNAPCRAFT_PRELOAD = "SNAPCRAFT_PRELOAD";
const std::string LD_PRELOAD = "LD_PRELOAD";
const std::string LD_LINUX = "/lib/ld-linux.so.2";
const std::string DEFAULT_VARLIB = "/var/lib";
const std::string DEFAULT_DEVSHM = "/dev/shm/";
const std::string DEFAULT_RUN = "/run/";
const std::string DEFAULT_RUNUSER = "/run/user/";
const std::string DEFAULT_VARRUN = "/var/run/";
const std::string DEFAULT_VARRUNUSER = "/var/run/user/";

std::string saved_snapcraft_preload;
std::string saved_varlib;
std::string saved_snap_name;
std::string saved_snap_revision;
std::string saved_snap_devshm;
std::string saved_snap_run;
std::string saved_snap_varrun;

std::vector<std::string> saved_ld_preloads;

int saved_verbose_level = 0;

int (*_access) (const char *, int) = NULL;

template <typename dirent_t>
using filter_function_t = int (*)(const dirent_t *);
template <typename dirent_t>
using compar_function_t = int (*)(const dirent_t **, const dirent_t **);

using socket_action_t = int (*) (int, const struct sockaddr *, socklen_t);
using execve_t = int (*) (const char *, char *const[], char *const[]);

inline std::string
getenv_string(const std::string& varname)
{
    char *envvar = secure_getenv (varname.c_str());
    return envvar ? envvar : "";
}

inline bool
str_starts_with(const std::string& str, std::string const& prefix)
{
    return str.compare (0, prefix.size (), prefix) == 0;
}

inline bool
str_ends_with(const std::string& str, std::string const& sufix)
{
    if (str.size () < sufix.size ())
        return false;

    return str.compare (str.size() - sufix.size (), sufix.size (), sufix) == 0;
}

struct Initializer { Initializer (); };
static Initializer initalizer;

Initializer::Initializer()
{
    _access = (decltype(_access)) dlsym (RTLD_NEXT, "access");

    // We need to save LD_PRELOAD and SNAPCRAFT_PRELOAD in case we need to
    // propagate the values to an exec'd program.
    std::string const& ld_preload = getenv_string (LD_PRELOAD);
    if (ld_preload.empty ()) {
        return;
    }

    saved_snapcraft_preload = getenv_string (SNAPCRAFT_PRELOAD);
    if (saved_snapcraft_preload.empty ()) {
        return;
    }

    saved_varlib = getenv_string ("SNAP_DATA");
    saved_snap_name = getenv_string ("SNAP_NAME");
    saved_snap_revision = getenv_string ("SNAP_REVISION");
    saved_snap_devshm = DEFAULT_DEVSHM + "snap." + saved_snap_name;
    saved_snap_run = DEFAULT_RUN + "snap." + saved_snap_name;
    saved_snap_varrun = DEFAULT_VARRUN + "snap." + saved_snap_name;

    // Pull out each absolute-pathed libsnapcraft-preload.so we find.  Better to
    // accidentally include some other libsnapcraft-preload than not propagate
    // ourselves.
    std::string p;
    std::istringstream ss (ld_preload);
    while (std::getline (ss, p, ':')) {
        if (str_ends_with (p, "/" SNAPCRAFT_LIBNAME_DEF)) {
            saved_ld_preloads.push_back (p);
        }
    }

	try {
		saved_verbose_level = std::stoi (getenv_string ("SNAPCRAFT_PRELOAD_VERBOSE"), nullptr, 0);
	}
	catch (const std::invalid_argument& e) {
		saved_verbose_level = 0;
	}
	catch (const std::out_of_range& e) {
		saved_verbose_level = INT_MAX;
	}
}

void
log_msg(int level, const char *s, ...)
{
	if (saved_verbose_level >= level) {
		va_list va;
		va_start(va, s);
		fprintf(stderr, "PRELOAD: ");
		vfprintf(stderr, s, va);
		va_end(va);
	}
}

inline void
string_length_sanitize(std::string& path)
{
    if (path.size () >= PATH_MAX) {
        std::cerr << "snapcraft-preload: path '" << path << "' exceeds PATH_MAX size (" << PATH_MAX << ") and it will be cut.\n"
                  << "Expect undefined behavior";
        path.resize (PATH_MAX);
    }
}

std::string
redirect_writable_path (std::string const& pathname, std::string const& basepath)
{
    if (pathname.empty ()) {
        return pathname;
    }

    std::string redirected_pathname (basepath);

    if (redirected_pathname.back () == '/' && pathname.back () == '/') {
        redirected_pathname.resize (redirected_pathname.size () - 1);
    }

    redirected_pathname += pathname;
    string_length_sanitize (redirected_pathname);

    return redirected_pathname;
}

std::string
redirect_path_full (std::string const& pathname, bool check_parent, bool only_if_absolute)
{
    if (pathname.empty ()) {
        return pathname;
    }

    const std::string& preload_dir = saved_snapcraft_preload;
    if (preload_dir.empty()) {
        return pathname;
    }

    if (only_if_absolute && pathname[0] != '/') {
        return pathname;
    }

    // And each app should have its own /var/lib writable tree.  Here, we want
    // to support reading the base system's files if they exist, else let the app
    // play in /var/lib themselves.  So we reverse the normal check: first see if
    // it exists in root, else do our redirection.
    if (pathname == DEFAULT_VARLIB || str_starts_with (pathname, DEFAULT_VARLIB + '/')) {
        if (!saved_varlib.empty () && !str_starts_with (pathname, saved_varlib) && _access (pathname.c_str(), F_OK) != 0) {
            return redirect_writable_path (pathname.data () + DEFAULT_VARLIB.size (), saved_varlib);
        } else {
            return pathname;
        }
    }

    // Some apps want to open shared memory in random locations. Here we will confine it to the
    // snaps allowed path.
    std::string redirected_pathname;

    log_msg(LOG_1, "%s: path '%s'.\n", __func__, pathname.c_str());
    if (str_starts_with (pathname, DEFAULT_DEVSHM) && !str_starts_with (pathname, saved_snap_devshm)) {
		log_msg(LOG_2, "rule: DEVSHM\n");
        std::string new_pathname = pathname.substr(DEFAULT_DEVSHM.size());
		if (new_pathname[0] == '/')
			new_pathname.erase(0, 1);	// if the first character is still '/', remove it
        redirected_pathname = saved_snap_devshm + '.' + new_pathname;
        string_length_sanitize (redirected_pathname);
        return redirected_pathname;
    }

	// TODO: fix the path of /run/user/[uid]/
    if (str_starts_with (pathname, DEFAULT_RUN) && !str_starts_with (pathname, DEFAULT_RUNUSER)
			&& !str_starts_with (pathname, saved_snap_run)) {
		log_msg(LOG_2, "rule: RUN\n");
        std::string new_pathname = pathname.substr(DEFAULT_RUN.size());
		if (new_pathname[0] == '/')
			new_pathname.erase(0, 1);	// if the first character is still '/', remove it
        redirected_pathname = saved_snap_run + '/' + new_pathname;
        string_length_sanitize (redirected_pathname);
        return redirected_pathname;
    }

    if (str_starts_with (pathname, DEFAULT_VARRUN) && !str_starts_with (pathname, DEFAULT_VARRUNUSER)
			&& !str_starts_with (pathname, saved_snap_varrun)) {
		log_msg(LOG_2, "rule: VARRUN\n");
        std::string new_pathname = pathname.substr(DEFAULT_VARRUN.size());
		if (new_pathname[0] == '/')
			new_pathname.erase(0, 1);	// if the first character is still '/', remove it
        redirected_pathname = saved_snap_varrun + '/' + new_pathname;
        string_length_sanitize (redirected_pathname);
        return redirected_pathname;
    }

    redirected_pathname = preload_dir;
    if (redirected_pathname.back () == '/') {
        redirected_pathname.resize(redirected_pathname.size ()-1);
    }

    if (pathname[0] != '/') {
        std::string cwd;
        cwd.reserve(PATH_MAX);
        if (getcwd (const_cast<char*>(cwd.data ()), PATH_MAX) == NULL) {
            return pathname;
        }

        redirected_pathname += cwd + '/';
    }

    redirected_pathname += pathname;
    size_t slash_pos = std::string::npos;

    if (check_parent) {
        slash_pos = redirected_pathname.find_last_of ('/');
        if (slash_pos != std::string::npos) { // should always be true
            redirected_pathname[slash_pos] = 0;
        }
    }

    int ret = _access (redirected_pathname.c_str (), F_OK);

    if (check_parent && slash_pos != std::string::npos) {
        redirected_pathname[slash_pos] = '/';
    }

    if (ret == 0 || errno == ENOTDIR) { // ENOTDIR is OK because it exists at least
        string_length_sanitize (redirected_pathname);
        return redirected_pathname;
    } else {
        return pathname;
    }
}

inline std::string
redirect_path (std::string const& pathname)
{
    return redirect_path_full (pathname, /*check_parent*/ false, /*only_if_absolute*/ false);
}

inline std::string
redirect_path_target (std::string const& pathname)
{
    return redirect_path_full (pathname, /*check_parent*/ true, /*only_if_absolute*/ false);
}

inline std::string
redirect_path_if_absolute (std::string const& pathname)
{
    return redirect_path_full (pathname, /*check_parent*/ false, /*only_if_absolute*/ true);
}

// helper class
template<typename R, template<typename...> class Params, typename... Args, std::size_t... I>
inline R call_helper(std::function<R(Args...)> const&func, Params<Args...> const&params, std::index_sequence<I...>)
{
    return func (std::get<I>(params)...);
}

template<typename R, template<typename...> class Params, typename... Args>
inline R call_with_tuple_args(std::function<R(Args...)> const&func, Params<Args...> const&params)
{
    return call_helper (func, params, std::index_sequence_for<Args...>{});
}

struct NORMAL_REDIRECT {
    static inline std::string redirect (const std::string& path) { return redirect_path (path); }
};

struct ABSOLUTE_REDIRECT {
    static inline std::string redirect (const std::string& path) { return redirect_path_if_absolute (path); }
};

struct TARGET_REDIRECT {
    static inline std::string redirect (const std::string& path) { return redirect_path_target (path); }
};

template<typename R, const char *FUNC_NAME, typename REDIRECT_PATH_TYPE, size_t PATH_IDX, typename... Ts>
inline R
redirect_n(Ts... as)
{
    std::tuple<Ts...> tpl(as...);
    const char *path = std::get<PATH_IDX>(tpl);
    static std::function<R(Ts...)> func (reinterpret_cast<R(*)(Ts...)> (dlsym (RTLD_NEXT, FUNC_NAME)));

    if (path != NULL) {
        std::string const& new_path = REDIRECT_PATH_TYPE::redirect (path);
        std::get<PATH_IDX>(tpl) = new_path.c_str ();
        R result = call_with_tuple_args (func, tpl);
        std::get<PATH_IDX>(tpl) = path;
        return result;
    }

    return func (std::forward<Ts>(as)...);
}

template<typename R, const char *FUNC_NAME, typename REDIRECT_PATH_TYPE, typename REDIRECT_TARGET_TYPE, typename... Ts>
inline R
redirect_target(const char *path, const char *target, Ts... as)
{
    std::string const& new_target = REDIRECT_PATH_TYPE::redirect (target ? target : "");
    return redirect_n<R, FUNC_NAME, REDIRECT_PATH_TYPE, 0, const char*, const char*, Ts...> (path, new_target.c_str ());
}

struct va_separator {};
template<typename R, const char *FUNC_NAME, typename REDIRECT_PATH_TYPE, size_t PATH_IDX, typename... Ts>
inline R
redirect_open(Ts... as, va_separator, va_list va)
{
    mode_t mode = 0;
    int flags = std::get<PATH_IDX+1>(std::tuple<Ts...>(as...));

    if (flags & (O_CREAT|O_TMPFILE)) {
        mode = va_arg (va, mode_t);
    }

    return redirect_n<R, FUNC_NAME, REDIRECT_PATH_TYPE, PATH_IDX, Ts..., mode_t>(as..., mode);
}

} // unnamed namespace

extern "C"
{
#define ARG(A) , A
#define REDIRECT_NAME(NAME) _ ## NAME ## _preload
#define DECLARE_REDIRECT(NAME) \
constexpr const char REDIRECT_NAME(NAME)[] = #NAME;

#define REDIRECT_1(RET, NAME, REDIR_TYPE, SIG, ARGS) \
DECLARE_REDIRECT(NAME) \
RET NAME (const char *path SIG) { return redirect_n<RET, REDIRECT_NAME(NAME), REDIR_TYPE, 0>(path ARGS); }

#define REDIRECT_2(RET, NAME, REDIR_TYPE, T1, SIG, ARGS) \
DECLARE_REDIRECT(NAME) \
RET NAME (T1 a1, const char *path SIG) { return redirect_n<RET, REDIRECT_NAME(NAME), REDIR_TYPE, 1>(a1, path ARGS); }

#define REDIRECT_3(RET, NAME, REDIR_TYPE, T1, T2, SIG, ARGS) \
DECLARE_REDIRECT(NAME) \
RET NAME (T1 a1, T2 a2, const char *path SIG) { return redirect_n<RET, REDIRECT_NAME(NAME), REDIR_TYPE, 2>(a1, a2, path ARGS); }

#define REDIRECT_1_1(RET, NAME) \
REDIRECT_1(RET, NAME, NORMAL_REDIRECT, ,)

#define REDIRECT_1_2(RET, NAME, T2) \
REDIRECT_1(RET, NAME, NORMAL_REDIRECT, ARG(T2 a2), ARG(a2))

#define REDIRECT_1_2_AT(RET, NAME, T2) \
REDIRECT_1(RET, NAME, ABSOLUTE_REDIRECT, ARG(T2 a2), ARG(a2))

#define REDIRECT_1_3(RET, NAME, T2, T3) \
REDIRECT_1(RET, NAME, NORMAL_REDIRECT, ARG(T2 a2) ARG(T3 a3), ARG(a2) ARG(a3))

#define REDIRECT_1_4(RET, NAME, T2, T3, T4) \
REDIRECT_1(RET, NAME, NORMAL_REDIRECT, ARG(T2 a2) ARG(T3 a3) ARG(T4 a4), ARG(a2) ARG(a3) ARG(a4))

#define REDIRECT_2_2(RET, NAME, T1) \
REDIRECT_2(RET, NAME, NORMAL_REDIRECT, T1, ,)

#define REDIRECT_2_3(RET, NAME, T1, T3) \
REDIRECT_2(RET, NAME, NORMAL_REDIRECT, T1, ARG(T3 a3), ARG(a3))

#define REDIRECT_2_3_AT(RET, NAME, T1, T3) \
REDIRECT_2(RET, NAME, ABSOLUTE_REDIRECT, T1, ARG(T3 a3), ARG(a3))

#define REDIRECT_2_4_AT(RET, NAME, T1, T3, T4) \
REDIRECT_2(RET, NAME, ABSOLUTE_REDIRECT, T1, ARG(T3 a3) ARG(T4 a4), ARG(a3) ARG(a4))

#define REDIRECT_2_5_AT(RET, NAME, T1, T3, T4, T5) \
REDIRECT_2(RET, NAME, ABSOLUTE_REDIRECT, T1, ARG(T3 a3) ARG(T4 a4) ARG(T5 a5), ARG(a3) ARG(a4) ARG(a5))

#define REDIRECT_3_5(RET, NAME, T1, T2, T4, T5) \
REDIRECT_3(RET, NAME, NORMAL_REDIRECT, T1, T2, ARG(T4 a4) ARG(T5 a5), ARG(a4) ARG(a5))

#define REDIRECT_TARGET(RET, NAME) \
DECLARE_REDIRECT(NAME) \
RET NAME (const char *path, const char *target) { return redirect_target<RET, REDIRECT_NAME(NAME), NORMAL_REDIRECT, TARGET_REDIRECT>(path, target); }

#define REDIRECT_OPEN(NAME) \
DECLARE_REDIRECT(NAME) \
int NAME (const char *path, int flags, ...) { va_list va; va_start(va, flags); int ret = redirect_open<int, REDIRECT_NAME(NAME), NORMAL_REDIRECT, 0, const char *, int>(path, flags, va_separator(), va); va_end(va); return ret; }

#define REDIRECT_OPEN_AT(NAME) \
DECLARE_REDIRECT(NAME) \
int NAME (int dirfp, const char *path, int flags, ...) { va_list va; va_start(va, flags); int ret = redirect_open<int, REDIRECT_NAME(NAME), ABSOLUTE_REDIRECT, 1, int, const char *, int>(dirfp, path, flags, va_separator(), va); va_end(va); return ret; }

REDIRECT_1_2(FILE *, fopen, const char *)
REDIRECT_1_1(int, unlink)
REDIRECT_2_3_AT(int, unlinkat, int, int)
REDIRECT_1_2(int, access, int)
REDIRECT_1_2(int, eaccess, int)
REDIRECT_1_2(int, euidaccess, int)
REDIRECT_2_4_AT(int, faccessat, int, int, int)
REDIRECT_1_2(int, stat, struct stat *)
REDIRECT_1_2(int, stat64, struct stat64 *)
REDIRECT_1_2(int, lstat, struct stat *)
REDIRECT_1_2(int, lstat64, struct stat64 *)
REDIRECT_1_2(int, creat, mode_t)
REDIRECT_1_2(int, creat64, mode_t)
REDIRECT_1_2(int, truncate, off_t)
REDIRECT_2_2(char *, bindtextdomain, const char *)
REDIRECT_2_3(int, xstat, int, struct stat *)
REDIRECT_2_3(int, __xstat, int, struct stat *)
REDIRECT_2_3(int, __xstat64, int, struct stat64 *)
REDIRECT_2_3(int, __lxstat, int, struct stat *)
REDIRECT_2_3(int, __lxstat64, int, struct stat64 *)
REDIRECT_3_5(int, __fxstatat, int, int, struct stat *, int)
REDIRECT_3_5(int, __fxstatat64, int, int, struct stat64 *, int)
REDIRECT_1_2(int, statfs, struct statfs *)
REDIRECT_1_2(int, statfs64, struct statfs64 *)
REDIRECT_1_2(int, statvfs, struct statvfs *)
REDIRECT_1_2(int, statvfs64, struct statvfs64 *)
REDIRECT_1_2(long, pathconf, int)
REDIRECT_1_1(DIR *, opendir)
REDIRECT_1_2(int, mkdir, mode_t)
REDIRECT_1_1(int, rmdir)
REDIRECT_1_3(int, chown, uid_t, gid_t)
REDIRECT_1_3(int, lchown, uid_t, gid_t)
REDIRECT_1_2(int, chmod, mode_t)
REDIRECT_1_2(int, lchmod, mode_t)
REDIRECT_1_1(int, chdir)
REDIRECT_1_3(ssize_t, readlink, char *, size_t)
REDIRECT_1_2(char *, realpath, char *)
REDIRECT_TARGET(int, link)
REDIRECT_TARGET(int, rename)
REDIRECT_OPEN(open)
REDIRECT_OPEN(open64)
REDIRECT_OPEN_AT(openat)
REDIRECT_OPEN_AT(openat64)
REDIRECT_2_3(int, inotify_add_watch, int, uint32_t)
REDIRECT_1_4(int, scandir, struct dirent ***, filter_function_t<struct dirent>, compar_function_t<struct dirent>);
REDIRECT_1_4(int, scandir64, struct dirent64 ***, filter_function_t<struct dirent64>, compar_function_t<struct dirent64>);
REDIRECT_2_5_AT(int, scandirat, int, struct dirent ***, filter_function_t<struct dirent>, compar_function_t<struct dirent>);
REDIRECT_2_5_AT(int, scandirat64, int, struct dirent64 ***, filter_function_t<struct dirent64>, compar_function_t<struct dirent64>);

// non-absolute library paths aren't simply relative paths, they need
// a whole lookup algorithm
REDIRECT_1_2_AT(void *, dlopen, int);
}

static int
socket_action (socket_action_t action, int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    const struct sockaddr_un *un_addr = (const struct sockaddr_un *)addr;

    if (addr->sa_family != AF_UNIX) {
        // Non-unix sockets
        return action (sockfd, addr, addrlen);
    }

    if (!un_addr->sun_path || un_addr->sun_path[0] == '\0') {
        // Abstract sockets
        return action (sockfd, addr, addrlen);
    }

    int result = 0;
    std::string const& new_path = redirect_path (un_addr->sun_path);

    if (new_path.compare (0, PATH_MAX, un_addr->sun_path) == 0) {
        result = action (sockfd, addr, addrlen);
    } else {
        struct sockaddr_un new_addr = {0};
        strncpy (new_addr.sun_path, new_path.c_str (), new_path.size ());
        new_addr.sun_path[new_path.size ()] = '\0';
        result = action (sockfd, (const struct sockaddr *) &new_addr, sizeof (new_addr));
    }

    return result;
}

extern "C" int
bind (int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    static socket_action_t _bind =
        (decltype(_bind)) dlsym (RTLD_NEXT, "bind");

    return socket_action (_bind, sockfd, addr, addrlen);
}

extern "C" int
connect (int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    static socket_action_t _connect =
        (decltype(_connect)) dlsym (RTLD_NEXT, "connect");

    return socket_action (_connect, sockfd, addr, addrlen);
}

inline bool
erase_substr(std::string& str, std::string const& substr)
{
	size_t pos = str.find(substr);

	if (pos != std::string::npos)
	{
		str.erase(pos, substr.length());
		return true;
	}

	return false;
}

extern "C" int
mkstemp(char *path)
{
	using mkstemp_t = int (*) (char *);

	static mkstemp_t _mkstemp =
		(decltype(_mkstemp)) dlsym (RTLD_NEXT, "mkstemp");

	std::string new_path = redirect_path (path);

	char c_str[new_path.size() + 1];
	new_path.copy(c_str, new_path.size() + 1);
	c_str[new_path.size()] = '\0';

	int result = _mkstemp(c_str);

	log_msg(LOG_1, "c_str: %s, new_path: %s\n", c_str, new_path.c_str());
	if (new_path.compare(c_str)) {
		new_path = c_str;

		// remove possible patterns of redirection from the filename
		do {
			if (erase_substr(new_path, std::string("snap." + saved_snap_name + ".")))
				break;
			if (erase_substr(new_path, "snap." + saved_snap_name + "/"))
				break;
			if (erase_substr(new_path, saved_snapcraft_preload))
				break;
		} while (false);
		log_msg(LOG_3, "path: %s, new_path: %s\n", path, new_path.c_str());

		// path and new_path should have a same length
		if (strlen(path) >= new_path.length())
			strcpy(path, new_path.c_str());
		else
			log_msg(LOG_0, "the size of the original path is too small\n");
	}

    return result;
}

namespace
{
void
ensure_in_ld_preload (std::string& ld_preload, const std::string& to_be_added)
{
    if (!ld_preload.empty ()) {
        bool found = false;

        std::string p;
        std::istringstream ss (ld_preload.substr (LD_PRELOAD.size () + 1, std::string::npos));
        while (std::getline (ss, p, ':') && !found) {
            if (p == to_be_added) {
                found = true;
            }
        }

        if (!found) {
            ld_preload += ':' + to_be_added;
        }
    } else {
        ld_preload = LD_PRELOAD + '=' + to_be_added;
    }
}

std::vector<std::string>
execve_copy_envp (char *const envp[])
{
    std::string ld_preload;
    std::vector<std::string> new_envp;

    for (unsigned i = 0; envp && envp[i]; ++i) {
        std::string env(envp[i]);
        new_envp.push_back (env);

        if (str_starts_with (env, LD_PRELOAD + '=')) {
            ld_preload = env; // point at last defined LD_PRELOAD index
        }
    }

    for (const std::string& saved_preload : saved_ld_preloads)
        ensure_in_ld_preload (ld_preload, saved_preload);

    if (!saved_ld_preloads.empty ())
        new_envp.push_back (ld_preload);

    if (!saved_snapcraft_preload.empty ()) {
        auto snapcraft_preload = SNAPCRAFT_PRELOAD + '=' + saved_snapcraft_preload;
        new_envp.push_back (snapcraft_preload);
    }

    return new_envp;
}

struct c_vector_holder
{
    c_vector_holder(const std::vector<std::string>& str_vector) {
        for (auto const& str : str_vector) {
            holder_.push_back (str.c_str ());
        }
        holder_.push_back (nullptr);
    }

    operator const char **() { return holder_.data (); }
    operator char* const*() { return (char* const*) holder_.data (); }

    private:
    std::vector<const char*> holder_;
};

int
execve32_wrapper (execve_t _execve, const std::string& path, char *const argv[], char *const envp[])
{
    std::string const& custom_loader = redirect_path (LD_LINUX);
    if (custom_loader == LD_LINUX) {
        return 0;
    }

    std::vector<std::string> new_argv;
    new_argv.push_back (path);

    // envp is already adjusted for our needs.  But we need to shift argv
    for (unsigned i = 0; argv && argv[i]; ++i) {
        new_argv.push_back (argv[i]);
    }

    // Now actually run execve with our loader and adjusted argv
    return _execve (custom_loader.c_str (), c_vector_holder (new_argv), envp);
}

int
execve_wrapper (const char *func, const char *path, char *const argv[], char *const envp[])
{
    int i, result;

    static execve_t _execve = (decltype(_execve)) dlsym (RTLD_NEXT, func);

    if (path == NULL) {
        return _execve (path, argv, envp);
    }

    std::string const& new_path = redirect_path (path);

    // Make sure we inject our original preload values, can't trust this
    // program to pass them along in envp for us.
    auto env_copy = execve_copy_envp (envp);
    c_vector_holder new_envp (env_copy);
    result = _execve (new_path.c_str (), argv, new_envp);

    if (result == -1 && errno == ENOENT) {
        // OK, get prepared for gross hacks here.  In order to run 32-bit ELF
        // executables -- which will hardcode /lib/ld-linux.so.2 as their ld.so
        // loader, we must redirect that check to our own version of ld-linux.so.2.
        // But that lookup is done behind the scenes by execve, so we can't
        // intercept it like normal.  Instead, we'll prefix the command by the
        // ld.so loader which will only work if the architecture matches.  So if
        // we failed to run it normally above because the loader couldn't find
        // something, try with our own 32-bit loader.
        if (_access (new_path.c_str (), F_OK) == 0) {
            // Only actually try this if the path actually did exist.  That
            // means the ENOENT must have been a missing linked library or the
            // wrong ld.so loader.  Lets assume the latter and try to run as
            // a 32-bit executable.
            result = execve32_wrapper (_execve, new_path, argv, new_envp);
        }
    }

    return result;
}

} // anonymous namepsace

extern "C" int
execv (const char *path, char *const argv[])
{
    return execve (path, argv, environ);
}

extern "C" int
execve (const char *path, char *const argv[], char *const envp[])
{
    return execve_wrapper ("execve", path, argv, envp);
}

extern "C" int
__execve (const char *path, char *const argv[], char *const envp[])
{
    return execve_wrapper ("__execve", path, argv, envp);
}
