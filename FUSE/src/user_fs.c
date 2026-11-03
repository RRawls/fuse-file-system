#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "user_fs.h"

char diskfile_path[PATH_MAX];

//in memory data structures
struct superblock *sb;
bitmap_t inode_map;
bitmap_t data_map;

//the capacity limits below are derived from the on-disk structs
#define NUM_DIRECT		((int)(sizeof(((struct inode *)0)->direct_ptr) / sizeof(int)))
#define NUM_INDIRECT	((int)(sizeof(((struct inode *)0)->indirect_ptr) / sizeof(int)))
#define DIRENTS_PER_BLK	((int)(BLOCK_SIZE / sizeof(struct dirent)))
#define MAX_NAME_LEN	(sizeof(((struct dirent *)0)->name) - 1) //leaves room for '\0'

//gets the available inode number from the bitmap
int get_avail_ino() {

	//reads the inode bitmap from the disk
	bio_read(0, sb);
	bio_read(sb->i_bitmap_blk, inode_map);

	//traverses inode bitmap to find an available slot
	int available = -1;
	for(int i = 0; i < sb->max_inum; i++){
		if(get_bitmap(inode_map, i) == 0){
			available = i;
			break;
		}
	}

	//updates the inode bitmap and write to the disk 
	if(available != -1){
		set_bitmap(inode_map, available);
		bio_write(sb->i_bitmap_blk, inode_map);
	}
	
	return available;
}

//gets the available data block number from the bitmap
int get_avail_blkno() {

	//reads the data block bitmap from disk
	bio_read(0, sb);
	bio_read(sb->d_bitmap_blk, data_map);

	//traverses the data block bitmap to find an available slot
	int available = -1;
	for(int i = 0; i < sb->max_dnum; i++){
		if(get_bitmap(data_map, i) == 0){
			available = i;
			break;
		}
	}

	//updates the data block bitmap and writes to the disk 
	if(available != -1){
		set_bitmap(data_map, available);
		bio_write(sb->d_bitmap_blk, data_map);
	}
	
	return available;
}

//gives back the an inode that was reserved and not used
static void release_ino(int ino) {
	bio_read(sb->i_bitmap_blk, inode_map);
	unset_bitmap(inode_map, ino);
	bio_write(sb->i_bitmap_blk, inode_map);
}

//same as above but for data blocks
static void release_blkno(int blkno) {
	bio_read(sb->d_bitmap_blk, data_map);
	unset_bitmap(data_map, blkno);
	bio_write(sb->d_bitmap_blk, data_map);
}

/* inode operations */

//reads the inode on-disk block and copies it into an inode structure
int readi(uint16_t ino, struct inode *inode) {

	//get the inode's on-disk block number
	bio_read(0, sb);
	int block_num = sb->i_start_blk + (ino / (BLOCK_SIZE / sizeof(struct inode)));

    //gets the offset of the inode in the inode on-disk block
	int offset = (ino % (BLOCK_SIZE / sizeof(struct inode))) * sizeof(struct inode);

   //reads the block from disk and copies it
	void *temp = malloc(BLOCK_SIZE);
	bio_read(block_num, temp);
	memcpy(inode, ((char*)temp + offset), sizeof(struct inode));
	free(temp);

	return 0;
}

//writes the relevant inode to the disk
int writei(uint16_t ino, struct inode *inode) {

	//get the inode's on-disk block number
	bio_read(0, sb);
	int block_num = sb->i_start_blk + (ino / (BLOCK_SIZE / sizeof(struct inode)));

	//gets the offset of the inode in the inode on-disk block
	int offset = (ino % (BLOCK_SIZE / sizeof(struct inode))) * sizeof(struct inode);

	//writes inode to disk 
	void *temp = malloc(BLOCK_SIZE);
	bio_read(block_num, temp);
	memcpy(((char*)temp + offset), inode, sizeof(struct inode));
	bio_write(block_num, temp);
	free(temp);

	return 0;
}


/* directory operations */

//fills in a directory entry (zeroes the whole struct first so no stale bytes reach the disk)
static void fill_dirent(struct dirent *d, uint16_t ino, const char *name, size_t name_len) {
	memset(d, 0, sizeof(struct dirent));
	d->ino = ino;
	d->valid = 1;
	d->len = name_len;
	memcpy(d->name, name, name_len); //name[name_len] is already '\0'
}

