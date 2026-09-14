#include <stdio.h>

#include "common/common.h"
#include "common/global.h"
#include "common/msg.h"
#include "config.h"
#include "options/options.h"
#include "options/path.h"
#include "osdep/io.h"
#include "test_utils.h"

static void test_join(char *file, int line, char *a, char *b, char *c)
{
    char *res = mp_path_join(NULL, a, b);
    assert_string_equal_impl(file, line, res, c);
    talloc_free(res);
}


static void test_normalize(char *file, int line, char *expected, char *path)
{
    char *normalized = mp_normalize_path(NULL, path);
    assert_string_equal_impl(file, line, normalized, expected);
    talloc_free(normalized);
}

static void test_dirname(char *file, int line, char *path, char *expected)
{
    char *res = bstrto0(NULL, mp_dirname(path));
    assert_string_equal_impl(file, line, res, expected);
    talloc_free(res);
}

static void create_path_fixture(const char *root)
{
    FILE *f = test_open_out(root, "mpv.conf");
    fclose(f);
    f = test_open_out(root, "input.conf");
    fclose(f);
    f = test_open_out(root, "subtitle.srt");
    fclose(f);

    char *scripts = mp_path_join(NULL, root, "scripts");
    mp_mkdirp(scripts);
    assert_true(mp_path_isdir(scripts));
    talloc_free(scripts);
}

static struct mpv_global *make_global(void *talloc_ctx, bool load_config,
                                      const char *configdir)
{
    struct mpv_global *global = talloc_zero(talloc_ctx, struct mpv_global);
    struct MPOpts opts = {
        .load_config = load_config,
        .force_configdir = (char *)configdir,
    };
    mp_init_paths(global, &opts);
    return global;
}

static void assert_user_path(struct mpv_global *global, const char *path,
                             const char *expected)
{
    char *actual = mp_get_user_path(NULL, global, path);
    assert_string_equal(actual, expected);
    talloc_free(actual);
}

static void assert_config_path(struct mpv_global *global, const char *name,
                               const char *root)
{
    char **paths = mp_find_all_config_files(NULL, global, name);
    char *expected = mp_path_join(NULL, root, name);
    assert_true(paths && paths[0] && !paths[1]);
    assert_string_equal(paths[0], expected);
    talloc_free(expected);
    talloc_free(paths);
}

static void assert_no_config_path(struct mpv_global *global, const char *name)
{
    char **paths = mp_find_all_config_files(NULL, global, name);
    assert_true(paths && !paths[0]);
    talloc_free(paths);
}

