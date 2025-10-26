#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>

MODULE_LICENSE("GPL");


//Define name
#define red_pin 67
#define yellow_pin 68
#define green_pin 44
#define btn0_pin 26
#define btn1_pin 47
#define major_number 61
#define minor_number 0

//Device name
#define DEVICE_NAME "mytraffic"


//Global Variable 
static struct gpio_desc *red, *green, *yellow, *btn0, *btn1;
static int irq_btn0 = 0; //for changing mode
static int irq_btn1 = 0; //for predstrain 
static struct timer_list traffic_timer; //for tracking cycle

//Variable for printing
static int current_mode = 0; // 0= normal, 1 = Flashing-red, 2=Flashing-yellow
static int cycle_rate = 1; // default = 1 Hz
static int count_cycle = 0; //Keep track of cycle
static bool pedrestrian_press = false; //For track at the start of red
static bool pedrestrian_cross = false; //For flashing yellow 5 cycles


//Function
static int dev_open(struct inode *inode, struct file *filp);
static int dev_release(struct inode *inode, struct file *filp);
static ssize_t dev_read(struct file *filp,
		char *buf, size_t count, loff_t *f_pos);
static ssize_t dev_write(struct file *filp,
		const char *buf, size_t count, loff_t *f_pos);
static struct gpio_desc *setup_gpio(unsigned int pin);
static int gpio_direction(struct gpio_desc *pin, bool is_output, int value);
static irqreturn_t btn0_handler(int irq, void *dev_id);
static irqreturn_t btn1_handler(int irq, void *dev_id);
static void free_gpio_pins(void);
static int mytraffic_init (void);
static void mytraffic_exit (void);
static void timer_callback(struct timer_list *t);
static int mytraffic_setup (void);


//Operation
static struct file_operations mytraffic_fops = {
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
    .release = dev_release,
};


/* Function to init and exit functions */
module_init(mytraffic_init);
module_exit(mytraffic_exit);

// Helper function
//Callback function 
static void timer_callback(struct timer_list *t){
    switch(current_mode){
        case 0:
            if (pedrestrian_cross){
                gpiod_set_value(red, 1);
                gpiod_set_value(yellow, 1);
                gpiod_set_value(green,0);

                if(++count_cycle >= 5){
                    pedrestrian_press = false;
                    pedrestrian_cross = false;
                    count_cycle = 0;
                }
            }else{
                int red_state = 0;
                int yellow_state = 0;
                int green_state = 0;
                if (count_cycle < 3){
                    green_state = 1;
                }else if (count_cycle < 4){
                    yellow_state = 1;
                }else{
                    red_state = 1; 
                }  
                gpiod_set_value(red, red_state);
                gpiod_set_value(yellow, yellow_state);
                gpiod_set_value(green, green_state);

                if (count_cycle == 4 && pedrestrian_press){
                    pedrestrian_cross = true;
                    count_cycle = -1;
                }
                
                if (++count_cycle >= 6){
                    count_cycle = 0;
                }
            }
            break;
        
        case 1:
            gpiod_set_value(red, count_cycle % 2);
            gpiod_set_value(yellow, 0);
            gpiod_set_value(green, 0);

            count_cycle ++;
            break;
        
        case 2:
            gpiod_set_value(red, 0);
            gpiod_set_value(yellow, count_cycle % 2);
            gpiod_set_value(green, 0);

            count_cycle ++;
            break;
    }

    mod_timer(&traffic_timer, jiffies + msecs_to_jiffies(1000 / cycle_rate));
}

// Set up GPIO -> return pointer
static struct gpio_desc *setup_gpio(unsigned int pin){
    struct gpio_desc *desc = gpio_to_desc(pin);
    if(IS_ERR(desc)){
        pr_err("Failed to get GPIO pin %d\n", pin);
    }
    return desc; 
}

static int gpio_direction(struct gpio_desc *pin, bool is_output, int value){
    int ret = 0;
    if (is_output){
        ret= gpiod_direction_output(pin, value); 
    }else{
        ret = gpiod_direction_input(pin);
    }
    if(ret){
        pr_err("Failed to set GPIO direction\n");
        return ret; 
    }else{
        return 0;
    }
}


