#include "vfs.h"

#include "mm.h"
#include "sched.h"

struct mount *rootfs;

static struct filesystem *g_filesystems;

static void zero_bytes(void *ptr, unsigned long len) {
    unsigned char *p = (unsigned char *)ptr;

    while (len--)
        *p++ = 0;
}

static int streq(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static unsigned long strn_len(const char *s, unsigned long max) {
    unsigned long n = 0;

    if (!s)
        return 0;
    while (n <= max && s[n])
        n++;
    return n;
}

static void copy_name(char *dst, const char *src, unsigned long len) {
    unsigned long i;

    for (i = 0; i < len; i++)
        dst[i] = src[i];
    dst[len] = '\0';
}

static struct filesystem *find_filesystem(const char *name) {
    struct filesystem *fs = g_filesystems;

    while (fs) {
        if (streq(fs->name, name))
            return fs;
        fs = fs->next;
    }
    return 0;
}

static struct vnode *follow_mount(struct vnode *node) {
    while (node && node->mounted)
        node = node->mounted->root;
    return node;
}

static struct vnode *root_for_current(void) {
    struct thread *t = get_current();

    if (t && t->root)
        return t->root;
    return rootfs ? rootfs->root : 0;
}

static struct vnode *cwd_for_current(void) {
    struct thread *t = get_current();

    if (t && t->cwd)
        return t->cwd;
    return root_for_current();
}

static struct vnode *parent_of(struct vnode *node) {
    if (!node)
        return 0;

    if (node->mount && node == node->mount->root) {
        struct vnode *mountpoint = node->mount->mountpoint;

        if (!mountpoint)        // rootfs root has mount but not mountpoint
            return node;
        if (mountpoint->parent) // if the mountpoint has a parent, return the parent of the mountpoint    
            return mountpoint->parent;
        return mountpoint;      // if the root '/' itself is a mountpoint

    if (node->parent)
        return node->parent;
    return node;
}

static int is_dot(const char *name, unsigned long len) {
    return len == 1 && name[0] == '.';
}

static int is_dotdot(const char *name, unsigned long len) {
    return len == 2 && name[0] == '.' && name[1] == '.';
}

static int path_len_ok(const char *path) {
    return path && strn_len(path, VFS_MAX_PATH) <= VFS_MAX_PATH;
}

static int resolve_path(const char *pathname, struct vnode **target,
                        struct vnode **parent, char *leaf) {
    const char *p;
    struct vnode *cur;
    struct vnode *last_parent = 0;
    char last_name[VFS_MAX_NAME + 1];

    if (target)
        *target = 0;
    if (parent)
        *parent = 0;
    if (leaf)
        leaf[0] = '\0';

    if (!path_len_ok(pathname) || !rootfs || !rootfs->root)
        return pathname ? VFS_ENAMETOOLONG : VFS_EINVAL;

    if (pathname[0] == '/') // if the path starts with '/', start from the root vnode of the current thread
        cur = root_for_current();
    else
        cur = cwd_for_current();    // otherwise, start from the current working directory vnode of the current thread
    cur = follow_mount(cur);  // 
    if (!cur)
        return VFS_EINVAL;

    p = pathname;
    while (*p) {
        const char *start;
        unsigned long len;
        struct vnode *next;
        char name[VFS_MAX_NAME + 1];
        int ret;

        while (*p == '/')   // skip consecutive '/' characters
            p++;
        if (*p == '\0')
            break;

        start = p;
        len = 0;
        while (p[len] && p[len] != '/') // extract a component
            len++;

        if (len > VFS_MAX_NAME)
            return VFS_ENAMETOOLONG;
        if (is_dot(start, len)) {    // '.', current directory, ignore
            p += len;
            continue;
        }
        if (is_dotdot(start, len)) { // '..', parent directory,  
            cur = parent_of(cur);    // use parent_of() to get the parent vnode
            p += len;
            continue;
        }

        copy_name(name, start, len);        // for lookup
        last_parent = cur;
        copy_name(last_name, start, len);   // for setting the leaf name

        if (cur->type != VNODE_DIR || !cur->v_ops || !cur->v_ops->lookup)   // if cur is not a directory or does not support lookup, return ENOTDIR
            return VFS_ENOTDIR;

        ret = cur->v_ops->lookup(cur, &next, name);
        if (ret != 0) {                             // assume lookup for dir/missing and failed
            if (parent) {
                *parent = cur;                      // set parent to /dir vnode
                if (leaf)                           // set leaf to "missing"
                    copy_name(leaf, start, len);
            }
            return ret;
        }

        cur = follow_mount(next);
        p += len;
    }

    if (target)
        *target = cur;
    if (parent && last_parent)
        *parent = last_parent;
    if (leaf && last_parent)
        copy_name(leaf, last_name, strn_len(last_name, VFS_MAX_NAME));
    return 0;
}


static int resolve_parent_path(const char *pathname, struct vnode **parent,
                               char *leaf) {
    const char *p;
    struct vnode *cur;

    if (parent)
        *parent = 0;
    if (leaf)
        leaf[0] = '\0';
    if (!parent || !leaf)
        return VFS_EINVAL;
    if (!path_len_ok(pathname) || !rootfs || !rootfs->root)
        return pathname ? VFS_ENAMETOOLONG : VFS_EINVAL;

    cur = (pathname[0] == '/') ? root_for_current() : cwd_for_current();
    cur = follow_mount(cur);
    if (!cur)
        return VFS_EINVAL;

    p = pathname;
    while (*p) {
        const char *start;
        const char *after;
        unsigned long len;
        int final;
        char name[VFS_MAX_NAME + 1];
        struct vnode *next;
        int ret;

        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        start = p;
        len = 0;
        while (p[len] && p[len] != '/')
            len++;
        if (len > VFS_MAX_NAME)
            return VFS_ENAMETOOLONG;

        after = p + len;
        while (*after == '/')
            after++;
        final = (*after == '\0');

        if (is_dot(start, len)) {
            if (final)
                return VFS_EEXIST;
            p += len;
            continue;
        }
        if (is_dotdot(start, len)) {
            if (final)
                return VFS_EEXIST;
            cur = parent_of(cur);
            p += len;
            continue;
        }

        copy_name(name, start, len);
        if (final) {
            *parent = cur;
            copy_name(leaf, start, len);
            return 0;
        }

        if (cur->type != VNODE_DIR || !cur->v_ops || !cur->v_ops->lookup)
            return VFS_ENOTDIR;
        ret = cur->v_ops->lookup(cur, &next, name);
        if (ret != 0)
            return ret;
        cur = follow_mount(next);
        p += len;
    }

    return VFS_EINVAL;
}

int register_filesystem(struct filesystem *fs) {
    if (!fs || !fs->name || !fs->setup_mount)
        return VFS_EINVAL;
    if (find_filesystem(fs->name)) // avoid duplicate registration
        return VFS_EEXIST;

    fs->next = g_filesystems;   // insert the new filesystem at the head of the g_filesystems list
    g_filesystems = fs;         // add the new filesystem to the head of the g_filesystems list
    return 0;
}

struct file *vfs_alloc_file(struct vnode *node, int flags) {
    struct file *file;

    if (!node)
        return 0;

    file = (struct file *)alloc(sizeof(*file));
    if (!file)
        return 0;
    zero_bytes(file, sizeof(*file));
    file->vnode = node;
    file->f_ops = node->f_ops;
    file->flags = flags;
    file->ref_count = 1;
    return file;
}

void vfs_free_file(struct file *file) {
    if (file)
        free(file);
}

int vfs_open(const char *pathname, int flags, struct file **target) {
    struct vnode *node;
    int ret;

    if (!target)
        return VFS_EINVAL;
    *target = 0;

    ret = resolve_path(pathname, &node, 0, 0);
    if (ret != 0) {
        struct vnode *parent;
        char name[VFS_MAX_NAME + 1];

        if (!(flags & O_CREAT) || ret != VFS_ENOENT)  // only create a new file when the O_CREAT flag is set and the error is ENOENT
            return ret;
        ret = resolve_parent_path(pathname, &parent, name);  // resolve the parent directory vnode and the leaf name of the target file to be created
        if (ret != 0)
            return ret;
        if (!parent->v_ops || !parent->v_ops->create)
            return VFS_EACCES;
        ret = parent->v_ops->create(parent, &node, name);
        if (ret != 0)
            return ret;
    }

    node = follow_mount(node);
    if (!node || !node->f_ops || !node->f_ops->open)
        return VFS_EINVAL;

    ret = node->f_ops->open(node, target);
    if (ret == 0 && *target)
        (*target)->flags = flags;
    return ret;
}

int vfs_close(struct file *file) {
    int ret = 0;

    if (!file)
        return VFS_EBADF;
    if (file->ref_count > 1) {
        file->ref_count--;
        return 0;
    }

    if (file->f_ops && file->f_ops->close)
        ret = file->f_ops->close(file);
    else
        vfs_free_file(file);
    return ret;
}

int vfs_write(struct file *file, const void *buf, unsigned long len) {
    if (!file || !file->f_ops || !file->f_ops->write)
        return VFS_EBADF;
    if (len && !buf)
        return VFS_EINVAL;
    return file->f_ops->write(file, buf, len);
}

int vfs_read(struct file *file, void *buf, unsigned long len) {
    if (!file || !file->f_ops || !file->f_ops->read)
        return VFS_EBADF;
    if (len && !buf)
        return VFS_EINVAL;
    return file->f_ops->read(file, buf, len);
}

long vfs_lseek64(struct file *file, long offset, int whence) {
    if (!file || !file->f_ops || !file->f_ops->lseek64)
        return VFS_EBADF;
    return file->f_ops->lseek64(file, offset, whence);
}

int vfs_ioctl(struct file *file, unsigned long request, void *arg) {
    if (!file || !file->f_ops || !file->f_ops->ioctl)
        return VFS_EBADF;
    return file->f_ops->ioctl(file, request, arg);
}

int vfs_lookup(const char *pathname, struct vnode **target) {
    return resolve_path(pathname, target, 0, 0);
}

int vfs_mkdir(const char *pathname) {
    struct vnode *parent;
    struct vnode *created;
    char name[VFS_MAX_NAME + 1];
    int ret;

    ret = resolve_path(pathname, &created, 0, 0);
    if (ret == 0)
        return VFS_EEXIST;
    if (ret != VFS_ENOENT)
        return ret;

    ret = resolve_parent_path(pathname, &parent, name);
    if (ret != 0)
        return ret;
    if (!parent->v_ops || !parent->v_ops->mkdir)
        return VFS_EACCES;
    return parent->v_ops->mkdir(parent, &created, name);
}

int vfs_mount(const char *target, const char *filesystem) {
    struct filesystem *fs;
    struct vnode *mountpoint;
    struct mount *mount;
    int ret;

    fs = find_filesystem(filesystem);   // find the filesystem by name
    if (!fs)
        return VFS_ENOENT;

    ret = resolve_path(target, &mountpoint, 0, 0);  // find the mountpoint vnode
    if (ret != 0)
        return ret;
    if (!mountpoint || mountpoint->type != VNODE_DIR)
        return VFS_ENOTDIR;
    if (mountpoint->mounted)
        return VFS_EBUSY;

    mount = (struct mount *)alloc(sizeof(*mount));
    if (!mount)
        return VFS_ENOMEM;
    zero_bytes(mount, sizeof(*mount));
    mount->fs = fs;
    mount->mountpoint = mountpoint;

    ret = fs->setup_mount(fs, mount);
    if (ret != 0) {
        free(mount);
        return ret;
    }
    if (!mount->root) {
        free(mount);
        return VFS_EIO;
    }
    mount->root->mount = mount;
    mount->root->parent = mount->root;
    mountpoint->mounted = mount;
    return 0;
}

int vfs_chdir(const char *pathname) {
    struct vnode *node;
    struct thread *t = get_current();
    int ret;

    ret = resolve_path(pathname, &node, 0, 0);
    if (ret != 0)
        return ret;
    if (!node || node->type != VNODE_DIR)
        return VFS_ENOTDIR;
    if (!t)
        return VFS_EINVAL;
    t->cwd = node;
    return 0;
}

int vfs_is_dir(struct vnode *node) {
    node = follow_mount(node);
    return node && node->type == VNODE_DIR;
}

void vfs_thread_init(struct thread *t) {
    int i;

    if (!t)
        return;
    t->root = rootfs ? rootfs->root : 0;
    t->cwd = t->root;
    for (i = 0; i < VFS_MAX_FD; i++)
        t->fd_table[i] = 0;
}

int vfs_thread_init_stdio(struct thread *t) {
    struct file *opened[3];
    int i;

    if (!t)
        return VFS_EINVAL;

    for (i = 0; i < 3; i++)
        opened[i] = 0;

    for (i = 0; i < 3; i++) {
        int ret;

        if (t->fd_table[i])
            continue;
        ret = vfs_open("/dev/uart", 0, &opened[i]);
        if (ret != 0) {
            int j;

            for (j = 0; j < i; j++) {
                if (opened[j])
                    vfs_close(opened[j]);
            }
            return ret;
        }
        t->fd_table[i] = opened[i];
    }
    return 0;
}

void vfs_thread_clone(struct thread *child, struct thread *parent) {
    int i;

    if (!child)
        return;
    child->root = parent ? parent->root : (rootfs ? rootfs->root : 0);
    child->cwd = parent ? parent->cwd : child->root;
    for (i = 0; i < VFS_MAX_FD; i++) {
        child->fd_table[i] = parent ? parent->fd_table[i] : 0;
        if (child->fd_table[i])
            child->fd_table[i]->ref_count++;
    }
}

void vfs_thread_cleanup(struct thread *t) {
    int i;

    if (!t)
        return;
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (t->fd_table[i]) {
            vfs_close(t->fd_table[i]);
            t->fd_table[i] = 0;
        }
    }
    t->cwd = 0;
    t->root = 0;
}

int vfs_init(void) {
    struct filesystem *fs;
    struct mount *mount;
    int ret;

    g_filesystems = 0;
    rootfs = 0;

    ret = tmpfs_init(); // register tmpfs to the g_filesystems list
    if (ret != 0)
        return ret;
    ret = ramfs_init(); // register ramfs to the g_filesystems list
    if (ret != 0)
        return ret;
    ret = devfs_init(); // register devfs to the g_filesystems list
    if (ret != 0)
        return ret;

    fs = find_filesystem("tmpfs"); // check if tmpfs is registered
    if (!fs)
        return VFS_ENOENT;

    mount = (struct mount *)alloc(sizeof(*mount));
    if (!mount)
        return VFS_ENOMEM;
    zero_bytes(mount, sizeof(*mount));
    mount->fs = fs; // use tmpfs as the root filesystem

    ret = fs->setup_mount(fs, mount); // the actual called function is tmpfs_setup_mount
    if (ret != 0) {
        free(mount);
        return ret;
    }
    if (!mount->root) {
        free(mount);
        return VFS_EIO;
    }
    mount->root->mount = mount;
    mount->root->parent = mount->root;  // set the parent of the root vnode to itself
    rootfs = mount;                     // set the global rootfs pointer to this newly created mount

    ret = vfs_mkdir("/ramfs");
    if (ret != 0 && ret != VFS_EEXIST)
        return ret;
    ret = vfs_mount("/ramfs", "ramfs");
    if (ret != 0)
        return ret;

    ret = vfs_mkdir("/dev");
    if (ret != 0 && ret != VFS_EEXIST)
        return ret;
    return vfs_mount("/dev", "devfs");
}
