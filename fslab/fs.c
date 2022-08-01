/*
Filesystem Lab disigned and implemented by Liang Junkai,RUC
*/

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fuse.h>
#include <errno.h>
#include "disk.h"
#include <stdlib.h>

#define DIRMODE S_IFDIR|0755
#define REGMODE S_IFREG|0644

#define BLOCK_SIZE 4096
#define BLOCK_NUM 65536
#define DISK_SIZE (BLOCK_SIZE*BLOCK_NUM)

#define FILE_NUM 32768
#define MAX_NAME_LENGTH 24

#define SUPERBLOCK 0 //SuperBlock块
#define INODE_BITMAP 1 //Inode Bitmap块
#define DATA_BITMAP1 2 //DataBlock Bitmap占两块
#define DATA_BITMAP2 3 
#define INODEBLOCK_S 4 //inode块的起始
#define DATABLOCK_S 749 //datablock块的起始

#define INODE_IN_BLOCK ((int)(BLOCK_SIZE/sizeof(Inode)))
#define DIR_ENTRY_NUM ((int)((BLOCK_SIZE-4)/sizeof(Dir_entry))) //每个目录块能存放的目录项数
#define DATABLOCK_NUM (BLOCK_NUM - DATABLOCK_S)

typedef struct SuperBlock{
	unsigned long f_bsize; //块大小
	fsblkcnt_t f_blocks;//块数量
	fsblkcnt_t f_bfree; //空闲块数量
	fsblkcnt_t f_bavail;//可用块数量
	fsfilcnt_t f_files; //文件节点数
	fsfilcnt_t f_ffree; //空闲节点数
	fsfilcnt_t f_favail;//可用节点数
	unsigned long  f_namemax;//文件名长度上限
}SuperBlock;

typedef struct Bitmap{//int为4B，1个数据块4KB，等于1024个int
//每个int的每bit都表示一个block是否被占用
	int used[1024];
}Bitmap;

typedef struct Inode{  //inode-size: 92B
	mode_t mode; //表明是文件还是目录
	off_t size; //文件大小（字节数）
	time_t atime; //被访问的时间
	time_t ctime; //状态改变的时间
	time_t mtime; //被修改的时间
	int block_num; //一共使用了几个数据块
	int direct_pointer[12]; //直接指针，记录数据块的编号
	int indirect_pointer[2]; //间接指针
}Inode;

typedef struct Dir_entry{
	char filename[MAX_NAME_LENGTH + 1];
	int inode_id;
}Dir_entry;

typedef struct Directory_Block{
	int nums; //该目录下文件/子目录的数量
	Dir_entry dir_entry[DIR_ENTRY_NUM];
}Directory_Block;


SuperBlock Read_SuperBlock(){ //读super block块
	SuperBlock superblock;
	char tmp[BLOCK_SIZE];
	disk_read(SUPERBLOCK, tmp);
	superblock = *(SuperBlock*)tmp;
	return superblock;
}


void Write_SuperBlock(SuperBlock superblock){ //写super block块
	char tmp[BLOCK_SIZE];
	memcpy(tmp, (char*)(&superblock), sizeof(SuperBlock));
	disk_write(SUPERBLOCK, tmp);
}


int Read_InodeBitmap(){ //读取inode bitmap，返回空闲可用的inode编号
	char tmp[BLOCK_SIZE];
	disk_read(INODE_BITMAP, tmp);
	Bitmap* bitmap = (Bitmap*)tmp;

	int free_id = -1;
	for(int i= 0; i < 1024; i++){
		int bits = bitmap->used[i];
		for(int offset = 0; offset < 32; offset++){
			if(((bits >> (31-offset)) & 0x1) == 0){
				free_id = i*32 + offset;
				return free_id;
			}
		}
	}
	
	return free_id;
}

int Read_DataBitmap(int blockid){ //读取参数块号对应的data bitmap，返回空闲可用的data block编号
	char tmp[BLOCK_SIZE];
	disk_read(blockid, tmp);
	Bitmap* bitmap = (Bitmap*)tmp;

	int free_id = -1;
	for(int i= 0; i < 1024; i++){
		int bits = bitmap->used[i];
		for(int offset = 0; offset < 32; offset++){
			if(((bits >> (31-offset)) & 0x1) == 0){
				free_id = i*32 + offset;
				return free_id;
			}
		}
	}

	//如果在data bitmap1中没找到
	if(blockid == DATA_BITMAP1){
		free_id = Read_DataBitmap(DATA_BITMAP2);
		if(free_id != -1){
			free_id += 32768;
			if(free_id >= DATABLOCK_NUM){//超出了数据块的范围，因为databitmap2最后有一部分不对应数据块
				return -1;
			}
			return free_id;
		}
	} 
	
	return free_id;
}


