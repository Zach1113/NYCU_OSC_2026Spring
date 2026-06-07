#include "vfs.h"

#include "mm.h"

#define TMPFS_MAX_ENTRIES 16
#define TMPFS_MAX_FILE_SIZE 4096

struct tmpfs_node;

struct tmpfs_dirent {
    char name[VFS_MAX_NAME + 1];
    struct tmpfs_node *node;
};

struct tmpfs_node {
    enum vnode_type type;
    unsigned long size;
    struct vnode *vnode;
    union {
        struct {
            int count;
            struct tmpfs_dirent entries[TMPFS_MAX_ENTRIES];
        } dir;
        unsigned char data[TMPFS_MAX_FILE_SIZE];
    } u;
};

static struct vnode_operations tmpfs_v_ops;
static struct file_operations tmpfs_f_ops;
static struct filesystem tmpfs_fs;

static void zero_bytes(void *ptr, unsigned long len) {
    unsigned char *p = (unsigned char *)ptr;

    while (len--)
        *p++ = 0;
}

static void copy_bytes(void *dst, const void *src, unsigned long len) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (len--)
        *d++ = *s++;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static unsigned long str_len(const char *s) {
    unsigned long n = 0;

    while (s && s[n])
        n++;
    return n;
}

static void copy_name(char *dst, const char *src) {
    unsigned long i;

    for (i = 0; src[i] && i < VFS_MAX_NAME; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static struct tmpfs_node *tmpfs_of(struct vnode *vnode) {
    return vnode ? (struct tmpfs_node *)vnode->internal : 0;
}

static struct tmpfs_node *tmpfs_alloc_node(enum vnode_type type,
                                           struct mount *mount,
                                           struct vnode *parent) {
    struct tmpfs_node *node;
    struct vnode *vnode;

    node = (struct tmpfs_node *)alloc(sizeof(*node));
    vnode = (struct vnode *)alloc(sizeof(*vnode));
    if (!node || !vnode) {
        if (node)
            free(node);
        if (vnode)
            free(vnode);
        return 0;
    }

    zero_bytes(node, sizeof(*node));
    zero_bytes(vnode, sizeof(*vnode));
    node->type = type;
    node->vnode = vnode;
    vnode->mount = mount;
    vnode->parent = parent ? parent : vnode;
    vnode->type = type;
    vnode->v_ops = &tmpfs_v_ops;
    vnode->f_ops = &tmpfs_f_ops;
    vnode->internal = node;
    return node;
}

static int tmpfs_find_entry(struct tmpfs_node *dir, const char *name) {
    int i;

    if (!dir || dir->type != VNODE_DIR)
        return -1;
    for (i = 0; i < dir->u.dir.count; i++) {
        if (streq(dir->u.dir.entries[i].name, name))
            return i;
    }
    return -1;
}

static int tmpfs_add_child(struct vnode *dir_vnode, struct vnode **target,
                           const char *name, enum vnode_type type) {
    struct tmpfs_node *dir = tmpfs_of(dir_vnode);
    struct tmpfs_node *child;

    if (!target || !name || name[0] == '\0')
        return VFS_EINVAL;
    *target = 0;
    if (!dir || dir->type != VNODE_DIR)
        return VFS_ENOTDIR;
    if (str_len(name) > VFS_MAX_NAME)
        return VFS_ENAMETOOLONG;
    if (tmpfs_find_entry(dir, name) >= 0)
        return VFS_EEXIST;
    if (dir->u.dir.count >= TMPFS_MAX_ENTRIES)
        return VFS_ENOSPC;

    child = tmpfs_alloc_node(type, dir_vnode->mount, dir_vnode);
    if (!child)
        return VFS_ENOMEM;

    copy_name(dir->u.dir.entries[dir->u.dir.count].name, name); // store the name of the new child in the directory entry
    dir->u.dir.entries[dir->u.dir.count].node = child;          // store the pointer to the new child node in the directory entry
    dir->u.dir.count++;
    *target = child->vnode;
    return 0;
}

static int tmpfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    struct tmpfs_node *dir = tmpfs_of(dir_node);    // the corresponding tmpfs_node inode of the given directory vnode
    int idx;

    if (!target)
        return VFS_EINVAL;
    *target = 0;
    if (!dir || dir->type != VNODE_DIR)
        return VFS_ENOTDIR;
    idx = tmpfs_find_entry(dir, component_name);
    if (idx < 0)
        return VFS_ENOENT;

    *target = dir->u.dir.entries[idx].node->vnode;
    return 0;
}

static int tmpfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    return tmpfs_add_child(dir_node, target, component_name, VNODE_FILE);
}

static int tmpfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name) {
    return tmpfs_add_child(dir_node, target, component_name, VNODE_DIR);
}

static int tmpfs_open(struct vnode *file_node, struct file **target) {
    if (!target)
        return VFS_EINVAL;
    *target = vfs_alloc_file(file_node, 0);
    return *target ? 0 : VFS_ENOMEM;
}

static int tmpfs_close(struct file *file) {
    vfs_free_file(file);
    return 0;
}

static int tmpfs_read(struct file *file, void *buf, unsigned long len) {
    struct tmpfs_node *node;
    unsigned long readable;

    if (!file || !buf)
        return VFS_EINVAL;
    node = tmpfs_of(file->vnode);
    if (!node)
        return VFS_EINVAL;
    if (node->type == VNODE_DIR)
        return VFS_EISDIR;
    if (file->f_pos >= node->size)
        return 0;

    readable = node->size - file->f_pos;
    if (readable > len)
        readable = len;
    copy_bytes(buf, node->u.data + file->f_pos, readable);
    file->f_pos += readable;
    return (int)readable;
}

static int tmpfs_write(struct file *file, const void *buf, unsigned long len) {
    struct tmpfs_node *node;
    unsigned long writable;

    if (!file || (!buf && len))
        return VFS_EINVAL;
    node = tmpfs_of(file->vnode);
    if (!node)
        return VFS_EINVAL;
    if (node->type == VNODE_DIR)
        return VFS_EISDIR;
    if (file->f_pos >= TMPFS_MAX_FILE_SIZE && len)
        return VFS_ENOSPC;

    writable = TMPFS_MAX_FILE_SIZE - file->f_pos;
    if (writable > len)
        writable = len;
    if (writable)
        copy_bytes(node->u.data + file->f_pos, buf, writable);
    file->f_pos += writable;
    if (node->size < file->f_pos)
        node->size = file->f_pos;
    return (int)writable;
}

static int tmpfs_setup_mount(struct filesystem *fs, struct mount *mount) {
    struct tmpfs_node *root;

    (void)fs;
    if (!mount)
        return VFS_EINVAL;

    root = tmpfs_alloc_node(VNODE_DIR, mount, 0); // create the root directory node for this mount
    if (!root)
        return VFS_ENOMEM;
    root->vnode->parent = root->vnode;  // point the parent of root to itself
    mount->root = root->vnode;          // set the root vnode of this mount to the root node's vnode
    return 0;
}

static struct vnode_operations tmpfs_v_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir,
};

static struct file_operations tmpfs_f_ops = {
    .open = tmpfs_open,
    .close = tmpfs_close,
    .read = tmpfs_read,
    .write = tmpfs_write,
};

static struct filesystem tmpfs_fs = {
    .name = "tmpfs",
    .setup_mount = tmpfs_setup_mount,
};

int tmpfs_init(void) {
    tmpfs_fs.next = 0;
    return register_filesystem(&tmpfs_fs); // register tmpfs filesystem
}