// Setup GPIO pin 
static int mytraffic_setup (void){

    int ret;
    
    // Request GPIOs first (ADDED - REQUIRED!)
    ret = gpio_request(red_pin, "red-led");
    if (ret) return ret;
    ret = gpio_request(yellow_pin, "yellow-led");
    if (ret) { gpio_free(red_pin); return ret; }
    ret = gpio_request(green_pin, "green-led");
    if (ret) { gpio_free(red_pin); gpio_free(yellow_pin); return ret; }
    ret = gpio_request(btn0_pin, "btn0");
    if (ret) { gpio_free(red_pin); gpio_free(yellow_pin); gpio_free(green_pin); return ret; }
    ret = gpio_request(btn1_pin, "btn1");
    if (ret) { gpio_free(red_pin); gpio_free(yellow_pin); gpio_free(green_pin); gpio_free(btn0_pin); return ret; }
    
    // Red light
    red = setup_gpio(red_pin); 
    if(IS_ERR(red)) return(PTR_ERR(red));

    //yellow light
    yellow = setup_gpio(yellow_pin);
    if(IS_ERR(yellow)) return(PTR_ERR(yellow));

    //Green light 
    green = setup_gpio(green_pin);
    if(IS_ERR(green)) return(PTR_ERR(green));

    //Button 0
    btn0 = setup_gpio(btn0_pin);
    if(IS_ERR(btn0)) return(PTR_ERR(btn0));

    //Button 1
    btn1 = setup_gpio(btn1_pin);
    if(IS_ERR(btn1)) return(PTR_ERR(btn1));

    /*========= Setup output and input =========*/
    // Red light
    ret = gpio_direction(red, true, 0); 
    if (ret) return ret;

    //yellow light
    ret = gpio_direction(yellow, true, 0); 
    if (ret) return ret;

    //Green light 
    ret = gpio_direction(green, true, 0); 
    if (ret) return ret;
    //Button 0
    ret = gpio_direction(btn0, false, 0); 
    if (ret) return ret;
    //Button 1
    ret = gpio_direction(btn1, false, 0); 
    if (ret) return ret;

    return 0;
}

// Clean GPIO / free pin 
static void free_gpio_pins(void){
    // Turn off all LEDs
    if (!IS_ERR_OR_NULL(red)) gpiod_set_value(red, 0);
    if (!IS_ERR_OR_NULL(yellow)) gpiod_set_value(yellow, 0);
    if (!IS_ERR_OR_NULL(green)) gpiod_set_value(green, 0);
    
    // Release all GPIOs (CHANGED - use gpio_free)
    gpio_free(red_pin);
    gpio_free(yellow_pin);
    gpio_free(green_pin);
    gpio_free(btn0_pin);
    gpio_free(btn1_pin);
}

// Press Button 0
static irqreturn_t btn0_handler(int irq, void *dev_id)
{
    current_mode = (current_mode + 1) % 3;
    count_cycle = 0;
    return IRQ_HANDLED;
}

// Press Button 1
static irqreturn_t btn1_handler(int irq, void *dev_id)
{
    if (current_mode == 0 && !pedrestrian_press) {
        pedrestrian_press = true;
    }
    
    return IRQ_HANDLED;
}

static ssize_t dev_read(struct file *filp, char *buf, size_t count, loff_t *f_pos){
    int msg_len;
    char msg[256];
    int error_count;
    const char *mode;
    const char *red_str, *yellow_str, *green_str;
    const char *ped_str;

    switch(current_mode){
        case 0:
            mode = "Normal Mode\n";
            break;
        case 1:
            mode = "Flashing-red Mode\n";
            break;
        case 2:
            mode = "Flashing-yellow\n";
            break;
        default:
            mode = "Unknown\n";
            break;
    }

    red_str = gpiod_get_value(red) ? "on": "off";
    yellow_str = gpiod_get_value(yellow) ? "on": "off";
    green_str = gpiod_get_value(green) ? "on": "off";

    ped_str = (pedrestrian_press || pedrestrian_cross) ? "Active" : "Not Active";

    msg_len = snprintf(msg, sizeof(msg), 
                "Current Mode: %s\n"
                "Cycle Rate: %d Hz\n"
                "Red: %s, Yellow: %s, Green: %s\n"
                "Pedestrian: %s\n", mode, cycle_rate, red_str, yellow_str, green_str, ped_str); 

    // Only read once
    if (*f_pos > 0){
        return 0;
    }

    // Copy to user space
    error_count = copy_to_user(buf, msg, msg_len);

    if (error_count == 0){
        *f_pos += msg_len;
        return msg_len;
    }else{
        pr_err("mytraffic: Failed to send data to user\n");
        return -EFAULT;
    }
                
}