void Write_Bitmap(int blockid, int changeid, int isdelete){//写bitmap块，其中isdelete参数表明是占用还是清空
	char tmp[BLOCK_SIZE];
	if(changeid > 32768){
		changeid = changeid - 32768;
		blockid = DATA_BITMAP2;
	}

	disk_read(blockid, tmp);
	Bitmap* bitmap = (Bitmap*)tmp;

	int i = (int)(changeid / 32); //找到属于第几个int
	int offset = changeid - i*32;
	if(!isdelete){ //对应写入，0变1
		bitmap->used[i] = (bitmap->used[i] | (0x1 << (31-offset)));
	}else{ //对应删除，1变0
		bitmap->used[i] = (bitmap->used[i] & (~(0x1 << (31-offset))));
	}

	char bits[BLOCK_SIZE];
	memcpy(bits, (char*)bitmap, sizeof(Bitmap));
	disk_write(blockid, bits);

	SuperBlock superblock = Read_SuperBlock();
	if(blockid == INODE_BITMAP){
		if(!isdelete){
			superblock.f_ffree--;
			superblock.f_favail--;
		}else{
			superblock.f_ffree++;
			superblock.f_favail++;
		}		
	}else{
		if(!isdelete){
			superblock.f_bfree--;
			superblock.f_bavail--;
		}else{
			superblock.f_bfree++;
			superblock.f_bavail++;
		}	
	}
	Write_SuperBlock(superblock);


}

Inode Read_Inode(int inode_id){//读取编号对应的inode
	int blockid = INODEBLOCK_S + (int)(inode_id/INODE_IN_BLOCK);
	int offset = inode_id % INODE_IN_BLOCK;
	char tmp[BLOCK_SIZE];
	disk_read(blockid, tmp);
	Inode* inodeblock = (Inode*)tmp;
	Inode inode = inodeblock[offset];
	return inode;
}

void Write_Inode(int inode_id, Inode inode){//写inode
	int blockid = INODEBLOCK_S + (int)(inode_id/INODE_IN_BLOCK);
	int offset = inode_id % INODE_IN_BLOCK;
	char tmp[BLOCK_SIZE];
	disk_read(blockid, tmp);
	Inode* inodeblock = (Inode*)tmp;
	inodeblock[offset] = inode;

	char bits[BLOCK_SIZE];
	memcpy(bits, (char*)inodeblock, BLOCK_SIZE);
	disk_write(blockid, bits);
}

void New_Indirect_Block(int indirect_block){ //创建并初始化间接指针块
	char tmp[BLOCK_SIZE];
	int num = 0;
	memcpy(tmp, (char*)(&num), sizeof(int));
	disk_write(indirect_block, tmp);
}

void Open_New_DirBlock(int blockid){ //开一个新的目录数据块
	char tmp[BLOCK_SIZE];
	Directory_Block newblock;
	newblock.nums = 0;
	memcpy(tmp, (char*)(&newblock), sizeof(Directory_Block));
	disk_write(blockid, tmp);
}

void Insert_In_Dir(int dir_inode, Dir_entry file_or_dir){//在目录中插入文件或子目录
	Inode inode = Read_Inode(dir_inode);
	int storeblock = -1;
	int indirect_block = -1;

	
	if(inode.block_num == 0){ //这个目录还是空的
		int free_block = Read_DataBitmap(DATA_BITMAP1);
		Write_Bitmap(DATA_BITMAP1, free_block, 0);
		inode.block_num = 1;
		inode.direct_pointer[0] = free_block + DATABLOCK_S;
		inode.size += BLOCK_SIZE;
		Write_Inode(dir_inode, inode);

		Open_New_DirBlock(free_block + DATABLOCK_S);
		Insert_In_Dir(dir_inode, file_or_dir);
		return;
	}else if(inode.block_num <= 12){ //直接指针
		storeblock = inode.direct_pointer[inode.block_num - 1];
	}else if(inode.block_num <= 1036){ //第一个间接指针
		indirect_block = inode.indirect_pointer[0];
		char newtmp[BLOCK_SIZE];
		disk_read(indirect_block, newtmp);
		int nums = ((int*)newtmp)[0]; //这个块里记录了几个指针
		storeblock = ((int*)newtmp)[nums]; //取最后一个
	}else{ //第二个间接指针
		indirect_block = inode.indirect_pointer[1];
		char newtmp[BLOCK_SIZE];
		disk_read(indirect_block, newtmp);
		int nums = ((int*)newtmp)[0]; //这个块里记录了几个指针
		storeblock = ((int*)newtmp)[nums]; //取最后一个
	}

	char tmp[BLOCK_SIZE];
	disk_read(storeblock, tmp);
	Directory_Block store_dir = *(Directory_Block*)tmp;
	//检查这个数据块是否已满
	if(store_dir.nums < DIR_ENTRY_NUM){
		store_dir.dir_entry[store_dir.nums] = file_or_dir;
		store_dir.nums++;

		char newtmp[BLOCK_SIZE];
		memcpy(newtmp, (char*)(&store_dir), sizeof(Directory_Block));
		disk_write(storeblock, newtmp);

		inode.mtime = time(NULL);
		inode.ctime = time(NULL);
		Write_Inode(dir_inode, inode);
	}else{ //已满，需要开新块
		int free_block = Read_DataBitmap(DATA_BITMAP1);
		Write_Bitmap(DATA_BITMAP1, free_block, 0);
		free_block += DATABLOCK_S;
		Open_New_DirBlock(free_block);
		inode.block_num++;
		inode.size += BLOCK_SIZE;

		if(inode.block_num <= 12){ //直接指针还没满
			inode.direct_pointer[inode.block_num-1] = free_block;
			Write_Inode(dir_inode, inode);
			Insert_In_Dir(dir_inode, file_or_dir);
		}else if(inode.block_num == 13){ //刚好直接指针满了，要开间接指针
			inode.block_num++; //开indirect块
			inode.size += BLOCK_SIZE;
			indirect_block = Read_DataBitmap(DATA_BITMAP1);
			Write_Bitmap(DATA_BITMAP1, indirect_block, 0);
			indirect_block += DATABLOCK_S;
			New_Indirect_Block(indirect_block);
			inode.indirect_pointer[0] = indirect_block;
			Write_Inode(dir_inode, inode);

			//修改间接指针块的内容，加上要添加的新块的编号
			char newtmp[BLOCK_SIZE];
			((int*)newtmp)[0] = 1;
			((int*)newtmp)[1] = free_block;
			disk_write(indirect_block, newtmp);
			Insert_In_Dir(dir_inode, file_or_dir);
		}else if(inode.block_num == 1037){//刚好间接指针1满了
			inode.block_num++; //开indirect块
			inode.size += BLOCK_SIZE;
			indirect_block = Read_DataBitmap(DATA_BITMAP1);
			Write_Bitmap(DATA_BITMAP1, indirect_block, 0);
			indirect_block += DATABLOCK_S;
			New_Indirect_Block(indirect_block);
			inode.indirect_pointer[1] = indirect_block;
			Write_Inode(dir_inode, inode);

			//修改间接指针块的内容，加上要添加的新块的编号
			char newtmp[BLOCK_SIZE];
			((int*)newtmp)[0] = 1;
			((int*)newtmp)[1] = free_block;
			disk_write(indirect_block, newtmp);
			Insert_In_Dir(dir_inode, file_or_dir);
		}else{
			Write_Inode(dir_inode, inode);
			char newtmp[BLOCK_SIZE];
			disk_read(indirect_block, newtmp);
			((int*)newtmp)[0] += 1;
			((int*)newtmp)[((int*)newtmp)[0]] = free_block;
			disk_write(indirect_block, newtmp);
			Insert_In_Dir(dir_inode, file_or_dir);
		}
	}
}