static void test_config_paths(const char *outdir, const char *environment_root)
{
    void *ctx = talloc_new(NULL);
    char *root_a = mp_path_join(ctx, outdir, "config-a");
    char *root_b = mp_path_join(ctx, outdir, "config-b");
    const char *mpv_home = getenv("MPV_HOME");

    assert_true(mpv_home);
    assert_string_equal(mpv_home, environment_root);
    create_path_fixture(environment_root);
    create_path_fixture(root_a);
    create_path_fixture(root_b);

    struct mpv_global *environment = make_global(ctx, true, NULL);
    char *expected = mp_path_join(ctx, environment_root, "subtitle.srt");
    assert_user_path(environment, "~~/subtitle.srt", expected);
    assert_user_path(environment, "~~home/subtitle.srt", expected);
    assert_config_path(environment, "mpv.conf", environment_root);

    struct mpv_global *enabled = make_global(ctx, true, root_a);
    expected = mp_path_join(ctx, root_a, "subtitle.srt");
    assert_user_path(enabled, "~~/subtitle.srt", expected);
    assert_user_path(enabled, "~~home/subtitle.srt", expected);
    assert_config_path(enabled, "mpv.conf", root_a);
    assert_config_path(enabled, "input.conf", root_a);
    assert_config_path(enabled, "scripts", root_a);

    struct mpv_global *plain_disabled = make_global(ctx, false, NULL);
    assert_user_path(plain_disabled, "~~/subtitle.srt", "subtitle.srt");
    assert_user_path(plain_disabled, "~~home/subtitle.srt", "subtitle.srt");
    assert_no_config_path(plain_disabled, "mpv.conf");
    assert_no_config_path(plain_disabled, "input.conf");
    assert_no_config_path(plain_disabled, "scripts");

    struct mpv_global *disabled_a = make_global(ctx, false, root_a);
    struct mpv_global *disabled_b = make_global(ctx, false, root_b);
    assert_user_path(disabled_a, "~~/", root_a);
    assert_user_path(disabled_a, "~~home/", root_a);
    expected = mp_path_join(ctx, root_a, "subtitle.srt");
    assert_user_path(disabled_a, "~~/subtitle.srt", expected);
    assert_user_path(disabled_a, "~~home/subtitle.srt", expected);
    expected = mp_path_join(ctx, root_b, "subtitle.srt");
    assert_user_path(disabled_b, "~~/subtitle.srt", expected);
    assert_user_path(disabled_b, "~~home/subtitle.srt", expected);
    expected = mp_path_join(ctx, root_a, "mpv.conf");
    assert_user_path(disabled_a, "~~/mpv.conf", expected);
    assert_user_path(disabled_a, "~~global/subtitle.srt", "subtitle.srt");
    assert_user_path(disabled_a, "~~cache/cache.bin", "cache.bin");

    assert_no_config_path(disabled_a, "mpv.conf");
    assert_no_config_path(disabled_a, "input.conf");
    assert_no_config_path(disabled_a, "scripts");
    assert_no_config_path(disabled_b, "mpv.conf");
    assert_no_config_path(disabled_b, "input.conf");
    assert_no_config_path(disabled_b, "scripts");

    talloc_free(ctx);
}

#define TEST_JOIN(a, b, c) \
    test_join(__FILE__, __LINE__, a, b, c);

#define TEST_ABS(abs, a) \
    assert_int_equal_impl(__FILE__, __LINE__, abs, mp_path_is_absolute(bstr0(a)))

#define TEST_NORMALIZE(expected, path) \
    test_normalize(__FILE__, __LINE__, expected, path)

#define TEST_BASENAME(path, expected) \
    assert_string_equal_impl(__FILE__, __LINE__, mp_basename(path), expected)

#define TEST_DIRNAME(path, expected) \
    test_dirname(__FILE__, __LINE__, path, expected)