/*
* gets the data block of the current directory from an inode and, if the name matches, copies the 
* directory entry to the dirent structure.
*/
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {


	struct inode dir_node;
	readi(ino, &dir_node);

    //gets data block of current directory from inode
	for(int i = 0; i < NUM_DIRECT; i++){
		if(dir_node.direct_ptr[i] == -1){
			break;
		}

		//checks each directory entry and checks directory entries to find a match
		char* data = malloc(BLOCK_SIZE);
		bio_read(dir_node.direct_ptr[i], data);
		struct dirent *val;	

		for(int j = 0; j < DIRENTS_PER_BLK; j++){
			val = (struct dirent*)(data + j * sizeof(struct dirent));
			if(val->valid == 1 && val->len == name_len && strcmp(val->name, fname) == 0){
				memcpy(dirent, val, sizeof(struct dirent));
				free(data);
				return 0;
			}
		}
		free(data);
	}

	return -ENOENT; //error code for no such file/dir
}

//If fname isn't already used in other entries, adds the directory entry in dir_inode's data block and writes it to the disk.
int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	if(name_len == 0 || name_len > MAX_NAME_LEN){
		return -ENAMETOOLONG; //would not fit in dirent.name
	}

	char *data = malloc(BLOCK_SIZE);
	if(data == NULL){
		return -ENOMEM;
	}

	int free_blk = -1; //disk block holding the first free slot
	int free_off = 0;  //byte offset of that slot within the block

	/*
	* scans every entry first and rejects duplicates anywhere in the directory,
	* while remembering the first free slot so it can be reused.
	*/
	for(int i = 0; i < NUM_DIRECT; i++){
		if(dir_inode.direct_ptr[i] == -1){
			break;
		}
		
		bio_read(dir_inode.direct_ptr[i], data);

		for(int j = 0; j < DIRENTS_PER_BLK; j++){
			struct dirent *val = (struct dirent*)(data + j * sizeof(struct dirent));
			if(val->valid == 1){
				if(val->len == name_len && strcmp(val->name, fname) == 0){
					free(data);
					return -EEXIST;
				}
			}
			else if(free_blk == -1){
				free_blk = dir_inode.direct_ptr[i];
				free_off = j * sizeof(struct dirent);
			}
		}
	}

	if(free_blk != -1){
		bio_read(free_blk, data); //data holds the last block scanned, not necessarily this one
		fill_dirent((struct dirent*)(data + free_off), f_ino, fname, name_len);
		bio_write(free_blk, data);
		free(data);
		return 0;
	}
	
	/*
	* in the case that there is no room, the directory needs a new data block; we need to
	* make sure there is a direct pointer to hold it prior to allocating
	*/
	int slot = -1;
	for(int i = 0; i < NUM_DIRECT; i++){
		if(dir_inode.direct_ptr[i] == -1){
			slot = i;
			break;
		}
	}
	if(slot == -1){
		free(data);
		return -ENOSPC; //directory already uses all its direct blocks
	}

	int new_num = get_avail_blkno();
	if(new_num == -1){
		free(data);
		return -ENOSPC;
	}

	//writes the directory entry into the new block
	memset(data, 0, BLOCK_SIZE);
	fill_dirent((struct dirent*)data, f_ino, fname, name_len);
	bio_write(new_num + sb->d_start_blk, data);

	/*
	* updates the directory inode (the link count isn't changed here as gaining a
	* data block doesn't add a hard link
	*/
	dir_inode.direct_ptr[slot] = new_num + sb->d_start_blk;
	dir_inode.size += BLOCK_SIZE; //we add a whole new block
	dir_inode.vstat.st_size += BLOCK_SIZE;
	dir_inode.vstat.st_blocks += 1;

	time_t current = time(NULL);
	dir_inode.vstat.st_mtime = current;

	writei(dir_inode.ino, &dir_inode);
	free(data);

	return 0;
}


//namei operation - resolves the path name, walks through it, and finds its inode
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	char *copy = strdup(path);
	if(copy == NULL){
		return -ENOMEM;
	}

	struct inode curr;
	readi(ino, &curr);

	char *saveptr = NULL; //strtok_r: strtok keeps hidden global state
	char* token = strtok_r(copy, "/", &saveptr);
	while(token != NULL){
		if(curr.type != S_IFDIR){
			free(copy);
			return -ENOTDIR;
		}

		struct dirent val;
		if(dir_find(curr.ino, token, strlen(token), &val) < 0){
			free(copy);
			return -ENOENT;
		}

		readi(val.ino, &curr);
		token = strtok_r(NULL, "/", &saveptr);
	}

	memcpy(inode, &curr, sizeof(struct inode));
	
	free(copy);
	return 0;
}