int Find_From_DirDB(int blockid, char* name){//从目录块中按名字找文件或子目录，找到则返回对应的inode编号，否则返回-1
	char tmp[BLOCK_SIZE];
	disk_read(blockid, tmp);
	Directory_Block* directory = (Directory_Block*)tmp;
	int nums = directory->nums;
	for(int i = 0; i < nums; i++){
		if(strcmp(directory->dir_entry[i].filename, name) == 0){
			return directory->dir_entry[i].inode_id;
		}
	}
	return -1;
}

int Find_InWhich_Block(int dir_inode, char* name){//已知目录的inode号，查找名为name的文件或子目录在哪个数据块，若找到返回对应的数据块编号，没找到返回-1
	int find_inodeid = -1;
	Inode inode = Read_Inode(dir_inode);

	int direct_ptrnum; //有几个直接指针
	if(inode.block_num < 12){
		direct_ptrnum = inode.block_num;
	}else{
		direct_ptrnum = 12;
	}
	for(int i = 0; i < direct_ptrnum; i++){//检查每个直接指针指向的块
		find_inodeid = Find_From_DirDB(inode.direct_pointer[i], name);
		if(find_inodeid != -1){ //找到了
			return inode.direct_pointer[i];
		}
	}

	//直接指针没有，需要找间接指针
	if(inode.block_num > 12){
		//先找间接指针1
		char tmp[BLOCK_SIZE];
		disk_read(inode.indirect_pointer[0], tmp);
		int* ptrs = (int*)tmp;
		int nums = ptrs[0];
		for(int i = 1; i <= nums; i++){
			find_inodeid = Find_From_DirDB(ptrs[i], name);
			if(find_inodeid != -1){
				return ptrs[i];
			}
		}

		//使用了间接指针2时，再找间接指针2
		if(inode.block_num > 1036){
			char newtmp[BLOCK_SIZE];
			disk_read(inode.indirect_pointer[1], newtmp);
			int numss = ((int*)newtmp)[0];
			for(int i = 1; i <= numss; i++){
				find_inodeid = Find_From_DirDB(((int*)newtmp)[i], name);
				if(find_inodeid != -1){
					return ((int*)newtmp)[i];
				}
			}
		}
	}
	return -1;
}

int Find_DB_From_Order(Inode inode, int order){ //找到第order个数据块的块号
	int num = inode.block_num;
	if(num <= 12){ //在直接指针范围内
		return inode.direct_pointer[num - 1];
	}else if(num <= 1036){
		order = order - 12;
		int indirect_block = inode.indirect_pointer[0];
		char tmp[BLOCK_SIZE];
		disk_read(indirect_block, tmp);
		return ((int*)tmp)[order];
	}else{
		order = order - 12 - 1023;
		int indirect_block = inode.indirect_pointer[1];
		char tmp[BLOCK_SIZE];
		disk_read(indirect_block, tmp);
		return ((int*)tmp)[order];
	}
}


int Get_Inodeid_From_Path(char* path){//从路径得到对应的inode编号,找不到返回-1
	int inode_id = -1;
	int fatherid = 0;
	int len = strlen(path);
	int cur = 1;
	char name[MAX_NAME_LENGTH + 1];
	while(cur < len){
		int record = 0;
		while(path[cur] != '/' && cur < len){
			name[record] = path[cur];
			record++;
			cur++;
		}
		name[record] = '\0';
		record++;
		cur++;
		int blockid = Find_InWhich_Block(fatherid, name);
		if(blockid == -1){
			return -1;
		}
		fatherid = Find_From_DirDB(blockid, name);
	}
	return fatherid;
}