int main(int argc, char **argv)
{
    assert_int_equal(argc, 3);
    test_config_paths(argv[1], argv[2]);

    TEST_ABS(true, "/ab");
    TEST_ABS(false, "ab");
    TEST_JOIN("",           "",             "");
    TEST_JOIN("a",          "",             "a");
    TEST_JOIN("/a",         "",             "/a");
    TEST_JOIN("",           "b",            "b");
    TEST_JOIN("",           "/b",           "/b");
    TEST_JOIN("ab",         "cd",           "ab/cd");
    TEST_JOIN("ab/",        "cd",           "ab/cd");
    TEST_JOIN("ab/",        "/cd",          "/cd");
    // Note: we prefer "/" on win32, but tolerate "\".
#if HAVE_DOS_PATHS
    TEST_ABS(true, "\\ab");
    TEST_ABS(true, "c:\\");
    TEST_ABS(true, "c:/");
    TEST_ABS(false, "c:");
    TEST_ABS(false, "c:a");
    TEST_ABS(false, "c:a\\");
    TEST_JOIN("ab\\",       "cd",           "ab\\cd");
    TEST_JOIN("ab\\",       "\\cd",         "\\cd");
    TEST_JOIN("c:/",        "de",           "c:/de");
    TEST_JOIN("c:/a",       "de",           "c:/a/de");
    TEST_JOIN("c:\\a",      "c:\\b",        "c:\\b");
    TEST_JOIN("c:/a",       "c:/b",         "c:/b");
    // Note: drive-relative paths are not always supported "properly"
    TEST_JOIN("c:/a",       "d:b",          "c:/a/d:b");
    TEST_JOIN("c:a",        "b",            "c:a/b");
    TEST_JOIN("c:",         "b",            "c:b");
#endif

    TEST_NORMALIZE("https://foo", "https://foo");
#if !HAVE_DOS_PATHS
    TEST_NORMALIZE("/foo", "/foo");
#endif

    void *ctx = talloc_new(NULL);
    bstr dst = bstr0(mp_getcwd(ctx));
    bstr_xappend(ctx, &dst, bstr0("/foo"));
#if HAVE_DOS_PATHS
    char *p = dst.start;
    while (*p) {
        *p = *p == '/' ? '\\' : *p;
        p++;
    }
#endif
    TEST_NORMALIZE(dst.start, "foo");
    talloc_free(ctx);

#if HAVE_DOS_PATHS
    TEST_NORMALIZE("C:\\foo\\baz", "C:/foo/bar/../baz");
    TEST_NORMALIZE("C:\\", "C:/foo/../..");
    TEST_NORMALIZE("C:\\foo\\baz", "C:/foo/bar/./../baz");
    TEST_NORMALIZE("C:\\foo\\bar\\baz", "C:/foo//bar/./baz");
    TEST_NORMALIZE("C:\\foo\\bar\\baz", "C:/foo\\./bar\\/baz");
    TEST_NORMALIZE("C:\\file.mkv", "\\\\?\\C:\\folder\\..\\file.mkv");
    TEST_NORMALIZE("C:\\dir", "\\\\?\\C:\\dir\\subdir\\..\\.");
    TEST_NORMALIZE("D:\\newfile.txt", "\\\\?\\D:\\\\new\\subdir\\..\\..\\newfile.txt");
    TEST_NORMALIZE("\\\\server\\share\\path", "\\\\?\\UNC/server/share/path/.");
    TEST_NORMALIZE("C:\\", "C:/.");
    TEST_NORMALIZE("C:\\", "C:/../");
#else
    TEST_NORMALIZE("/foo/bar", "/foo//bar");
    TEST_NORMALIZE("/foo/bar", "/foo///bar");
    TEST_NORMALIZE("/foo/bar", "/foo/bar/");
    TEST_NORMALIZE("/foo/bar", "/foo/./bar");
    TEST_NORMALIZE("/usr", "/usr/bin/..");
#endif

    TEST_BASENAME("/usr/local/bin", "bin");
    TEST_BASENAME("/usr/local/", "");
    TEST_BASENAME("/usr/", "");
    TEST_BASENAME("/", "");
    TEST_BASENAME("usr/local/bin", "bin");
    TEST_BASENAME("usr/local/", "");
    TEST_BASENAME("usr/", "");
    TEST_BASENAME("usr", "usr");
    TEST_BASENAME("", "");
    TEST_BASENAME(".", ".");
    TEST_BASENAME("..", "..");
#if HAVE_DOS_PATHS
    TEST_BASENAME("C:\\Windows\\System32", "System32");
    TEST_BASENAME("C:\\Windows\\", "");
    TEST_BASENAME("C:\\", "");
    TEST_BASENAME("C:", "");
#endif

    TEST_DIRNAME("/usr/local/bin", "/usr/local/");
    TEST_DIRNAME("/usr/local/", "/usr/local/");
    TEST_DIRNAME("/usr/", "/usr/");
    TEST_DIRNAME("/", "/");
    TEST_DIRNAME("usr/local/bin", "usr/local/");
    TEST_DIRNAME("usr/local/", "usr/local/");
    TEST_DIRNAME("usr/", "usr/");
    TEST_DIRNAME("usr", ".");
    TEST_DIRNAME("", ".");
    TEST_DIRNAME(".", ".");
    TEST_DIRNAME("..", ".");
#if HAVE_DOS_PATHS
    TEST_DIRNAME("C:\\Windows\\System32", "C:\\Windows\\");
    TEST_DIRNAME("C:\\Windows\\", "C:\\Windows\\");
    TEST_DIRNAME("C:\\", "C:\\");
    TEST_DIRNAME("C:", "C:");
#endif

    return 0;
}
