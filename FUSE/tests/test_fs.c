#define main userfs_main
#include UFS_SRC
#undef main

#include <sys/wait.h>
#include <assert.h>

//stand in for libfuse so we don't need to link it
int fuse_main_real(int argc, char* argv[], const struct fuse_operations* op,
	size_t op_size, void* user_data) {
	(void)argc; (void)argv; (void)op; (void)op_size; (void)user_data;
	return 0;
}

static char img[PATH_MAX];
static int current_failed;

#define EXPECT(cond, ...) do { if(!(cond)){ \
	printf("    expectation failed (line %d): %s\n      ", __LINE__, #cond); \
	printf(__VA_ARGS__); printf("\n"); current_failed = 1; } } while(0)

/* ---------- helpers ---------- */
static void fresh_fs(const char* name) {
	snprintf(img, sizeof img, "/tmp/userfs_test_%s_%d.img", name, getpid());
	unlink(img);
	snprintf(diskfile_path, sizeof diskfile_path, "%s", img);
	userfs_init(NULL);
}

static void teardown(void) {
	userfs_destroy(NULL);
	unlink(img);
}

static int count_bits(int bitmap_blk, int nbits) {
	unsigned char buf[BLOCK_SIZE];
	bio_read(bitmap_blk, buf);
	int n = 0;
	for (int i = 0; i < nbits; i++) n += get_bitmap(buf, i);
	return n;
}
static int used_inodes(void) { bio_read(0, sb); return count_bits(sb->i_bitmap_blk, sb->max_inum); }
static int used_blocks(void) { bio_read(0, sb); return count_bits(sb->d_bitmap_blk, sb->max_dnum); }

static int do_create(const char* p) {
	struct fuse_file_info fi; memset(&fi, 0, sizeof fi);
	return userfs_create(p, 0644, &fi);
}
static int do_write(const char* p, const void* buf, size_t n, off_t off) {
	struct fuse_file_info fi; memset(&fi, 0, sizeof fi);
	if (userfs_open(p, &fi) < 0) return -ENOENT;
	return userfs_write(p, buf, n, off, &fi);
}
static int do_read(const char* p, void* buf, size_t n, off_t off) {
	struct fuse_file_info fi; memset(&fi, 0, sizeof fi);
	if (userfs_open(p, &fi) < 0) return -ENOENT;
	return userfs_read(p, buf, n, off, &fi);
}
static long file_size(const char* p) {
	struct stat st;
	if (userfs_getattr(p, &st) < 0) return -1;
	return st.st_size;
}

struct listing { char names[1200][256]; int n; int this_call; int per_call; off_t last_off; };

static int filler_cb(void* buf, const char* name, const struct stat* st, off_t off) {
	(void)st;
	struct listing* l = buf;
	if (l->per_call && l->this_call >= l->per_call) return 1; //buffer is full
	if (l->n >= 1200) return 1;
	snprintf(l->names[l->n++], 256, "%s", name);
	l->this_call++;
	l->last_off = off;
	return 0;
}

// lists a directory, resuming with the offset the way the kernel would.
static void list_dir(const char* p, struct listing* l, int per_call) {
	memset(l, 0, sizeof * l);
	l->per_call = per_call;
	struct fuse_file_info fi; memset(&fi, 0, sizeof fi);
	userfs_opendir(p, &fi);
	off_t off = 0;
	for (int iter = 0; iter < 300; iter++) {
		int before = l->n;
		l->this_call = 0;
		userfs_readdir(p, l, filler_cb, off, &fi);
		if (per_call == 0 || l->n == before) break; //one call is the whole listing
		off = l->last_off;
	}
}

static int has_name(struct listing* l, const char* name) {
	for (int i = 0; i < l->n; i++) if (strcmp(l->names[i], name) == 0) return 1;
	return 0;
}

/* ---------- tests ---------- */
static void t_root_and_basics(void) {
	fresh_fs("basics");
	struct stat st;
	EXPECT(userfs_getattr("/", &st) == 0 && S_ISDIR(st.st_mode), "root is a directory");
	EXPECT(userfs_mkdir("/a", 0755) == 0, "mkdir /a");
	EXPECT(do_create("/a/f") == 0, "create /a/f");
	EXPECT(userfs_getattr("/a/f", &st) == 0 && S_ISREG(st.st_mode), "/a/f is a regular file");
	EXPECT(userfs_getattr("/nope", &st) == -ENOENT, "missing path gives ENOENT");
	struct listing l; list_dir("/a", &l, 0);
	EXPECT(l.n == 3 && has_name(&l, ".") && has_name(&l, "..") && has_name(&l, "f"),
		"listing of /a has ., .., f (got %d entries)", l.n);
	teardown();
}

