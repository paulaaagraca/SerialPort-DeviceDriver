//____________________________________________________
//____________________________________________________
//_______Joao_Pedro_Bastos_Santos______201504545______
//_______Paula_Alexandra_Agra_Graça____201503979______
//____________________________________________________
//____________________________________________________
#include <linux/init.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/cdev.h>
#include "serial_reg.h"
#include <linux/ioport.h>
#include <asm/io.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

MODULE_LICENSE("Dual BSD/GPL");

#define SERP_MAJOR 0
#define SERP_MINOR 0
#define BASE 0x3f8
#define SERP_DEVS 1

int Major=SERP_MAJOR;
int Minor=SERP_MINOR;
module_param(Major,int,0);
module_param(Minor,int,0); 

dev_t dev;
struct cdev *cdp;

//_________________Functions definition_________________
int op_open(struct inode *inodep, struct file *filep);
int op_close(struct inode *inode, struct file *filp);
ssize_t op_write(struct file *filep, const char __user *buff, size_t count, loff_t *offp);
ssize_t op_read(struct file *filep, char __user *buff, size_t count, loff_t *offp);

//________________Operations definition_________________
struct file_operations fops = {
    .owner =    THIS_MODULE,
    .llseek =	no_llseek,
    .open =     op_open,
    .release =  op_close,
    .write =    op_write,
    .read =     op_read,
};
//_____________________INIT________________________
static int serp_init(void)
{
    unsigned char lcr=0;
    dev=MKDEV(Major,Minor);
	if(Major)
    {
		if(register_chrdev_region(dev,SERP_DEVS,"serp"))
        {
            printk(KERN_ALERT "Error registering device\n");
            return -1;
        }
	} 
	else
    {
		if(alloc_chrdev_region (& dev, 0,SERP_DEVS, "serp"))
        {
            printk(KERN_ALERT "Error registering device\n");
            return -1;
        }
        else
        {
            Major=MAJOR(dev); 
            printk(KERN_INFO"Device allocated with success. The major number is %d\n", Major);
        }
    }
    cdp = cdev_alloc();
    cdp->ops = &fops; 
    cdp->owner = THIS_MODULE;
    if(cdev_add(cdp, dev, 1))   //successfull if equal to 0
    {
        printk(KERN_ALERT "Device not successfully added\n"); 
        return -1;
    }
    
    if(request_region(BASE,1,"serp") == NULL)
    {
        printk(KERN_ALERT "Could not allocate port. Is being used\n");
        return -1;
    }
    lcr = UART_LCR_DLAB | UART_LCR_WLEN8 | UART_LCR_STOP | UART_LCR_EPAR;

    outb(lcr, BASE+UART_LCR);   //load da line control register
    outb(UART_DIV_1200, BASE+UART_DLL);
    outb(UART_DIV_1200>>8, BASE+UART_DLM);
    lcr &= ~UART_LCR_DLAB;      // DLAB = 0
    outb(lcr, BASE+UART_LCR);   //load new lcr
    outb(0, BASE+UART_IER);     //disable interrupt

    return 0;
}

//______________________EXIT________________________
static void serp_exit(void)
{
	printk(KERN_ALERT "Goodbye, device world\n");
	cdev_del(cdp);
	unregister_chrdev_region(dev,SERP_DEVS);
        release_region(BASE, 1);
}

///////////////////////////////////////////////////////////////////////////////////
//______________________OPEN________________________
int op_open(struct inode *inodep, struct file *filep)
{
	filep->private_data = cdp;
	nonseekable_open(inodep,filep);
	printk(KERN_ALERT "The file was opened\n");
    return 0;
}

//______________________CLOSE________________________
int op_close(struct inode *inode, struct file *filp)
{
	printk(KERN_ALERT "The file was closed\n");
    return 0;
}

//______________________WRITE________________________
ssize_t op_write(struct file *filep, const char __user *buff, size_t count, loff_t *offp)
{
	unsigned char *buf_kern; // new buffer to use in kernel space 
	unsigned long nbytes;
    unsigned char lsr=0;
    int i=0;

	buf_kern = (unsigned char*)kzalloc(sizeof(unsigned char)*count,GFP_KERNEL);	//GFP_KERNEL flag specifies a normal kernel allocation
    nbytes = copy_from_user(buf_kern, buff, count);
	if(nbytes)
	{
		printk(KERN_ALERT "Error copying buffer from user\n");
		return -1;
    }   
    //printk(KERN_ALERT "number of bytes left (write): %lu\n", nbytes);
    while(i < count)
    {
        lsr = inb(BASE+UART_LSR);
		if(lsr & UART_LSR_THRE)
        {
            outb(buf_kern[i],BASE+UART_TX);
            i++;
        }
        else
            msleep_interruptible(HZ);
    }
	kfree(buf_kern);
	return count-nbytes;
}
//______________________READ________________________
ssize_t op_read(struct file *filep, char __user *buff, size_t count, loff_t *offp)
{
    unsigned char lsr=0;
	unsigned char *buf_kern; // new buffer to use in kernel space 
	unsigned long nbytes, clock=0;
	int i = 0;
    buf_kern = (unsigned char*)kzalloc(sizeof(unsigned char)*count,GFP_KERNEL);    //GFP_KERNEL flag specifies a normal kernel allocation
    clock = jiffies;
    while(i < count)
    {
        lsr = inb(BASE+UART_LSR);
        if((lsr & UART_LSR_OE))
        {
            printk(KERN_ALERT "Error on reception, program too slow\n");
            kfree(buf_kern);
            return -EIO;
        }
        if(lsr & UART_LSR_DR)
        {
            buf_kern[i] = inb(BASE+UART_RX);
            i++;
            clock=jiffies;
        }
        else if((jiffies-clock > 5*HZ) && (!(lsr & UART_LSR_DR)))
            break;
        
    }
	nbytes = copy_to_user(buff, buf_kern, i); // returns number of bytes it couldnt copy
	//printk(KERN_ALERT "number of bytes left(read): %lu\n", nbytes);
	if(nbytes)
	{
		printk(KERN_ALERT "Error copying buffer from user. Empty receive buffer\n");
        kfree(buf_kern);
		return -EAGAIN;
	}
	kfree(buf_kern);
	return count-nbytes;
}
module_init(serp_init);
module_exit(serp_exit);