//makes the file system
int userfs_mkfs() {

	//calls dev_init() to initialize (create) the diskfile
	dev_init(diskfile_path);

	//writes superblock information
	int inode_blocks = (MAX_INUM * sizeof(struct inode))/(BLOCK_SIZE);
	
	sb = calloc(1, BLOCK_SIZE); //zeroed: the whole block is written to the disk
	sb->magic_num = MAGIC_NUM;
	sb->max_inum = MAX_INUM;
	sb->max_dnum = (MAX_DNUM - 3 - inode_blocks);
	sb->i_bitmap_blk = 1;
	sb->d_bitmap_blk = 2;
	sb->i_start_blk = 3;
	sb->d_start_blk = 3 + inode_blocks;

	bio_write(0, sb);

	//initializes the inode bitmap
	inode_map = calloc(1, BLOCK_SIZE);

	//initializes the data block bitmap
	data_map = calloc(1, BLOCK_SIZE);

	//updates the bitmap information for root directory
	set_bitmap(inode_map, 0);
	set_bitmap(data_map, 0); 

	bio_write(sb->i_bitmap_blk, inode_map);
	bio_write(sb->d_bitmap_blk, data_map);

	//updates the inode for root directory
	struct inode rooti;
	memset(&rooti, 0, sizeof(struct inode));
	rooti.ino = 0;
	rooti.valid = 1;
	rooti.size = BLOCK_SIZE;
	rooti.type = S_IFDIR;
	rooti.link = 2;

	rooti.direct_ptr[0] = sb->d_start_blk; 
	for(int i = 1; i < NUM_DIRECT; i++){
		rooti.direct_ptr[i] = -1;
	}
	for(int i = 0; i < NUM_INDIRECT; i++){
		rooti.indirect_ptr[i] = -1;
	}

	rooti.vstat.st_ino = 0;
	rooti.vstat.st_mode = S_IFDIR | 0755;
	rooti.vstat.st_nlink = 2;
	rooti.vstat.st_size = BLOCK_SIZE;
	rooti.vstat.st_blksize = BLOCK_SIZE;
	rooti.vstat.st_blocks = 1;

	time_t current = time(NULL);
	rooti.vstat.st_mtime = current; 
	rooti.vstat.st_ctime = current; 
	rooti.vstat.st_atime = current; 

	char *data = calloc(1, BLOCK_SIZE); //zeroed so unused slots read back as invalid
	fill_dirent((struct dirent*)data, 0, ".", 1);
	bio_write(rooti.direct_ptr[0], data);
	free(data);

	writei(0, &rooti);

	return 0;
}


/* FUSE file operations */

//initializes the file system
static void *userfs_init(struct fuse_conn_info *conn) { 

	if(dev_open(diskfile_path) != 0){ //if a disk file is not found, mkfs is called
		userfs_mkfs();
	}
	else{ //otherwise, in-memory data structures are initialied and the superblock is read from the disk
		sb = calloc(1, BLOCK_SIZE); 
		inode_map = calloc(1, BLOCK_SIZE);
		data_map = calloc(1, BLOCK_SIZE);
		bio_read(0, sb);
	}

	return NULL;
}

//destroys the file system
static void userfs_destroy(void *userdata) {

	//de-allocates in-memory data structures
	free(inode_map);
	free(data_map);
	free(sb);

	//closes the diskfile
	dev_close();
}

//gets the attribute of a given file
static int userfs_getattr(const char *path, struct stat *stbuf) {

	//gets the inode from the provided path
	struct inode hold;
	int ret_val = get_node_by_path(path, 0, &hold);
	if(ret_val < 0){
		return ret_val;
	}

	//fills a file's attribute into stbuf from the inode
	memset(stbuf, 0, sizeof(struct stat));

	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();

	time_t curr = time(NULL);
	stbuf->st_atime = curr;
	stbuf->st_ctime = hold.vstat.st_ctime;
	stbuf->st_mtime = hold.vstat.st_mtime;
	stbuf->st_nlink = hold.vstat.st_nlink;


	if(hold.vstat.st_mode & S_IFDIR){
		stbuf->st_mode = S_IFDIR | 0755;
	} 
	else{
		stbuf->st_mode = S_IFREG | 0644;
	}

	stbuf->st_size = hold.vstat.st_size; 

	return 0;
}

