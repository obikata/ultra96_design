#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <asm/current.h>
#include <asm/uaccess.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/poll.h>
#include <linux/videodev2.h>
#include "zynq_v4l2.h"

MODULE_DESCRIPTION("ZYNQ v4l2 device driver");
MODULE_AUTHOR("osawa");
MODULE_LICENSE("Dual BSD/GPL");

int vdma_h_res = 640;
int vdma_v_res = 480;
module_param(vdma_h_res, int, S_IRUGO);
module_param(vdma_v_res, int, S_IRUGO);
MODULE_PARM_DESC(vdma_h_res, "v4l2 buffer width");
MODULE_PARM_DESC(vdma_v_res, "v4l2 buffer height");

static int zynq_v4l2_open(struct inode *inode, struct file *filp)
{
	struct zynq_v4l2_data *dp;

	printk(KERN_INFO "%s\n", __FUNCTION__);
	dp = container_of(inode->i_cdev, struct zynq_v4l2_data, cdev);
	if (dp == NULL) {
		printk(KERN_ERR "container_of failed\n");
		return -EFAULT;
	}
	filp->private_data = dp;

	return 0;
}

static int zynq_v4l2_close(struct inode *inode, struct file *filp)
{
	struct zynq_v4l2_data *dp = filp->private_data;

	printk(KERN_INFO "%s\n", __FUNCTION__);
	if (dp->user_mem) {
		vfree(dp->user_mem);
		dp->user_mem = NULL;
	}

	return 0;
}

static ssize_t zynq_v4l2_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct zynq_v4l2_data *dp = filp->private_data;

	if (count > vdma_h_res * vdma_v_res * 3) {
		return -EINVAL;
	}
	if (raw_copy_to_user(buf, dp->virt_wb_vdma, count) != 0) {
		return -EFAULT;
	}

	return count;
}

static ssize_t zynq_v4l2_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	printk(KERN_INFO "%s\n", __FUNCTION__);	

	return 0;
}

static int find_oldest(uint32_t active_bits, int latest)
{
	uint32_t bits = active_bits;

	bits &= ~((latest << 1) - 1);
	if (bits ) {
		return __builtin_ctz(bits);
	} else if (active_bits) {
		return __builtin_ctz(active_bits);
	} else {
		return -1;
	}
}

static unsigned int zynq_v4l2_poll(struct file *filp, struct poll_table_struct *waitp)
{
	struct zynq_v4l2_data *dp = filp->private_data;
	int do_wait, rc;

	printk(KERN_INFO "%s\n", __FUNCTION__);	

	spin_lock_irq(&dp->lock);
	if (find_oldest(dp->active_bits, dp->latest_frame) == -1) {
		do_wait = 1;
	} else {
		do_wait = 0;
	}
	spin_unlock_irq(&dp->lock);

	if (do_wait) {
		poll_wait(filp, &dp->waitq, waitp);
		spin_lock_irq(&dp->lock);
		if (find_oldest(dp->active_bits, dp->latest_frame) == -1) {
			rc = 0;
		} else {
			rc = (POLLIN | POLLRDNORM);
		}
		spin_unlock_irq(&dp->lock);
	} else {
		rc = (POLLIN | POLLRDNORM);
	}

	return rc;
}

