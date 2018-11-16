//____________________________________________________
//____________________________________________________
//_______Joao_Pedro_Bastos_Santos_____________________
//_______Paula_Alexandra_Agra_Graça___________________
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
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>

MODULE_LICENSE("Dual BSD/GPL");

#define SERI_MAJOR 0
#define SERI_MINOR 0
#define BASE 0x3f8
#define SERI_DEVS 1
#define Hz 100
#define FIFOLEN 8192	// 2^13

int Major=SERI_MAJOR;
int Minor=SERI_MINOR;
module_param(Major,int,0);
module_param(Minor,int,0); 

//_________________Device definition_________________
struct seri_device {
    struct kfifo *transfifo;    // transmitter fifo
    struct kfifo *receivfifo;    // receiver fifo
    spinlock_t translock;  
    spinlock_t receivlock;
    spinlock_t userlock;
    wait_queue_head_t tx;   //write
	wait_queue_head_t rx;   //read
	struct cdev cdp;        // Char device structure
    int number;             //users in use
};

dev_t dev;
struct seri_device *d = NULL;

//_________________Functions definition_________________
int op_open(struct inode *inodep, struct file *filep);
int op_close(struct inode *inode, struct file *filp);
ssize_t op_write(struct file *filep, const char __user *buff, size_t count, loff_t *offp);
ssize_t op_read(struct file *filep, char __user *buff, size_t count, loff_t *offp);