static void t_no_phantom_entries(void) {
	fresh_fs("phantom");
	userfs_mkdir("/a", 0755);

	struct inode root, a; unsigned char blk[BLOCK_SIZE];
	readi(0, &root);
	bio_read(root.direct_ptr[0], blk);
	int dirty = 0;
	for (int i = 2 * sizeof(struct dirent); i < BLOCK_SIZE; i++) if (blk[i]) { dirty = 1; break; }
	EXPECT(!dirty, "root dir block has non-zero bytes after its '.' and 'a' entries");

	get_node_by_path("/a", 0, &a);
	bio_read(a.direct_ptr[0], blk);
	dirty = 0;
	for (int i = 2 * sizeof(struct dirent); i < BLOCK_SIZE; i++) if (blk[i]) { dirty = 1; break; }
	EXPECT(!dirty, "/a dir block has non-zero bytes after its '.' and '..' entries");

	struct listing l; list_dir("/", &l, 0);
	EXPECT(l.n == 2, "root should list exactly '.' and 'a', got %d entries", l.n);
	list_dir("/a", &l, 0);
	EXPECT(l.n == 2, "/a should list exactly '.' and '..', got %d entries", l.n);
	teardown();
}

static void t_file_size_semantics(void) {
	fresh_fs("size");
	do_create("/f");
	char buf[4096 * 3], out[4096 * 3];
	memset(buf, 'x', sizeof buf);

	EXPECT(do_write("/f", buf, 100, 0) == 100, "write 100 bytes");
	EXPECT(file_size("/f") == 100, "size after 100-byte write should be 100, got %ld", file_size("/f"));
	EXPECT(do_read("/f", out, 4096, 0) == 100, "read of 4096 should stop at EOF (100)");

	EXPECT(do_write("/f", buf, sizeof buf, 0) == (int)sizeof buf, "write 3 blocks");
	EXPECT(file_size("/f") == 3 * 4096, "size should be 12288, got %ld", file_size("/f"));

	do_write("/f", buf, 10, 0); //rewrites the start of the file
	EXPECT(file_size("/f") == 3 * 4096, "rewriting block 0 must not shrink the file, got %ld", file_size("/f"));
	EXPECT(do_read("/f", out, sizeof out, 0) == (int)sizeof out, "read back full 3 blocks");
	EXPECT(do_read("/f", out, 50, 3 * 4096 + 5) == 0, "read past EOF returns 0");

	struct stat st; userfs_getattr("/f", &st);
	teardown();
}

static void t_hole_read(void) {
	fresh_fs("hole");
	do_create("/f");
	char data[10]; memset(data, 'z', sizeof data);
	EXPECT(do_write("/f", data, 10, 3 * 4096) == 10, "write beyond EOF");
	EXPECT(file_size("/f") == 3 * 4096 + 10, "size should be 12298, got %ld", file_size("/f"));
	char out[4096]; memset(out, 0x55, sizeof out);
	int r = do_read("/f", out, 4096, 0);
	int zeros = 1; for (int i = 0; i < 4096; i++) if (out[i]) zeros = 0;
	EXPECT(r == 4096 && zeros, "reading an unwritten hole should give 4096 zero bytes (got %d)", r);
	teardown();
}

static void t_write_without_open(void) {
	fresh_fs("fh0");
	do_create("/f");
	struct fuse_file_info fi; memset(&fi, 0, sizeof fi); //forces the path lookup branch 
	char buf[64]; memset(buf, 'q', sizeof buf);
	EXPECT(userfs_write("/f", buf, sizeof buf, 0, &fi) == (int)sizeof buf, "write via path lookup");
	struct stat st;
	EXPECT(userfs_getattr("/", &st) == 0 && S_ISDIR(st.st_mode), "root inode must still be a directory");
	struct listing l; list_dir("/", &l, 0);
	EXPECT(has_name(&l, "f"), "root directory must still contain f");
	EXPECT(file_size("/f") == 64, "file size should be 64, got %ld", file_size("/f"));
	teardown();
}

static void t_parent_must_be_dir(void) {
	fresh_fs("parent");
	do_create("/f");
	int blocks_before = used_blocks();
	EXPECT(do_create("/f/x") == -ENOTDIR, "create under a regular file should be ENOTDIR");
	EXPECT(userfs_mkdir("/f/d", 0755) == -ENOTDIR, "mkdir under a regular file should be ENOTDIR");
	EXPECT(file_size("/f") == 0, "the file must be untouched");
	EXPECT(used_blocks() == blocks_before, "no blocks should have been consumed");
	teardown();
}

