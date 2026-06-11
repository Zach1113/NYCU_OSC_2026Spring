#include "vfs.h"

#include "sched.h"
#include "uart.h"
#include "video.h"

enum devfs_kind {
    DEVFS_ROOT,
    DEVFS_UART,
    DEVFS_FB
};

struct devfs_node {
    enum devfs_kind kind;
    const char *name;
    struct vnode vnode;
};

static struct vnode_operations devfs_v_ops;
static struct file_operations devfs_f_ops;
static struct filesystem devfs_fs;

static struct devfs_node devfs_root = { .kind = DEVFS_ROOT, .name = "" };
static struct devfs_node devfs_uart = { .kind = DEVFS_UART, .name = "uart" };
static struct devfs_node devfs_fb = { .kind = DEVFS_FB, .name = "fb" };

static int streq(const char *a, const char *b) {
    if (!a || !b)
        return 0;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void irq_enable(void) {
    asm volatile("csrsi sstatus, 2");
}

static struct devfs_node *devfs_of(struct vnode *vnode) {
    return vnode ? (struct devfs_node *)vnode->internal : 0;
}

static void devfs_init_vnode(struct devfs_node *node, struct mount *mount,
                             struct vnode *parent, enum vnode_type type) {
    node->vnode.mount = mount;
    node->vnode.mounted = 0;
    node->vnode.parent = parent ? parent : &node->vnode;
    node->vnode.type = type;
    node->vnode.v_ops = type == VNODE_DIR ? &devfs_v_ops : 0;
    node->vnode.f_ops = &devfs_f_ops;
    node->vnode.internal = node;
}

static int devfs_lookup(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    struct devfs_node *dir = devfs_of(dir_node);

    if (!target)
        return VFS_EINVAL;
    *target = 0;
    if (!dir || dir->kind != DEVFS_ROOT)
        return VFS_ENOTDIR;

    if (streq(component_name, "uart")) {
        *target = &devfs_uart.vnode;
        return 0;
    }
    if (streq(component_name, "fb")) {
        *target = &devfs_fb.vnode;
        return 0;
    }
    return VFS_ENOENT;
}

static int devfs_create(struct vnode *dir_node, struct vnode **target,
                        const char *component_name) {
    (void)dir_node;
    (void)target;
    (void)component_name;
    return VFS_EROFS;
}

static int devfs_mkdir(struct vnode *dir_node, struct vnode **target,
                       const char *component_name) {
    (void)dir_node;
    (void)target;
    (void)component_name;
    return VFS_EROFS;
}

static int devfs_open(struct vnode *file_node, struct file **target) {
    if (!target)
        return VFS_EINVAL;
    *target = vfs_alloc_file(file_node, 0);
    return *target ? 0 : VFS_ENOMEM;
}

static int devfs_close(struct file *file) {
    vfs_free_file(file);
    return 0;
}

static int devfs_uart_read(struct file *file, void *buf, unsigned long len) {
    char *out = (char *)buf;
    unsigned long i;

    (void)file;
    for (i = 0; i < len; i++) {
        while (!uart_getc_nonblock(&out[i])) {
            irq_enable();
            schedule();
        }
    }
    return (int)len;
}

static int devfs_read(struct file *file, void *buf, unsigned long len) {
    struct devfs_node *node;

    if (!file || (len && !buf))
        return VFS_EINVAL;
    node = devfs_of(file->vnode);
    if (!node)
        return VFS_EINVAL;

    if (node->kind == DEVFS_ROOT)
        return VFS_EISDIR;
    if (node->kind == DEVFS_UART)
        return devfs_uart_read(file, buf, len);
    return VFS_EACCES;
}

static int devfs_write(struct file *file, const void *buf, unsigned long len) {
    struct devfs_node *node;
    int ret;

    if (!file || (len && !buf))
        return VFS_EINVAL;
    node = devfs_of(file->vnode);
    if (!node)
        return VFS_EINVAL;

    if (node->kind == DEVFS_ROOT)
        return VFS_EISDIR;
    if (node->kind == DEVFS_UART) {
        uart_write_atomic((const char *)buf, len);
        return (int)len;
    }
    if (node->kind != DEVFS_FB)
        return VFS_EINVAL;

    ret = video_framebuffer_write(file->f_pos, buf, len);
    if (ret > 0)
        file->f_pos += (unsigned long)ret;
    return ret;
}

static long devfs_lseek64(struct file *file, long offset, int whence) {
    struct devfs_node *node;

    if (!file || whence != 0 || offset < 0)
        return VFS_EINVAL;
    node = devfs_of(file->vnode);
    if (!node || node->kind != DEVFS_FB)
        return VFS_EINVAL;
    if ((unsigned long)offset > video_framebuffer_size())
        return VFS_EINVAL;

    file->f_pos = (unsigned long)offset;
    return offset;
}

static int devfs_ioctl(struct file *file, unsigned long request, void *arg) {
    struct devfs_node *node;

    if (!file || !arg)
        return VFS_EINVAL;
    node = devfs_of(file->vnode);
    if (!node || node->kind != DEVFS_FB)
        return VFS_EINVAL;
    if (request != 0)
        return VFS_EINVAL;

    video_framebuffer_info((struct framebuffer_info *)arg);
    return 0;
}

static int devfs_setup_mount(struct filesystem *fs, struct mount *mount) {
    (void)fs;
    if (!mount)
        return VFS_EINVAL;

    devfs_init_vnode(&devfs_root, mount, 0, VNODE_DIR);
    devfs_init_vnode(&devfs_uart, mount, &devfs_root.vnode, VNODE_FILE);
    devfs_init_vnode(&devfs_fb, mount, &devfs_root.vnode, VNODE_FILE);
    mount->root = &devfs_root.vnode;
    return 0;
}

static struct vnode_operations devfs_v_ops = {
    .lookup = devfs_lookup,
    .create = devfs_create,
    .mkdir = devfs_mkdir,
};

static struct file_operations devfs_f_ops = {
    .open = devfs_open,
    .close = devfs_close,
    .read = devfs_read,
    .write = devfs_write,
    .lseek64 = devfs_lseek64,
    .ioctl = devfs_ioctl,
};

static struct filesystem devfs_fs = {
    .name = "devfs",
    .setup_mount = devfs_setup_mount,
};

int devfs_init(void) {
    devfs_fs.next = 0;
    return register_filesystem(&devfs_fs);
}