static long zynq_v4l2_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct zynq_v4l2_data *dp = filp->private_data;
	int rc, oldest;

	printk(KERN_INFO "%s\n", __FUNCTION__);

	switch (cmd) {
	case VIDIOC_G_FMT:
		if (raw_copy_from_user(&fmt, (void __user *)arg, sizeof(fmt))) {
			return -EFAULT;
		}
		switch (fmt.type) {
		case V4L2_BUF_TYPE_VIDEO_CAPTURE:
			fmt.fmt.pix.width = vdma_h_res;
			fmt.fmt.pix.height = vdma_v_res;
			fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
			fmt.fmt.pix.field = V4L2_FIELD_ANY;
			if (raw_copy_to_user((void __user *)arg, &fmt, sizeof(fmt))) {
				return -EFAULT;
			}
			return 0;
		default:
			return -ENOSYS;
		}
		break;
	case VIDIOC_S_FMT:
		if (raw_copy_from_user(&fmt, (void __user *)arg, sizeof(fmt))) {
			return -EFAULT;
		}
		if (fmt.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			return -ENOSYS;
		}
		if ((fmt.fmt.pix.width != vdma_h_res) ||
			(fmt.fmt.pix.height != vdma_v_res) ||
			(fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB24)) {
			return -EINVAL;
		}
		return 0;
	case VIDIOC_REQBUFS:
		if (raw_copy_from_user(&req, (void __user *)arg, sizeof(req))) {
			return -EFAULT;
		}
		if (req.memory != V4L2_MEMORY_MMAP) {
			return -EINVAL;
		}
		if (req.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			return -EINVAL;
		}
		if (req.count > 32) {
			return -EINVAL;
		}
		dp->user_frame_count = req.count;
		dp->user_mem = vmalloc(dp->frame_size * req.count);
		if (!dp->user_mem) {
			return -ENOMEM;
		}
		return 0;
	case VIDIOC_QUERYBUF:
		if (raw_copy_from_user(&buf, (void __user *)arg, sizeof(buf))) {
			return -EFAULT;
		}
		if (buf.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			return -EINVAL;
		}
		if (buf.type != V4L2_MEMORY_MMAP) {
			return -EINVAL;
		}
		if (buf.index >= dp->user_frame_count) {
			return -EINVAL;
		}
		buf.length = dp->frame_size;
		buf.m.offset = buf.index * dp->frame_size;
		if (raw_copy_to_user((void __user *)arg, &buf, sizeof(buf))) {
			return -EFAULT;
		}
		return 0;
	case VIDIOC_QBUF:
		if (raw_copy_from_user(&buf, (void __user *)arg, sizeof(buf))) {
			return -EFAULT;
		}
		if (buf.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			return -EINVAL;
		}
		if (buf.type != V4L2_MEMORY_MMAP) {
			return -EINVAL;
		}
		if (buf.index >= dp->user_frame_count) {
			return -EINVAL;
		}
		spin_lock_irq(&dp->lock);
		dp->queue_bits |= (1 << buf.index);
		spin_unlock_irq(&dp->lock);
		return 0;
	case VIDIOC_DQBUF:
		if (raw_copy_from_user(&buf, (void __user *)arg, sizeof(buf))) {
			return -EFAULT;
		}
		if (buf.type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
			return -EINVAL;
		}
		if (buf.type != V4L2_MEMORY_MMAP) {
			return -EINVAL;
		}
		if (buf.index >= dp->user_frame_count) {
			return -EINVAL;
		}
		spin_lock_irq(&dp->lock);
		oldest = find_oldest(dp->active_bits, dp->latest_frame);
		if (oldest == -1 ) {
			rc = -EAGAIN;
		} else {
			dp->queue_bits &= ~(1 << oldest);
			dp->active_bits &= ~(1 << oldest);
			buf.index = oldest;
			rc = 0;
		}
		spin_unlock_irq(&dp->lock);
		return 0;
	case VIDIOC_STREAMON:
		zynq_v4l2_vdma_intr_enable(dp);
		return 0;
	case VIDIOC_STREAMOFF:
		zynq_v4l2_vdma_intr_disable(dp);
		return 0;
	default:
		return -ENOSYS;
	}
}

static int zynq_v4l2_mmap(struct file *filp, struct vm_area_struct *vma)
{
	printk(KERN_INFO "%s\n", __FUNCTION__);

	return 0;
}

static struct file_operations zynq_v4l2_fops = {
	.open           = zynq_v4l2_open,
	.release        = zynq_v4l2_close,
	.read           = zynq_v4l2_read,
	.write          = zynq_v4l2_write,
	.poll           = zynq_v4l2_poll,
	.unlocked_ioctl = zynq_v4l2_ioctl,
	.compat_ioctl   = zynq_v4l2_ioctl,
	.mmap           = zynq_v4l2_mmap,
};

static int zynq_v4l2_create_cdev(struct device *parent, struct zynq_v4l2_data *dp)
{
	int ret;
	dev_t dev;
	int minor;

	printk(KERN_INFO "%s\n", __FUNCTION__);

	ret = alloc_chrdev_region(&dev, MINOR_BASE, MINOR_NUM, DRIVER_NAME);
	if (ret != 0) {
		printk(KERN_ERR "alloc_chrdev_region = %d\n", ret);
		return -1;
	}

	dp->major = MAJOR(dev);
	printk(KERN_INFO " major = %d\n", dp->major);
	dev = MKDEV(dp->major, MINOR_BASE);

	cdev_init(&dp->cdev, &zynq_v4l2_fops);
	dp->cdev.owner = THIS_MODULE;

	ret = cdev_add(&dp->cdev, dev, MINOR_NUM);
	if (ret != 0) {
		printk(KERN_ERR "cdev_add = %d\n", ret);
		unregister_chrdev_region(dev, MINOR_NUM);
		return -1;
	}

	dp->class = class_create(THIS_MODULE, "v4l2");
	if (IS_ERR(dp->class)) {
		printk(KERN_ERR "class_create\n");
		cdev_del(&dp->cdev);
		unregister_chrdev_region(dev, MINOR_NUM);
		return -1;
	}

	for (minor = MINOR_BASE; minor < MINOR_BASE + MINOR_NUM; minor++) {
		device_create(dp->class, parent, MKDEV(dp->major, minor), dp, "video%d", minor);
	}
	dp->dma_dev = parent;

	return 0;
}