static void t_directory_capacity(void) {
	fresh_fs("cap");
	userfs_mkdir("/d", 0755);
	char path[64];
	int made = 0, rc = 0;
	for (int i = 0; i < 400; i++) {
		snprintf(path, sizeof path, "/d/f%03d", i);
		rc = do_create(path);
		if (rc != 0) break;
		made++;
	}

	//16 direct blocks * 19 dirents = 304 slots, 2 taken by . and ..
	EXPECT(made == 302, "should fit exactly 302 files, made %d", made);
	EXPECT(rc == -ENOSPC, "the next create should fail with ENOSPC, got %d", rc);
	int missing = 0;
	struct stat st;
	for (int i = 0; i < made; i++) {
		snprintf(path, sizeof path, "/d/f%03d", i);
		if (userfs_getattr(path, &st) != 0) missing++;
	}
	EXPECT(missing == 0, "%d files that create() reported success for cannot be found", missing);
	teardown();
}

static void t_no_inode_leak_on_failure(void) {
	fresh_fs("inoleak");
	userfs_mkdir("/a", 0755);
	do_create("/f");
	int ino0 = used_inodes(), blk0 = used_blocks();
	EXPECT(userfs_mkdir("/a", 0755) == -EEXIST, "mkdir of existing dir gives EEXIST");
	EXPECT(do_create("/f") == -EEXIST, "create of existing file gives EEXIST");
	EXPECT(used_inodes() == ino0, "failed mkdir/create leaked %d inode(s)", used_inodes() - ino0);
	EXPECT(used_blocks() == blk0, "failed mkdir/create leaked %d block(s)", used_blocks() - blk0);
	teardown();
}

static void t_name_length(void) {
	fresh_fs("name");
	char path[400];
	char name[300];

	memset(name, 'a', 207); name[207] = 0;
	snprintf(path, sizeof path, "/%s", name);
	EXPECT(do_create(path) == 0, "207-char name is the longest that fits");
	struct stat st;
	EXPECT(userfs_getattr(path, &st) == 0, "and it can be found again");

	memset(name, 'b', 250); name[250] = 0;
	snprintf(path, sizeof path, "/%s", name);
	EXPECT(do_create(path) == -ENAMETOOLONG, "250-char name must be rejected with ENAMETOOLONG");
	memset(name, 'c', 208); name[208] = 0;
	snprintf(path, sizeof path, "/%s", name);
	EXPECT(userfs_mkdir(path, 0755) == -ENAMETOOLONG, "208-char name must be rejected");
	teardown();
}

static void t_readdir_resume(void) {
	fresh_fs("resume");
	userfs_mkdir("/d", 0755);
	char path[64];
	for (int i = 0; i < 50; i++) { snprintf(path, sizeof path, "/d/f%02d", i); do_create(path); }
	struct listing l; list_dir("/d", &l, 7); /* buffer "fills" after 7 entries each call */
	EXPECT(l.n == 52, "paged listing should return 52 entries once each, got %d", l.n);
	int dup = 0;
	for (int i = 0; i < l.n; i++) for (int j = i + 1; j < l.n; j++) if (!strcmp(l.names[i], l.names[j])) dup++;
	EXPECT(dup == 0, "paged listing returned %d duplicate names", dup);
	teardown();
}

static void t_file_limits(void) {
	fresh_fs("limits");
	do_create("/f");
	static char big[16 * 4096]; memset(big, 'k', sizeof big);
	EXPECT(do_write("/f", big, sizeof big, 0) == (int)sizeof big, "64KB fits in 16 direct blocks");
	EXPECT(file_size("/f") == 16 * 4096, "size is 65536");
	EXPECT(do_write("/f", big, 4096, 16 * 4096) == -EFBIG, "write past the 64KB limit gives EFBIG");
	EXPECT(file_size("/f") == 16 * 4096, "failed write must not change size");
	teardown();
}

static void t_leaks(void) {
	fresh_fs("leaks");
	userfs_mkdir("/d", 0755);
	do_create("/d/f");
	char buf[4096 * 2]; memset(buf, 1, sizeof buf);
	do_write("/d/f", buf, sizeof buf, 0);
	struct listing l;
	for (int i = 0; i < 300; i++) {
		do_read("/d/f", buf, sizeof buf, 0);
		list_dir("/d", &l, 0);
	}
	char p[64];
	for (int i = 0; i < 100; i++) { snprintf(p, sizeof p, "/d/g%d", i); do_create(p); }
	teardown();
}