static ssize_t dev_write(struct file *filp, const char __user *buffer,
                         size_t len, loff_t *offset)
{
    char user_input[16];
    int new_cycle_rate;
    
    // Copy data from user space
    if (len > sizeof(user_input) - 1)
        len = sizeof(user_input) - 1;
    
    if (copy_from_user(user_input, buffer, len))
        return -EFAULT;
    
    user_input[len] = '\0';
    
    // The integer (1-9)
    if (kstrtoint(user_input, 10, &new_cycle_rate) == 0) {
        if (new_cycle_rate >= 1 && new_cycle_rate <= 9) {
            cycle_rate = new_cycle_rate;
            pr_info("mytraffic: Cycle rate set to %d Hz\n", cycle_rate);
        } else {
            pr_debug("mytraffic: Invalid cycle rate %d, ignoring\n", new_cycle_rate);
        }
    }
    
    return len;
}

static int dev_release(struct inode *inode, struct file *filp)
{
    pr_info("mytraffic: Device closed\n");
    return 0;
}

static int dev_open(struct inode *inode, struct file *filp)
{
    pr_info("mytraffic: Device opened\n");
    return 0;
}

static int mytraffic_init(void)
{
	int result = 0;
    int ret = 0;

	/* Registering device */
	result = register_chrdev(major_number, "mytraffic", &mytraffic_fops);
	if (result < 0)
	{
		printk(KERN_ALERT
			"mytraffic: cannot obtain major number %d\n", major_number);
		return result;
	}
	
	printk(KERN_INFO "mytraffic: module loaded\n");

    //Set up GPIO pins
    ret = mytraffic_setup();
    if (ret) {
        printk(KERN_ALERT "mytraffic: Failed to setup GPIO pins\n");
        unregister_chrdev(major_number, "mytraffic");
        return ret;
    }

    irq_btn0 = gpiod_to_irq(btn0);
    irq_btn1 = gpiod_to_irq(btn1);
    if (irq_btn0 < 0 || irq_btn1 < 0) {
        printk(KERN_ALERT "mytraffic: Failed to get IRQ numbers\n");
        free_gpio_pins();
        unregister_chrdev(major_number, "mytraffic");
        return -ENODEV;
    }

    // Request IRQs 
    ret = request_irq(irq_btn0, btn0_handler, IRQF_TRIGGER_FALLING, "btn0_irq", NULL);
    if (ret) {
        printk(KERN_ALERT "mytraffic: Failed to request IRQ for btn0\n");
        free_gpio_pins();
        unregister_chrdev(major_number, "mytraffic");
        return ret;
    }

    ret = request_irq(irq_btn1, btn1_handler, IRQF_TRIGGER_FALLING, "btn1_irq", NULL);
    if (ret) {
        printk(KERN_ALERT "mytraffic: Failed to request IRQ for btn1\n");
        free_irq(irq_btn0, NULL);
        free_gpio_pins();
        unregister_chrdev(major_number, "mytraffic");
        return ret;
    }

	// Initialize and start timer
    timer_setup(&traffic_timer, timer_callback, 0);
    mod_timer(&traffic_timer, jiffies + msecs_to_jiffies(1000 / cycle_rate));

	return 0;
}

static void mytraffic_exit(void)
{
    // Delete timer
    del_timer_sync(&traffic_timer);

    // Free IRQs (ADDED - REQUIRED!)
    if (irq_btn0 > 0) free_irq(irq_btn0, NULL);
    if (irq_btn1 > 0) free_irq(irq_btn1, NULL);

    // Free GPIO pins
    free_gpio_pins();

    /* Freeing the major number */
    unregister_chrdev(major_number, "mytraffic");

    printk(KERN_ALERT "Removing mytraffic module\n");
}