char* Get_FileName(char* path){//从路径得到文件名
	int length = strlen(path);
	char* name = (char*)malloc(length);
	int cur = length - 1;
	while(path[cur] != '/'){
		cur--;
	}
	for(int i = 0; i < length - cur - 1; i++){
		name[i] = path[i + cur + 1];
	}
	name[length-cur-1] = '\0';
	return name;
}

char* Get_Father_Path(char* path){//从路径得到父目录路径
	int length = strlen(path);
	char* fpath = (char*)malloc(length);
	int cur = length - 1;
	while(path[cur] != '/'){
		cur--;
	}
	for(int i = 0; i < cur; i++){
		fpath[i] = path[i];
	}
	fpath[cur] = '\0';
	return fpath;
}

void Read_Dir_DB(int blockid, void* buffer, fuse_fill_dir_t filler){ //读取一个数据块中所有文件或子目录
	char tmp[BLOCK_SIZE];
	disk_read(blockid, tmp);
	Directory_Block dir;
	dir = *(Directory_Block*)tmp;
	int nums = dir.nums;
	for(int i = 0; i < nums; i++){
		filler(buffer, dir.dir_entry[i].filename, NULL, 0);
	}
}


//Format the virtual block device in the following function
int mkfs()
{
	printf("INitial\n");
	//初始化superblock块
	SuperBlock superblock;
	superblock.f_bsize = BLOCK_SIZE;
	superblock.f_blocks = BLOCK_NUM;
	superblock.f_bfree = BLOCK_NUM - 4;//去掉superblock和bitmap
	superblock.f_bavail = BLOCK_NUM - 4;
	superblock.f_files = FILE_NUM;
	superblock.f_ffree = FILE_NUM;
	superblock.f_favail = FILE_NUM;
	superblock.f_namemax = MAX_NAME_LENGTH;
	Write_SuperBlock(superblock);

	//初始化bitmap
	Bitmap bitmap;
	for(int i = 0; i < 1024; i++){
		bitmap.used[i] = 0;
	}
	char tmp[BLOCK_SIZE];
	memcpy(tmp, (char*)(&bitmap), sizeof(Bitmap));
	disk_write(INODE_BITMAP, tmp);
	disk_write(DATA_BITMAP1, tmp);
	disk_write(DATA_BITMAP2, tmp);

	int inode_id = Read_InodeBitmap();
	Write_Bitmap(INODE_BITMAP, inode_id, 0);

	Inode root;
	root.mode = DIRMODE;
	root.atime = time(NULL);
	root.ctime = time(NULL);
	root.mtime = time(NULL);
	root.block_num = 0;
	root.size = 0;
	Write_Inode(inode_id, root);

	return 0;
}

//Filesystem operations that you need to implement
int fs_getattr (const char *path, struct stat *attr)
{
	printf("Getattr is called:%s\n",path);

	int inode_id = Get_Inodeid_From_Path(path);
	if(inode_id == -1) return -ENOENT;

	Inode inode = Read_Inode(inode_id);
	attr->st_mode = inode.mode;
	attr->st_nlink = 1;
	attr->st_uid = getuid();
	attr->st_gid = getgid();
	attr->st_size = inode.size;
	attr->st_atime = inode.atime;
	attr->st_mtime = inode.mtime;
	attr->st_ctime = inode.ctime;

	return 0;
}

int fs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	printf("Readdir is called:%s\n", path);

	int inode_id = Get_Inodeid_From_Path(path);
	if(inode_id == -1) return -ENOENT;

	Inode dir_inode = Read_Inode(inode_id);

	int direct_ptrnum;  
	if(dir_inode.block_num <= 12)
		direct_ptrnum = dir_inode.block_num;
	else
		direct_ptrnum = 12;

	for(int i = 0; i < direct_ptrnum; i++){//读直接指针
		Read_Dir_DB(dir_inode.direct_pointer[i], buffer, filler);
	}
	if(dir_inode.block_num > 12){//需要读间接指针
		char tmp[BLOCK_SIZE];
		disk_read(dir_inode.indirect_pointer[0], tmp);
		int nums = ((int*)tmp)[0];
		for(int i = 1; i <= nums; i++){
			Read_Dir_DB(((int*)tmp)[i], buffer, filler);
		}

		if(dir_inode.block_num > 1036){ //使用了间接指针2，也要读
			char newtmp[BLOCK_SIZE];
			disk_read(dir_inode.indirect_pointer[1], newtmp);
			int numss = ((int*)newtmp)[0];
			for(int i = 1; i <= numss; i++){
				Read_Dir_DB(((int*)newtmp)[i], buffer, filler);
			}
		}
	}

	dir_inode.atime = time(NULL);
	Write_Inode(inode_id, dir_inode);

	return 0;
}