static void t_benchmark_shape(void) {
	fresh_fs("bench");
	do_create("/file");
	char buf[4096], out[4096];
	for (int i = 0; i < 16; i++) { memset(buf, 'A' + i, sizeof buf); do_write("/file", buf, sizeof buf, (off_t)i * 4096); }
	int ok = 1;
	for (int i = 0; i < 16; i++) {
		memset(buf, 'A' + i, sizeof buf);
		if (do_read("/file", out, sizeof out, (off_t)i * 4096) != 4096 || memcmp(buf, out, sizeof buf)) ok = 0;
	}
	EXPECT(ok, "16 sequential block writes read back identically");
	EXPECT(file_size("/file") == 16 * 4096, "file is 65536 bytes");
	EXPECT(userfs_mkdir("/dir", 0755) == 0, "mkdir /dir");
	char p[64]; int fails = 0;
	for (int i = 0; i < 100; i++) { snprintf(p, sizeof p, "/dir/sub%d", i); if (userfs_mkdir(p, 0755) != 0) fails++; }
	EXPECT(fails == 0, "%d of 100 subdirectory creations failed", fails);
	struct listing l; list_dir("/dir", &l, 0);
	EXPECT(l.n == 102, "/dir should list 102 entries, got %d", l.n);
	EXPECT(used_inodes() == 103, "inodes in use should be 103 (root, file, dir, 100 subdirs), got %d", used_inodes());
	EXPECT(used_blocks() == 123, "data blocks in use should be 123 (matches the report), got %d", used_blocks());
	struct stat st; userfs_getattr("/dir", &st);
	EXPECT(st.st_nlink == 102, "/dir link count should be 2 + 100 subdirs = 102, got %d", (int)st.st_nlink);
	teardown();
}

static void t_persistence(void) {
	snprintf(img, sizeof img, "/tmp/userfs_test_persist_%d.img", getpid());
	unlink(img);
	snprintf(diskfile_path, sizeof diskfile_path, "%s", img);

	pid_t pid = fork();
	if (pid == 0) { //builds a tree and unmounts
		userfs_init(NULL);
		userfs_mkdir("/keep", 0755);
		do_create("/keep/data");
		char buf[5000]; memset(buf, 'P', sizeof buf);
		do_write("/keep/data", buf, sizeof buf, 0);
		userfs_destroy(NULL);
		exit(0);
	}
	int status; waitpid(pid, &status, 0);
	EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "phase 1 ran cleanly");

	//remounts the same image
	userfs_init(NULL);
	EXPECT(file_size("/keep/data") == 5000, "file size survives remount, got %ld", file_size("/keep/data"));
	char out[5000];
	EXPECT(do_read("/keep/data", out, sizeof out, 0) == 5000 && out[0] == 'P' && out[4999] == 'P', "data survives remount");
	userfs_destroy(NULL);
	unlink(img);
}

typedef struct { const char* name; void (*fn)(void); } test_t;
static test_t tests[] = {
	{"root and basics",                    t_root_and_basics},
	{"no phantom entries (zeroed blocks)", t_no_phantom_entries},
	{"file size semantics + EOF reads",    t_file_size_semantics},
	{"reading holes",                      t_hole_read},
	{"write when fh == 0 keeps root safe", t_write_without_open},
	{"parent must be a directory",         t_parent_must_be_dir},
	{"directory capacity",                 t_directory_capacity},
	{"no inode/block leak on failure",     t_no_inode_leak_on_failure},
	{"name length limits",                 t_name_length},
	{"readdir resumes with offsets",       t_readdir_resume},
	{"file size limit",                    t_file_limits},
	{"memory leaks",                       t_leaks},
	{"benchmark shape matches report",     t_benchmark_shape},
	{"persistence across remount",         t_persistence},
};

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	int total = sizeof tests / sizeof tests[0], passed = 0;
	for (int i = 0; i < total; i++) {
		printf("[ RUN  ] %s\n", tests[i].name);
		pid_t pid = fork();
		if (pid == 0) {
			current_failed = 0;
			tests[i].fn();
			exit(current_failed ? 1 : 0); //ensures that LeakSanitizer runs
		}
		int status; waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			printf("[  OK  ] %s\n", tests[i].name); passed++;
		}
		else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
			printf("[ FAIL ] %s\n", tests[i].name);
		}
		else if (WIFEXITED(status)) {
			printf("[ FAIL ] %s (exit %d: sanitizer error or leak)\n", tests[i].name, WEXITSTATUS(status));
		}
		else {
			printf("[ FAIL ] %s (killed by signal %d)\n", tests[i].name, WTERMSIG(status));
		}
	}
	printf("\n%d/%d tests passed\n", passed, total);
	return passed == total ? 0 : 1;
}
