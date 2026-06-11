#include "vfs.h"

#include "cpio.h"
#include "mm.h"

struct ramfs_node {
    enum vnode_type type;
    const char *name;
    const unsigned char *data;
    unsigned long size;
    struct vnode *vnode;
};

static struct vnode_operations ramfs_v_ops;
static struct file_operations ramfs_f_ops;
static struct filesystem ramfs_fs;

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

static unsigned long str_len(const char *s) {
    unsigned long n = 0;

    while (s && s[n])
        n++;
    return n;
}

static struct ramfs_node *ramfs_of(struct vnode *vnode) {
    return vnode ? (struct ramfs_node *)vnode->internal : 0;
}

static struct ramfs_node *ramfs_alloc_node(enum vnode_type type,
                                           struct mount *mount,
                                           struct vnode *parent,
                                           const char *name,
                                           const void *data,
                                           unsigned long size) {
    struct ramfs_node *node;
    struct vnode *vnode;

    node = (struct ramfs_node *)alloc(sizeof(*node));
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
    node->name = name;
    node->data = (const unsigned char *)data;
    node->size = size;
    node->vnode = vnode;
    vnode->mount = mount;
    vnode->parent = parent ? parent : vnode;
    vnode->type = type;
    vnode->v_ops = &ramfs_v_ops;
    vnode->f_ops = &ramfs_f_ops;
    vnode->internal = node;
    return node;
}

static int ramfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    const void *data;
    unsigned long size;
    struct ramfs_node *node;

    if (!target)
        return VFS_EINVAL;
    *target = 0;
    if (!dir_node || dir_node->type != VNODE_DIR)
        return VFS_ENOTDIR;
    if (!component_name || component_name[0] == '\0' ||
        str_len(component_name) > VFS_MAX_NAME)
        return VFS_EINVAL;
    if (!cpio_find(component_name, &data, &size))
        return VFS_ENOENT;

    node = ramfs_alloc_node(VNODE_FILE, dir_node->mount, dir_node,
                            component_name, data, size);
    if (!node)
        return VFS_ENOMEM;
    *target = node->vnode;
    return 0;
}

static int ramfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    (void)dir_node;
    (void)target;
    (void)component_name;
    return VFS_EROFS;
}

static int ramfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name) {
    (void)dir_node;
    (void)target;
    (void)component_name;
    return VFS_EROFS;
}

static int ramfs_open(struct vnode *file_node, struct file **target) {
    if (!target)
        return VFS_EINVAL;
    *target = vfs_alloc_file(file_node, 0);
    return *target ? 0 : VFS_ENOMEM;
}

static int ramfs_close(struct file *file) {
    vfs_free_file(file);
    return 0;
}

static int ramfs_read(struct file *file, void *buf, unsigned long len) {
    struct ramfs_node *node;
    unsigned long readable;

    if (!file || !buf)
        return VFS_EINVAL;
    node = ramfs_of(file->vnode);
    if (!node)
        return VFS_EINVAL;
    if (node->type == VNODE_DIR)
        return VFS_EISDIR;
    if (file->f_pos >= node->size)
        return 0;

    readable = node->size - file->f_pos;
    if (readable > len)
        readable = len;
    copy_bytes(buf, node->data + file->f_pos, readable);
    file->f_pos += readable;
    return (int)readable;
}

static long ramfs_lseek64(struct file *file, long offset, int whence) {
    if (!file || whence != 0 || offset < 0)
        return VFS_EINVAL;
    file->f_pos = (unsigned long)offset;
    return offset;
}

static int ramfs_write(struct file *file, const void *buf, unsigned long len) {
    (void)file;
    (void)buf;
    (void)len;
    return VFS_EROFS;
}

static int ramfs_setup_mount(struct filesystem *fs, struct mount *mount) {
    struct ramfs_node *root;

    (void)fs;
    if (!mount)
        return VFS_EINVAL;

    root = ramfs_alloc_node(VNODE_DIR, mount, 0, "", 0, 0);
    if (!root)
        return VFS_ENOMEM;
    root->vnode->parent = root->vnode;
    mount->root = root->vnode;
    return 0;
}

static struct vnode_operations ramfs_v_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
};

static struct file_operations ramfs_f_ops = {
    .open = ramfs_open,
    .close = ramfs_close,
    .read = ramfs_read,
    .write = ramfs_write,
    .lseek64 = ramfs_lseek64,
};

static struct filesystem ramfs_fs = {
    .name = "ramfs",
    .setup_mount = ramfs_setup_mount,
};

int ramfs_init(void) {
    ramfs_fs.next = 0;
    return register_filesystem(&ramfs_fs);
}