//opens a given directory obtained from a path
static int userfs_opendir(const char *path, struct fuse_file_info *fi) {

	struct inode hold;

	int ret_val = get_node_by_path(path, 0, &hold);
	if(ret_val < 0){
		return ret_val;
	}

	if((hold.vstat.st_mode & S_IFMT) != S_IFDIR){
		return -ENOTDIR; 
	}

	fi->fh = (uint64_t)hold.ino; 
    return 0;
}

//reads the contents of a directory and retrieves its entries
static int userfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	struct inode dir_inode;

	if(fi->fh == 0){
		int ret_val = get_node_by_path(path, 0, &dir_inode);
		if(ret_val < 0){
			return ret_val;
		}
	}
	else{
		readi(fi->fh, &dir_inode);
	}
	

	if((dir_inode.vstat.st_mode & S_IFMT) != S_IFDIR){
		return -ENOTDIR;
	}

	/*
	* reads the directory entires from its data blocks and copies them to filler;
	* each entry handed to filler carries the offset to resume from
	*/
	char* data = malloc(BLOCK_SIZE);
	if(data == NULL){
		return -ENOMEM;
	}
	struct dirent *val;

	for(int i = 0; i < NUM_DIRECT; i++){
		if(dir_inode.direct_ptr[i] == -1){
			break;
		}
		if((off_t)(i + 1) * DIRENTS_PER_BLK <= offset){
			continue; //this whole block was already returned
		}
		bio_read(dir_inode.direct_ptr[i], data);

		for(int j = 0; j < DIRENTS_PER_BLK; j++){
			off_t slot = (off_t)i * DIRENTS_PER_BLK + j;
			if(slot < offset){
				continue;
			}

			val = (struct dirent*)(data + j * sizeof(struct dirent)); 
			if(val->valid == 1){
				if(filler(buffer, val->name, NULL, slot + 1) != 0){ 
					free(data);
					return 0; //reply buffer full; caller resumes at the last offset we gave it
				}
			}
		}
	}

	free(data);
	return 0;
}

//split path into parent directory and final component, using caller-owned buffers.
static void split_path(const char *path, char dir_buf[PATH_MAX], char base_buf[PATH_MAX], char **dir_name, char **base_name) {
	snprintf(dir_buf, PATH_MAX, "%s", path);
	snprintf(base_buf, PATH_MAX, "%s", path);
	*dir_name = dirname(dir_buf);
	*base_name = basename(base_buf);
}


//creates a new directory
static int userfs_mkdir(const char *path, mode_t mode) {

	//uses dirname() and basename() to separate parent directory path and target directory name
	char dir_buf[PATH_MAX], base_buf[PATH_MAX];
	char *dir_name, *base_name;
	split_path(path, dir_buf, base_buf, &dir_name, &base_name);

	size_t name_len = strlen(base_name);
	if(name_len > MAX_NAME_LEN){
		return -ENAMETOOLONG;
	}

	//gets inode inside of parent directory
	struct inode parent;
	int ret_val = get_node_by_path(dir_name, 0, &parent);
	if(ret_val < 0){
		return ret_val;
	}
	if(parent.type != S_IFDIR){
		return -ENOTDIR; //can't create inside a regular file
	}

	// Step 3: Call get_avail_ino() to get an available inode number

	int available_ino = get_avail_ino();
	if(available_ino < 0){
		return -ENOSPC; //no space
	}

	/*
	* reserves the new directory's data block too before touching the parent,
	* so a failure here can't leave a half-made entry behind
	*/
	int available_data = get_avail_blkno();
	if(available_data < 0){
		release_ino(available_ino);
		return -ENOSPC; 
	}

	//calls dir_add() to add the directory entry of the target directory to the parent directory
	ret_val = dir_add(parent, available_ino, base_name, name_len);
	if(ret_val < 0){
		release_ino(available_ino); //gives back what we reserved
		release_blkno(available_data);
		return ret_val;
	}

	//updates the inode for the target directory
	readi(parent.ino, &parent); 

	struct inode dir_inode;
	memset(&dir_inode, 0, sizeof(struct inode));

	dir_inode.ino = available_ino;
	dir_inode.valid = 1;
	dir_inode.size = BLOCK_SIZE;
	dir_inode.type = S_IFDIR;
	dir_inode.link = 2;

	dir_inode.vstat.st_ino = available_ino;
	dir_inode.vstat.st_size = BLOCK_SIZE;
	dir_inode.vstat.st_nlink = 2;
	dir_inode.vstat.st_mode = S_IFDIR | mode; 
	dir_inode.vstat.st_blocks = 1;
	dir_inode.vstat.st_blksize = BLOCK_SIZE;

	time_t current = time(NULL);
	dir_inode.vstat.st_mtime = current;
	dir_inode.vstat.st_atime = current;
	dir_inode.vstat.st_ctime = current;

	dir_inode.direct_ptr[0] = available_data + sb->d_start_blk;
	for(int i = 1; i < NUM_DIRECT; i++){
		dir_inode.direct_ptr[i] = -1;
	}
	for(int i = 0; i < NUM_INDIRECT; i++){
		dir_inode.indirect_ptr[i] = -1;
	}

	/*
	* the new block holds "." and ".."; the rest must be zero(calloc) so
	* leftover heap bytes never look like valid entries
	*/ 
	char *data = calloc(1, BLOCK_SIZE);
	struct dirent *entries = (struct dirent*)data;
	fill_dirent(&entries[0], available_ino, ".", 1);
	fill_dirent(&entries[1], parent.ino, "..", 2);
	bio_write(dir_inode.direct_ptr[0], data);
	free(data);
	
	writei(available_ino, &dir_inode);

	parent.link += 1; //the child's ".." points back at the parent
	parent.vstat.st_nlink += 1;
	writei(parent.ino, &parent);

	return 0;
}