//__________________LAB4 functions___________________
irqreturn_t int_handler(int irq, void *dev_id);

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
static int seri_init(void)
{
    unsigned char lcr=0, interrupt=0, fiforeg=0;
    dev=MKDEV(Major,Minor);
	if(Major)
    {
		if(register_chrdev_region(dev,SERI_DEVS,"seri"))
        {
            printk(KERN_ALERT "Error registering device\n");
            return -1;
        }
	} 
	else
    {
		if(alloc_chrdev_region (& dev, 0,SERI_DEVS, "seri"))
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
    
    d =(struct seri_device*) kzalloc((sizeof(struct seri_device))*SERI_DEVS, GFP_KERNEL);//allocate device
    if(d==NULL)
    {
        printk(KERN_ALERT "Error in kzalloc seri_device\n");
        return -ENOMEM;
    }
    cdev_init(&d->cdp, &fops);  //full initialization of cdev
    (&d->cdp)->ops = &fops; 
    (&d->cdp)->owner = THIS_MODULE;

    d->transfifo =kfifo_alloc(FIFOLEN, GFP_KERNEL, &(d->translock));
    d->receivfifo =kfifo_alloc(FIFOLEN, GFP_KERNEL, &(d->receivlock));

    init_waitqueue_head(&(d->tx));  //iniciate wait queues
    init_waitqueue_head(&(d->rx));  //iniciate wait queues

    spin_lock_init(&d->userlock);
    spin_lock_init(&d->receivlock);
    spin_lock_init(&d->translock);

    if(cdev_add(&d->cdp, dev, 1))   //successfull if equal to 0
    {
        printk(KERN_ALERT "Device not successfully added\n"); 
        return -1;
    }
    
    if(request_region(BASE,1,"seri") == NULL)
    {
        printk(KERN_ALERT "Could not allocate port. Is being used\n");
        return -1;
    }
    
    if(request_irq(4,int_handler, SA_INTERRUPT, "seri", d))  //requests interrupt line
    {
        printk(KERN_ALERT "couldnt request interrupt\n");
        return -1;
    }

    d->number=0;    //inicializar number of users -> 0

    lcr = UART_LCR_DLAB | UART_LCR_WLEN8 | UART_LCR_STOP | UART_LCR_EPAR;
    outb(lcr, BASE+UART_LCR);   //load da line control register
    
    outb(UART_DIV_1200, BASE+UART_DLL);     //define 1200 bps - DLL
    outb(UART_DIV_1200>>8, BASE+UART_DLM);  //define 1200 bps - DLM
    
    lcr &= ~UART_LCR_DLAB;      // DLAB = 0
    outb(lcr, BASE+UART_LCR);   //load new lcr
   
    interrupt = inb(BASE + UART_IER);   //to change only the ones who matter
    interrupt = UART_IER_THRI | UART_IER_RDI;   //prepare interrupt line
    outb(interrupt, BASE+UART_IER);

    fiforeg = inb(BASE+UART_FCR);  //to change only the ones who matter - fifo control register
    fiforeg = UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_MASK;    //prepare fifo 
    outb(fiforeg, BASE+UART_FCR);

    return 0;
}

//______________________EXIT________________________
static void seri_exit(void)
{
	printk(KERN_ALERT "Goodbye, device world\n");
	
    kfifo_free(d->transfifo);
    kfifo_free(d->receivfifo);
    cdev_del(&d->cdp);
	unregister_chrdev_region(dev,SERI_DEVS);
    release_region(BASE, 1);
    free_irq(4, d); //free interrupt line
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//______________________OPEN________________________
int op_open(struct inode *inodep, struct file *filep)
{

    spin_lock(&d->userlock);    //not allowing more that 1 user to access
    if(d->number)
    {
        printk("Already being accessed, user using\n");
        return -1;
    }
    (d->number)++;  //inicialize 1 user
    spin_unlock(&(d->userlock));
	filep->private_data = container_of(inodep->i_cdev, struct seri_device, cdp);   //obtain struct adress from another struct that contains the first one

    nonseekable_open(inodep,filep);
	printk(KERN_ALERT "The file was opened\n");
    return 0;
}

//______________________CLOSE________________________
int op_close(struct inode *inode, struct file *filp)
{
    spin_lock(&(d->userlock));      
    if(d->number)       //if there was a user -> user=0
        (d->number)--;
    spin_unlock(&(d->userlock));    

	printk(KERN_ALERT "The file was closed\n");
    return 0;
}

//______________________WRITE________________________
ssize_t op_write(struct file *filep, const char __user *buff, size_t count, loff_t *offp)
{
	unsigned char *buf_kern, lsr=0; // new buffer to use in kernel space 
	unsigned long nbytes, plus=0;
    int left_byte, done_byte = 0, test=1;
    char get;

	buf_kern = (unsigned char*)kzalloc(sizeof(unsigned char)*count,GFP_KERNEL);	//GFP_KERNEL flag specifies a normal kernel allocation
	if(buf_kern == NULL)
    {
        printk(KERN_ALERT "Error in kzalloc buf_kern (write)\n");
        return -ENOMEM;
    }
    //copy_from_user makes copy of buffer used in user mode so we can use it in kernel mode. It wouldnt be possible to use it otherwise
    nbytes = copy_from_user(buf_kern, buff, count);
	if(nbytes)
	{
		printk(KERN_ALERT "Error copying buffer from user\n");
		kfree(buf_kern);	//avoid memory leaks
		return -1;
    }   

    printk(KERN_ALERT "number of bytes left(write): %lu\n", nbytes);
    
    for(left_byte = count; left_byte > 0;)
    {
        if((filep->f_flags & O_NONBLOCK) && (kfifo_len(d->transfifo) == FIFOLEN))
        {
            printk(KERN_ALERT "O_NONBLOCK (write)\n");
            return -EAGAIN;
        }
    	test = wait_event_interruptible_timeout(d->tx, kfifo_len(d->transfifo)<FIFOLEN, 5*Hz); //waits to write in UART; is there space left? //0 if false after timeout; 1 if condition true after timeout; other if condition true before timeout
    	if(!test)	//if false after timeout
    		return -1;
      /*  if(test == -ERESTARTSYS) 
        {
            kfree(buf_kern);
            printk(KERN_ALERT "Ctrl-C (write)\n");
            return -ERESTARTSYS;
        } 
      */
        done_byte = kfifo_put(d->transfifo, buf_kern+plus, left_byte);
    	plus += done_byte;
    	left_byte -= done_byte;
    	lsr = inb(BASE+UART_LSR);
    	if(lsr & UART_LSR_THRE)
    	{
    		kfifo_get(d->transfifo, &get, 1);
    		outb(get, BASE+UART_TX);
    	}
    }

	kfree(buf_kern);
	return count-nbytes;
}

//______________________READ________________________
ssize_t op_read(struct file *filep, char __user *buff, size_t count, loff_t *offp)
{
    
    unsigned char lsr=0;
	unsigned char *buf_kern; // new buffer to use in kernel space 
	unsigned long nbytes;
	int i=0, charact, test=1;

    buf_kern = (unsigned char*)kzalloc(sizeof(unsigned char)*count,GFP_KERNEL);    //GFP_KERNEL flag specifies a normal kernel allocation
    if(buf_kern == 0)
    {
        printk(KERN_ALERT "Error in kzalloc buf_kern (read)\n");
        return -ENOMEM;
    }
    while((i = kfifo_len(d->receivfifo)) < count)
    {
        if((filep->f_flags & O_NONBLOCK) && ((kfifo_len(d->receivfifo)) == 0))
        {
            printk(KERN_ALERT "O_NONBLOCK (read)\n");
            return -EAGAIN;
        }
        
        test = wait_event_interruptible_timeout(d->rx, kfifo_len(d->receivfifo)>i, 5*Hz);   //waits to read froms UART   //0 if len<i; 1 if len>i after timeout; other if len>i before timeout
    	if(!test) break;

        if(test==-ERESTARTSYS)   //Ctrl-C
        {
            printk(KERN_ALERT "Ctrl-C (read)\n");
            return -ERESTARTSYS;
        }

    }
    i = kfifo_len(d->receivfifo);
    
    charact = kfifo_get(d->receivfifo, buf_kern, i);
    if(charact != i)
    {
    	kfree(buf_kern);	//avoid memory leaks
    	printk(KERN_ALERT"GET went wrong...\n");
    	return -1;
    }

    lsr = inb(BASE+UART_LSR);
    
    if((lsr & UART_LSR_OE)) //test if there was overrun (lab3)
        {
            printk(KERN_ALERT "Error on reception, program too slow\n");
            kfree(buf_kern);	//avoid memory leaks
            return -EIO;
        }
	
    nbytes = copy_to_user(buff, buf_kern, i); // returns number of bytes it couldnt copy

	printk(KERN_ALERT "number of bytes left(read): %lu\n", nbytes);
	if(nbytes)
	{
		printk(KERN_ALERT "Error copying buffer from user. Empty receive buffer\n");	// (lab3)
        kfree(buf_kern);
		return -EAGAIN;
	}
	kfree(buf_kern);
	return i;
}

//______________________HANDLER________________________
irqreturn_t int_handler(int irq, void *dev_id) //(4, d)
{   //cannot sleep/block
    unsigned char lir, lsr_in=0, lsr_out=0;
    
    lir = inb(BASE+UART_IIR);   //adress to receive interrupts
    //printk(KERN_ALERT "Interrupt used!\n");
    if(lir & UART_IIR_NO_INT) return 0;  //if no interrupt pending -> does nothing
      	
    if (lir & UART_IIR_RDI) //if available to read
	{
		lsr_in = inb(BASE + UART_RX);

        if(kfifo_put(d->receivfifo, &lsr_in,1) == 0) //put data into the rx fifo queue
            printk(KERN_ALERT "Full kfifo buffer. Try later!\n");
		wake_up_interruptible(&(d->rx));  //wakes up the queue; processes the input lsr
	}

	if((lir & UART_IIR_THRI))  //if available to write
    {
        if(kfifo_get(d->transfifo, &lsr_out, 1))
        {
            outb(lsr_out, BASE + UART_TX);
            wake_up_interruptible(&(d->tx));
        }
        else
            printk(KERN_ALERT "Empty kfifo buffer!\n");
    }

    return IRQ_HANDLED;
}

module_init(seri_init);
module_exit(seri_exit);