int fs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("Read is called:%s\n",path);
	int inode_id = Get_Inodeid_From_Path(path);
	Inode inode = Read_Inode(inode_id);

	inode.atime = time(NULL); //修改atime
	Write_Inode(inode_id, inode);

	int direct_ptrnum;
	if(inode.block_num <= 12){
		direct_ptrnum = inode.block_num;
	}else{
		direct_ptrnum = 12;
	}
	
	char file_more[inode.size];//存放文件开头到要求的停止处的内容，之后再去掉offset之前的部分
	off_t cur = 0;
	int finished = 0;
	for(int i = 0; i < direct_ptrnum; i++){ //读直接指针
		char tmp[BLOCK_SIZE];
		disk_read(inode.direct_pointer[i], tmp);
		size_t copysize;
		if(inode.size - cur < BLOCK_SIZE)
			copysize = inode.size - cur;
		else
			copysize = BLOCK_SIZE;
		memcpy(file_more + cur, tmp, copysize);
		cur = cur + copysize;
		if(cur >= offset + size){ //已经读到要求处，可以不再继续读了
			finished = 1;
			break;
		}
	}

	if(!finished){//如果还没读到要求的位置，需要尝试读间接指针
		if(inode.block_num > 12){ //读间接指针1
			int indirect_block = inode.indirect_pointer[0];
			char newtmp[BLOCK_SIZE];
			disk_read(indirect_block, newtmp);

			int* ptrs = (int*)newtmp; 
			int nums = ptrs[0]; //指向数据块的指针数量
			char tmp[BLOCK_SIZE];
			for(int i = 1; i <= nums; i++){
				disk_read(ptrs[i], tmp);
				size_t copysize;
				if(inode.size - cur < BLOCK_SIZE)
					copysize = inode.size - cur;
				else
					copysize = BLOCK_SIZE;
				memcpy(file_more + cur, tmp, copysize);
				cur = cur + copysize;
				if(cur >= offset + size){ //已经读到要求处，可以不再继续读了
					finished = 1;
					break;
				}
			}

		}

		if(!finished){//还没读到要求的位置，再尝试间接指针2
			if(inode.block_num > 1036){
				int indirect_block = inode.indirect_pointer[1];
				char newtmp[BLOCK_SIZE];
				disk_read(indirect_block, newtmp);

				int* ptrs = (int*)newtmp; 
				int nums = ptrs[0]; //指向数据块的指针数量
				char tmp[BLOCK_SIZE];
				for(int i = 1; i <= nums; i++){
					disk_read(ptrs[i], tmp);
					size_t copysize;
					if(inode.size - cur < BLOCK_SIZE)
						copysize = inode.size - cur;
					else
						copysize = BLOCK_SIZE;
					memcpy(file_more + cur, tmp, copysize);
					cur = cur + copysize;
					if(cur >= offset + size){ //已经读到要求处，可以不再继续读了
						finished = 1;
						break;
					}
				}
			}

		}

	}
	size_t realsize;//实际能读取的大小（限制于文件本身大小）
	if(inode.size < offset + size){
		realsize = inode.size - offset;
	}else{
		realsize = size;
	}
	memcpy(buffer, file_more + offset, realsize);
	return realsize;

}

int fs_mknod (const char *path, mode_t mode, dev_t dev)
{
	printf("Mknod is called:%s\n",path);

	int fatherid = Get_Inodeid_From_Path(Get_Father_Path(path));
	if(fatherid == -1) return -ENOSPC;

	int fileid = Read_InodeBitmap();
	if(fileid == -1) return -ENOSPC;

	Write_Bitmap(INODE_BITMAP, fileid, 0);//改bitmap

	Inode newfile; //改inode
	newfile.mode = REGMODE;
	newfile.size = 0;
	newfile.atime = time(NULL);
	newfile.ctime = time(NULL);
	newfile.mtime = time(NULL);
	newfile.block_num = 0;
	Write_Inode(fileid, newfile);

	Dir_entry newdir;//为要插入的文件创建目录项
	newdir.inode_id = fileid;
	strcpy(newdir.filename, Get_FileName(path));

	Insert_In_Dir(fatherid, newdir);
	return 0;
}

int fs_mkdir (const char *path, mode_t mode)
{
	printf("Mkdir is called:%s\n",path);


	int inode_id = Read_InodeBitmap();
	if(inode_id == -1) return -ENOSPC;

	Write_Bitmap(INODE_BITMAP, inode_id, 0);//改bitmap

	Inode dir_inode; //改inode
	dir_inode.mode = DIRMODE;
	dir_inode.size = 0;
	dir_inode.atime = time(NULL);
	dir_inode.ctime = time(NULL);
	dir_inode.mtime = time(NULL);
	dir_inode.block_num = 0;
	Write_Inode(inode_id, dir_inode);

	Dir_entry newdir;//为要插入的子目录创建目录项
	newdir.inode_id = inode_id;
	strcpy(newdir.filename, Get_FileName(path));

	int fatherid = Get_Inodeid_From_Path(Get_Father_Path(path));
	if(fatherid == -1) return -ENOSPC;

	Insert_In_Dir(fatherid, newdir);
	return 0;
}