static void zynq_v4l2_delete_cdev(struct zynq_v4l2_data *dp)
{
	int minor;
	dev_t dev = MKDEV(dp->major, MINOR_BASE);

	printk(KERN_INFO "%s\n", __FUNCTION__);
	for (minor = MINOR_BASE; minor < MINOR_BASE + MINOR_NUM; minor++) {
		device_destroy(dp->class, MKDEV(dp->major, minor));
	}
	class_destroy(dp->class);
	cdev_del(&dp->cdev);
	unregister_chrdev_region(dev, MINOR_NUM);
}

static int zynq_v4l2_hw_init(struct device *dev, struct zynq_v4l2_data *dp)
{
	printk(KERN_INFO "%s\n", __FUNCTION__);

	if (zynq_v4l2_vdma_init(dev, dp)) {
		return -ENODEV;
	}

	if (zynq_v4l2_demosaic_init()) {
		free_irq(dp->irq_num_vdma, dp);
		dma_free_coherent(dp->dma_dev, dp->alloc_size_wb_vdma, dp->virt_wb_vdma, dp->phys_wb_vdma);
		iounmap(dp->reg_vdma);
		return -ENODEV;
	}

	if (zynq_v4l2_mipicsi_init()) {
		free_irq(dp->irq_num_vdma, dp);
		dma_free_coherent(dp->dma_dev, dp->alloc_size_wb_vdma, dp->virt_wb_vdma, dp->phys_wb_vdma);
		iounmap(dp->reg_vdma);
		return -ENODEV;
	}

	return 0;
}

static int zynq_v4l2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zynq_v4l2_data *dp;

	dev_info(dev, "Device Tree Probing");
	printk(KERN_INFO "%s\n", __FUNCTION__);

	dp = (struct zynq_v4l2_data *)kzalloc(sizeof(struct zynq_v4l2_data), GFP_KERNEL);
	if (!dp) {
		dev_err(dev, "Could not allocate memory\n");
		return -ENOMEM;
	}
	spin_lock_init(&dp->lock);
	init_waitqueue_head(&dp->waitq);

	dev_set_drvdata(dev, dp);

	if (zynq_v4l2_create_cdev(&pdev->dev, dp)) {
		dev_err(dev, "zynq_v4l2_create_cdev failed\n");
		kfree(dp);
		dev_set_drvdata(dev, NULL);
		return -EBUSY;
	}

	if (zynq_v4l2_hw_init(&pdev->dev, dp)) {
		dev_err(dev, "hw_init failed\n");
		zynq_v4l2_delete_cdev(dp);
		kfree(dp);
		dev_set_drvdata(dev, NULL);
		return -ENODEV;
	}

	return 0;
}

static int zynq_v4l2_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zynq_v4l2_data *dp = dev_get_drvdata(dev);
	extern int frame_cnt_intr;

	printk(KERN_INFO "%s\n", __FUNCTION__);
	printk(KERN_INFO "frame_cnt_intr = %d\n", frame_cnt_intr);
	zynq_v4l2_delete_cdev(dp);
	free_irq(dp->irq_num_vdma, dp);
	dma_free_coherent(dp->dma_dev, dp->alloc_size_wb_vdma, dp->virt_wb_vdma, dp->phys_wb_vdma);
	iounmap(dp->reg_vdma);
	if (dp->user_mem) {
		vfree(dp->user_mem);
	}
	kfree(dp);
	dev_set_drvdata(dev, NULL);

	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id zynq_v4l2_of_match[] = {
	{ .compatible = "fixstars,zynq-v4l2-1.0", },
	{ /* end of table */ },
};
MODULE_DEVICE_TABLE(of, zynq_v4l2_of_match);
#else
#define zynq_v4l2_of_match
#endif

static struct platform_driver zynq_v4l2_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = zynq_v4l2_of_match,
	},
	.probe = zynq_v4l2_probe,
	.remove = zynq_v4l2_remove,
};

static int zynq_v4l2_init(void)
{
	printk(KERN_INFO "%s\n", __FUNCTION__);

    return platform_driver_register(&zynq_v4l2_driver);
}

static void zynq_v4l2_exit(void)
{
	printk(KERN_INFO "%s\n", __FUNCTION__);
	platform_driver_unregister(&zynq_v4l2_driver);
}

module_init(zynq_v4l2_init);
module_exit(zynq_v4l2_exit);
