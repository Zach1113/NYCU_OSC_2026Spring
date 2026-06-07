#ifndef VFS_H
#define VFS_H

#define O_CREAT 00000100

#define VFS_MAX_PATH 255
#define VFS_MAX_NAME 15
#define VFS_MAX_FD   16

#define VFS_EPERM    (-1)   // Operation not permitted
#define VFS_ENOENT   (-2)   // No such file or directory
#define VFS_EIO      (-5)   // I/O error
#define VFS_EBADF    (-9)   // Bad file number
#define VFS_ENOMEM   (-12)  // Out of memory
#define VFS_EACCES   (-13)  // Permission denied
#define VFS_EEXIST   (-17)  // File exists
#define VFS_ENOTDIR  (-20)  // Not a directory
#define VFS_EISDIR   (-21)  // Is a directory
#define VFS_EINVAL   (-22)  // Invalid argument
#define VFS_ENFILE   (-23)  // Too many open files
#define VFS_EBUSY    (-24)  // Device or resource busy
#define VFS_ENOSPC   (-28)  // No space left on device
#define VFS_EROFS    (-30)  // Read-only file system
#define VFS_ENAMETOOLONG (-36)  // File name too long
#define VFS_ENOSYS   (-38)  // Function not implemented

enum vnode_type {
    VNODE_DIR,
    VNODE_FILE
};

struct file;
struct filesystem;
struct mount;
struct thread;
struct vnode;

struct file_operations {
    int (*open)(struct vnode *file_node, struct file **target);
    int (*close)(struct file *file);
    int (*read)(struct file *file, void *buf, unsigned long len);
    int (*write)(struct file *file, const void *buf, unsigned long len);
    long (*lseek64)(struct file *file, long offset, int whence);
};

struct vnode_operations {
    int (*lookup)(struct vnode *dir_node, struct vnode **target,
                  const char *component_name);
    int (*create)(struct vnode *dir_node, struct vnode **target,
                  const char *component_name);
    int (*mkdir)(struct vnode *dir_node, struct vnode **target,
                 const char *component_name);
};

struct vnode {
    struct mount *mount;
    struct mount *mounted;
    struct vnode *parent;
    enum vnode_type type;
    struct vnode_operations *v_ops;
    struct file_operations *f_ops;
    void *internal;
};

struct file {
    struct vnode *vnode;
    unsigned long f_pos;
    struct file_operations *f_ops;
    int flags;
    int ref_count;
};

struct mount {
    struct vnode *root;
    struct vnode *mountpoint;
    struct filesystem *fs;
};

struct filesystem {
    const char *name;
    int (*setup_mount)(struct filesystem *fs, struct mount *mount);
    struct filesystem *next;
};

extern struct mount *rootfs;

int vfs_init(void);
int register_filesystem(struct filesystem *fs);
int vfs_open(const char *pathname, int flags, struct file **target);
int vfs_close(struct file *file);
int vfs_write(struct file *file, const void *buf, unsigned long len);
int vfs_read(struct file *file, void *buf, unsigned long len);
int vfs_mkdir(const char *pathname);
int vfs_mount(const char *target, const char *filesystem);
int vfs_lookup(const char *pathname, struct vnode **target);
int vfs_chdir(const char *pathname);
int vfs_is_dir(struct vnode *node);

struct file *vfs_alloc_file(struct vnode *node, int flags);
void vfs_free_file(struct file *file);

void vfs_thread_init(struct thread *t);
void vfs_thread_clone(struct thread *child, struct thread *parent);
void vfs_thread_cleanup(struct thread *t);

int tmpfs_init(void);
int ramfs_init(void);

#endif