int fs_rmdir (const char *path)
{
	printf("Rmdir is called:%s\n",path);

	int fatherid = Get_Inodeid_From_Path(Get_Father_Path(path));
	if(fatherid == -1) return -ENOSPC;

	Inode father = Read_Inode(fatherid);//更新父目录mtime和ctime
	father.mtime = time(NULL);
	father.ctime = time(NULL);
	Write_Inode(fatherid, father);

	char* name = Get_FileName(path);
	int fblock = Find_InWhich_Block(fatherid, name); //要删除的子目录在父目录中所在的数据块

	int inode_id = Find_From_DirDB(fblock, name);
	Inode inode = Read_Inode(inode_id);
	int direct_ptrnum;
	if(inode.block_num <= 12){
		direct_ptrnum = inode.block_num;
	}else{
		direct_ptrnum = 12;
	}
	for(int i = 0; i < direct_ptrnum; i++){ //删除直接指针指向的数据
		Write_Bitmap(DATA_BITMAP1, inode.direct_pointer[i] - DATABLOCK_S, 1);
	}
	if(inode.block_num > 12){ //删除间接指针指向的数据
		int indirect_block = inode.indirect_pointer[0];
		char tmp1[BLOCK_SIZE];
		disk_read(indirect_block, tmp1);
		int* ptrs = (int*)tmp1;
		int len = ptrs[0];
		for(int i = 1; i <= len; i++){
			Write_Bitmap(DATA_BITMAP1, ptrs[i] - DATABLOCK_S, 1);
		}
		if(inode.block_num > 1036){
			indirect_block = inode.indirect_pointer[1];
			disk_read(indirect_block, tmp1);
			len = ((int*)tmp1)[0];
			for(int i = 1; i <= len; i++){
				Write_Bitmap(DATA_BITMAP1, ((int*)tmp1)[i] - DATABLOCK_S, 1);
			}
		}
	}
	Write_Bitmap(INODE_BITMAP, inode_id, 1); //删除inode

	//将要删除的子目录之后的文件或子目录前移
	char tmp[BLOCK_SIZE];
	disk_read(fblock, tmp);
	Directory_Block* dir_block = (Directory_Block*)tmp;
	int flag = 0;
	for(int i = 0; i < dir_block->nums; i++){
		if(strcmp(dir_block->dir_entry[i].filename, name) == 0){
			flag = 1;
		}
		if(flag && (i < dir_block->nums - 1)){ //将要删除的目录后面的按顺序前移一位
			dir_block->dir_entry[i] = dir_block->dir_entry[i+1];
		}
	}
	dir_block->nums--;
	memcpy(tmp, (char*)dir_block, sizeof(Directory_Block));
	disk_write(fblock, tmp);

	
	return 0;
}

int fs_unlink (const char *path)
{
	printf("Unlink is callded:%s\n",path);

	int fatherid = Get_Inodeid_From_Path(Get_Father_Path(path));
	if(fatherid == -1) return -ENOSPC;

	Inode father = Read_Inode(fatherid);//更新父目录mtime和ctime
	father.mtime = time(NULL);
	father.ctime = time(NULL);
	Write_Inode(fatherid, father);

	char* name = Get_FileName(path);
	int fblock = Find_InWhich_Block(fatherid, name); //要删除的文件在父目录中所在的数据块

	int inode_id = Find_From_DirDB(fblock, name);
	Inode inode = Read_Inode(inode_id);
	int direct_ptrnum;
	if(inode.block_num <= 12){
		direct_ptrnum = inode.block_num;
	}else{
		direct_ptrnum = 12;
	}
	for(int i = 0; i < direct_ptrnum; i++){ //删除直接指针指向的数据
		Write_Bitmap(DATA_BITMAP1, inode.direct_pointer[i] - DATABLOCK_S, 1);
	}
	if(inode.block_num > 12){ //删除间接指针指向的数据
		int indirect_block = inode.indirect_pointer[0];
		char tmp1[BLOCK_SIZE];
		disk_read(indirect_block, tmp1);
		int* ptrs = (int*)tmp1;
		int len = ptrs[0];
		for(int i = 1; i <= len; i++){
			Write_Bitmap(DATA_BITMAP1, ptrs[i] - DATABLOCK_S, 1);
		}
		if(inode.block_num > 1036){
			indirect_block = inode.indirect_pointer[1];
			disk_read(indirect_block, tmp1);
			len = ((int*)tmp1)[0];
			for(int i = 1; i <= len; i++){
				Write_Bitmap(DATA_BITMAP1, ((int*)tmp1)[i] - DATABLOCK_S, 1);
			}
		}
	}
	Write_Bitmap(INODE_BITMAP, inode_id, 1); //删除inode

	//将要删除的文件之后的文件或子目录前移
	char tmp[BLOCK_SIZE];
	disk_read(fblock, tmp);
	Directory_Block* dir_block = (Directory_Block*)tmp;
	int flag = 0;
	for(int i = 0; i < dir_block->nums; i++){
		if(strcmp(dir_block->dir_entry[i].filename, name) == 0){
			flag = 1;
		}
		if(flag && (i < dir_block->nums - 1)){ //将要删除的目录后面的按顺序前移一位
			dir_block->dir_entry[i] = dir_block->dir_entry[i+1];
		}
	}
	dir_block->nums--;
	memcpy(tmp, (char*)dir_block, sizeof(Directory_Block));
	disk_write(fblock, tmp);

	return 0;
}