//creates a new file system
static int userfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	//uses dirname() and basename() to separate parent directory path and target file name
	char dir_buf[PATH_MAX], base_buf[PATH_MAX];
	char *dir_name, *base_name;
	split_path(path, dir_buf, base_buf, &dir_name, &base_name);

	size_t name_len = strlen(base_name);
	if(name_len > MAX_NAME_LEN){
		return -ENAMETOOLONG;
	}

	struct inode parent;
	int ret_val = get_node_by_path(dir_name, 0, &parent);
	if(ret_val < 0){
		return ret_val;
	}
	if(parent.type != S_IFDIR){
		return -ENOTDIR; //can't create inside a regular file
	}

	int available_ino = get_avail_ino();
	if(available_ino < 0){
		return -ENOSPC; //no space
	}

	ret_val = dir_add(parent, available_ino, base_name, name_len);
	if(ret_val < 0){
		release_ino(available_ino); //ensures that we don't leak the inode when the add fails (e.g. EEXIST)
		return ret_val;
	}

	//updates the inode for the target file
	struct inode file_inode;
	memset(&file_inode, 0, sizeof(struct inode));
	
	file_inode.ino = available_ino;
	file_inode.valid = 1;
	file_inode.size = 0; 
	file_inode.type = S_IFREG;
	file_inode.link = 1;

	file_inode.vstat.st_ino = available_ino;
	file_inode.vstat.st_size = 0;
	file_inode.vstat.st_nlink = 1;
	file_inode.vstat.st_mode = S_IFREG | mode; 
	file_inode.vstat.st_blksize = BLOCK_SIZE;
	file_inode.vstat.st_blocks = 0;

	time_t current = time(NULL);
	file_inode.vstat.st_mtime = current;
	file_inode.vstat.st_atime = current;
	file_inode.vstat.st_ctime = current;


	for(int i = 0; i < NUM_DIRECT; i++){
		file_inode.direct_ptr[i] = -1;
	}
	for(int i = 0; i < NUM_INDIRECT; i++){
		file_inode.indirect_ptr[i] = -1;
	}

	//calls writei() to write inode to disk
	fi->fh = (uint64_t)(file_inode.ino);
	writei(available_ino, &file_inode);

	return 0;
}

//opens an established file system
static int userfs_open(const char *path, struct fuse_file_info *fi) {

	struct inode val;
	int ret_val = get_node_by_path(path, 0, &val);
	if(ret_val < 0){
		return ret_val;
	}

	fi->fh = (uint64_t)(val.ino);

	return 0;
}