int fs_rename (const char *oldpath, const char *newpath)
{
	printf("Rename is called:%s\n",newpath);

	int old_father_id = Get_Inodeid_From_Path(Get_Father_Path(oldpath));
	if(old_father_id == -1) return -ENOSPC;
	int new_father_id = Get_Inodeid_From_Path(Get_Father_Path(newpath));
	if(new_father_id == -1) return -ENOSPC;

	char* oldname = Get_FileName(oldpath);
	char* newname = Get_FileName(newpath);
	Dir_entry file;

	//取出旧的目录块中的目标文件或子目录，并将其后文件或子目录依次前移一位
	int fblock = Find_InWhich_Block(old_father_id, oldname);
	if(fblock == -1) return -ENOSPC;
	char tmp[BLOCK_SIZE];
	disk_read(fblock, tmp);
	Directory_Block* dir_block = (Directory_Block*)tmp;
	int flag = 0;
	for(int i = 0; i < dir_block->nums; i++){
		if(strcmp(dir_block->dir_entry[i].filename, oldname) == 0){
			flag = 1;
			file = dir_block->dir_entry[i];
		}
		if(flag && (i < dir_block->nums - 1)){
			dir_block->dir_entry[i] = dir_block->dir_entry[i+1];
		}
	}
	dir_block->nums--;
	memcpy(tmp, (char*)dir_block, sizeof(Directory_Block));
	disk_write(fblock, tmp);

	//把目标文件或子目录加到新的目录中
	strcpy(file.filename, newname);
	Insert_In_Dir(new_father_id, file);

	return 0;
}

int fs_write (const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("Write is called:%s\n",path);

	off_t newsize = offset + size;
	if(fs_truncate(path, newsize) == -ENOSPC){ //检查是否有足够空间改大小
		return 0;
	}

	Inode inode = Read_Inode(Get_Inodeid_From_Path(path));
	
	int sblock = (int)(offset / BLOCK_SIZE); //写入内容的起始块的次序
	int eblock = (int)((offset + size - 1) / BLOCK_SIZE); //写入内容的结束块的次序

	int soffset = (int)(offset % BLOCK_SIZE);//写入内容起始位置偏移量
	int eoffset = (int)((offset + size - 1) % BLOCK_SIZE);//结束位置偏移量

	int cur_buf = 0;

	for(int i = sblock; i <= eblock; i++){
		char tmp[BLOCK_SIZE];
		int blockid = Find_DB_From_Order(inode, i);
		if(sblock == eblock){
			disk_read(blockid, tmp);
			memcpy(tmp + soffset, buffer, eoffset - soffset + 1);
		}else if(i == sblock){
			disk_read(blockid, tmp);
			memcpy(tmp + soffset, buffer, BLOCK_SIZE-soffset);
			cur_buf = BLOCK_SIZE - soffset;
		}else if(i == eblock){
			memcpy(tmp, buffer + cur_buf, eoffset + 1);
		}else{
			memcpy(tmp, buffer + cur_buf, BLOCK_SIZE);
			cur_buf += BLOCK_SIZE;
		}
		disk_write(blockid, tmp);
	}

	return size;
}