//reads the contents of a file system
static int userfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	struct inode file_inode;

	if(fi->fh == 0){ 
		int ret_val = get_node_by_path(path, 0, &file_inode);
		if(ret_val < 0){
			return ret_val;
		}
	}
	else{
		readi(fi->fh, &file_inode);
	}

	/*
	* based on the size and offset, reads data blocks from the disk and copies
	* the correct amoujnt of data from the offset to the buffer
	*/
	if(offset < 0 || offset >= (off_t)file_inode.size){ //ensures we never read past the end of the file
		return 0;
	}
	if(size > (size_t)(file_inode.size - offset)){
		size = file_inode.size - offset;
	}

	size_t bytes = 0;
	char hold[BLOCK_SIZE];

	while(bytes < size){
		off_t curr_pos = offset + bytes;
		size_t block_num = curr_pos / BLOCK_SIZE;
		size_t block_offset = curr_pos % BLOCK_SIZE;

		if(block_num >= (size_t)NUM_DIRECT){
			break;
		}

		size_t copy = BLOCK_SIZE - block_offset;
		if(copy > size - bytes){
			copy = size - bytes;
		}

		int data = file_inode.direct_ptr[block_num]; 
		if(data == -1){
			memset(buffer + bytes, 0, copy); //never-written block inside the file: reads as zeros
		}
		else{
			bio_read(data, hold);
			memcpy(buffer + bytes, hold + block_offset, copy);
		}

		bytes += copy;
	}

	return bytes; //returns the amount of bytes copied to buffer
}

//writes data to file system
static int userfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	struct inode file_inode;

	if(fi->fh == 0){
		int ret_val = get_node_by_path(path, 0, &file_inode);
		if(ret_val < 0){
			return ret_val;
		}
	}
	else{
		readi(fi->fh, &file_inode);
	}

	if(size == 0){
		return 0;
	}

	//based on size and offset, reads its data blocks from disk
	size_t bytes = 0;
	int err = 0;
	char hold[BLOCK_SIZE];

	while(bytes < size){
		off_t curr_pos = offset + bytes;
		size_t block_num = curr_pos / BLOCK_SIZE;
		size_t block_offset = curr_pos % BLOCK_SIZE;

		if(block_num >= (size_t)NUM_DIRECT){
			err = -EFBIG; //no more direct pointers (not handling indirect)
			break;
		}

		int fresh = 0;
		if(file_inode.direct_ptr[block_num] == -1){
			int data = get_avail_blkno();
			if(data == -1){
				err = -ENOSPC;
				break;
			}
			file_inode.direct_ptr[block_num] = data + sb->d_start_blk;
			file_inode.vstat.st_blocks += 1; //counted per block actually allocated
			fresh = 1;
		}

		//writes the correct amount of data from offset to disk
		size_t copy = BLOCK_SIZE - block_offset;
		if(copy > size - bytes){
			copy = size - bytes;
		}

		if(fresh){
			memset(hold, 0, BLOCK_SIZE); //brand-new block: don't inherit stale disk contents
		}
		else{
			bio_read(file_inode.direct_ptr[block_num], hold);
		}
		memcpy(hold + block_offset, buffer + bytes, copy);
		bio_write(file_inode.direct_ptr[block_num], hold);

		bytes += copy;
	}

	if(bytes > 0){
		off_t end = offset + bytes;
		if(end > (off_t)file_inode.size){ //size only ever grows by writing past the end
			file_inode.size = end;
			file_inode.vstat.st_size = end;
		}
		time_t current = time(NULL);
		file_inode.vstat.st_mtime = current;
	}
	
	writei(file_inode.ino, &file_inode); //uses the inode's own number, not fi->fh (which may be 0)

	if(bytes == 0){
		return err;
	}
	return bytes;
}

static struct fuse_operations userfs_ope = {
	.init		= userfs_init,
	.destroy	= userfs_destroy,

	.getattr	= userfs_getattr,
	.readdir	= userfs_readdir,
	.opendir	= userfs_opendir,
	.mkdir		= userfs_mkdir,

	.create		= userfs_create,
	.open		= userfs_open,
	.read 		= userfs_read,
	.write		= userfs_write,
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	/*
	* the globals (sb, bitmaps) are shared and unlocked, so without such safeguards, single-threaded
	* mode is the only safe way for the code to be run via adding FUSE's "-s" option
	*/
	char **args = malloc((argc + 2) * sizeof(char*));
	memcpy(args, argv, argc * sizeof(char*));
	args[argc++] = "-s";
	args[argc] = NULL;

	fuse_stat = fuse_main(argc, args, &userfs_ope, NULL);

	free(args);
	return fuse_stat;
}