int fs_truncate (const char *path, off_t size)
{
	printf("Truncate is called:%s\n",path);

	int fatherid = Get_Inodeid_From_Path(Get_Father_Path(path));
	char* name = Get_FileName(path);
	//查找目标文件的inode编号,并读取inode
	int blockid = Find_InWhich_Block(fatherid, name);
	int inode_id = Find_From_DirDB(blockid, name);
	Inode inode = Read_Inode(inode_id);

	int oldnum = inode.block_num;
	int newnum = (int)(size / BLOCK_SIZE);
	if(size % BLOCK_SIZE){ //取上整
		newnum++;
	}

	if(inode.size < size){
		//新的大小更大，要扩充
		int add = newnum - oldnum;
		while((inode.block_num < 12) && (add > 0)){ //直接指针还没满
			add--;
			int newblock = Read_DataBitmap(DATA_BITMAP1);
			if(newblock == -1) return -ENOSPC;
			Write_Bitmap(DATA_BITMAP1, newblock, 0);
			inode.direct_pointer[inode.block_num] = newblock + DATABLOCK_S;
			inode.block_num++;
		}
		while(add > 0){//如果还不够，还要尝试间接指针
			if(inode.block_num == 12 || inode.block_num == 1036){//正好需要新使用一个间接指针
				int indirect_block = Read_DataBitmap(DATA_BITMAP1); //改bitmap
				if(indirect_block == -1) return -ENOSPC;
				Write_Bitmap(DATA_BITMAP1, indirect_block, 0);
				//分配的数据块存储到对应指针
				if(inode.block_num == 12){
					inode.indirect_pointer[0] = indirect_block + DATABLOCK_S;
				}else{
					inode.indirect_pointer[1] = indirect_block + DATABLOCK_S;
				}
				inode.block_num++;
				inode.size += BLOCK_SIZE;
				//初始化间接指针块
				New_Indirect_Block(indirect_block + DATABLOCK_S);
			}

			if(inode.block_num < 1036){//间接指针1还未满，可以分配
				int indirect_block = inode.indirect_pointer[0];
				char tmp[BLOCK_SIZE];
				disk_read(indirect_block, tmp);
				while((((int*)tmp)[0] < 1023) && (add > 0)){ //间接指针1还没满
					add--;
					int newblock = Read_DataBitmap(DATA_BITMAP1);
					if(newblock == -1) return -ENOSPC;
					Write_Bitmap(DATA_BITMAP1, newblock, 0);
					((int*)tmp)[((int*)tmp)[0]+1] = newblock + DATABLOCK_S;
					((int*)tmp)[0]++;
				}
				disk_write(indirect_block, tmp);
			}else{//尝试分配间接指针2
				int indirect_block = inode.indirect_pointer[1];
				char tmp[BLOCK_SIZE];
				disk_read(indirect_block, tmp);
				while((((int*)tmp)[0] < 1023) && (add > 0)){ //间接指针1还没满
					add--;
					int newblock = Read_DataBitmap(DATA_BITMAP1);
					if(newblock == -1) return -ENOSPC;
					Write_Bitmap(DATA_BITMAP1, newblock, 0);
					((int*)tmp)[((int*)tmp)[0]+1] = newblock + DATABLOCK_S;
					((int*)tmp)[0]++;
				}
				disk_write(indirect_block, tmp);
				if(add > 0) return -ENOSPC;//到这里已经没有分配空间了
			}
		}
		if(add > 0) return -ENOSPC;

	}else{//新的大小不变或更小，可能需要删除
		for(int i = oldnum; i > newnum; i--){
			if(inode.block_num <= 12){ //删除直接指针指向的数据块
				Write_Bitmap(DATA_BITMAP1, inode.direct_pointer[inode.block_num - 1]-DATABLOCK_S, 1);
				inode.block_num--;
			}else if(inode.block_num <= 1036){ //删除间接指针1数据块
				char tmp[BLOCK_SIZE];
				disk_read(inode.indirect_pointer[0], tmp);
				((int*)tmp)[0]--;
				inode.block_num--;
				
				if(((int*)tmp)[0] == 0){//如果正好空了，删除该间接指针块
					Write_Bitmap(DATA_BITMAP1, inode.indirect_pointer[0]-DATABLOCK_S, 1);
					inode.block_num--;
				}else{
					Write_Bitmap(DATA_BITMAP1, ((int*)tmp)[((int*)tmp)[0]+1]-DATABLOCK_S, 1);
				}
				
			}else{ //删除间接指针2数据块
				char tmp[BLOCK_SIZE];
				disk_read(inode.indirect_pointer[1], tmp);
				((int*)tmp)[0]--;
				inode.block_num--;
				
				if(((int*)tmp)[0] == 0){//如果正好空了，删除该间接指针块
					Write_Bitmap(DATA_BITMAP1, inode.indirect_pointer[1]-DATABLOCK_S, 1);
					inode.block_num--;
				}else{
					Write_Bitmap(DATA_BITMAP1, ((int*)tmp)[((int*)tmp)[0]+1]-DATABLOCK_S, 1);
				}
			}
		}
	}

	inode.size = size;
	inode.ctime = time(NULL);
	Write_Inode(inode_id, inode); //更新inode

	return 0;
}


int fs_utime (const char *path, struct utimbuf *buffer)
{
	printf("Utime is called:%s\n",path);

	int inode_id = Get_Inodeid_From_Path(path);
	Inode inode = Read_Inode(inode_id);
	inode.atime = buffer->actime;
	inode.mtime = buffer->modtime;
	//inode.ctime = time(NULL);
	Write_Inode(inode_id, inode);

	return 0;
}

int fs_statfs (const char *path, struct statvfs *stat)
{
	printf("Statfs is called:%s\n",path);

	SuperBlock block = Read_SuperBlock();
	stat->f_bsize = block.f_bsize;
    stat->f_blocks = block.f_blocks;
    stat->f_bfree = block.f_bfree;
    stat->f_bavail = block.f_bavail;
    stat->f_files = block.f_files;
    stat->f_ffree = block.f_ffree;
    stat->f_favail = block.f_favail;
    stat->f_namemax = block.f_namemax;
	return 0;
}

int fs_open (const char *path, struct fuse_file_info *fi)
{
	printf("Open is called:%s\n",path);
	return 0;
}

//Functions you don't actually need to modify
int fs_release (const char *path, struct fuse_file_info *fi)
{
	printf("Release is called:%s\n",path);
	return 0;
}

int fs_opendir (const char *path, struct fuse_file_info *fi)
{
	printf("Opendir is called:%s\n",path);
	return 0;
}

int fs_releasedir (const char * path, struct fuse_file_info *fi)
{
	printf("Releasedir is called:%s\n",path);
	return 0;
}

static struct fuse_operations fs_operations = {
	.getattr    = fs_getattr,
	.readdir    = fs_readdir,
	.read       = fs_read,
	.mkdir      = fs_mkdir,
	.rmdir      = fs_rmdir,
	.unlink     = fs_unlink,
	.rename     = fs_rename,
	.truncate   = fs_truncate,
	.utime      = fs_utime,
	.mknod      = fs_mknod,
	.write      = fs_write,
	.statfs     = fs_statfs,
	.open       = fs_open,
	.release    = fs_release,
	.opendir    = fs_opendir,
	.releasedir = fs_releasedir
};

int main(int argc, char *argv[])
{
	if(disk_init())
		{
		printf("Can't open virtual disk!\n");
		return -1;
		}
	if(mkfs())
		{
		printf("Mkfs failed!\n");
		return -2;
		}
    return fuse_main(argc, argv, &fs_operations, NULL);
}
