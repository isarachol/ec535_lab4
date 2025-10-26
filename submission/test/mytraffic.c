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


//Define name
#define red_pin 67
#define yellow_pin 68
#define green_pin 44
#define yellow_pin 68
#define btn0_pin 26
#define btn1_pin 46
#define major_number 60
#define minor_number 0

//Device name
#define DEVICE_NAME "mytraffic"


//Global Variable 
static struct gpio_desc *red, *green, *yellow, *btn0, *btn1;
static int irq_btn0 = 0; //for changing mode
static int irq_btn1 = 1; //for predstrain 
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
static int mytraffic_exit (void);
static void timer_callback(struct timer_list *t);
static int mytraffic_setup (void);


//Operation
static struct file_operations mytraffic_fops{
    .open = dev_open,
    .read = dev_read,
    .write = dev_write.
    .release = dev_release,
};


/* Function to init and exit functions */
module_init(mytraffic_init);
module_exit(mytimer_exit);

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
    if(IS_ERR(red)){
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
        pr_err("Failed to set GPIO direction\n")
        gpiod_put(pin), 
        return ret; 
    }else{
        return 0;
    }
}


// Setup GPIO pin 
static int mytraffic_setup (void){

    int ret;
    // Request all GPIOs first
    ret = gpio_request(red_pin, "red-led");
    if (ret) {
        pr_err("mytraffic: Failed to request GPIO %d (red): %d\n", red_pin, ret);
        gpiod_put(red);
        return ret;
    }
    
    ret = gpio_request(yellow_pin, "yellow-led");
    if (ret) {
        pr_err("mytraffic: Failed to request GPIO %d (yellow): %d\n", yellow_pin, ret);
        ggpiod_put(yellow);
        return ret;
    }
    
    ret = gpio_request(green_pin, "green-led");
    if (ret) {
        pr_err("mytraffic: Failed to request GPIO %d (green): %d\n", green_pin, ret);
        gpiod_put(green);
        return ret;
    }
    
    ret = gpio_request(btn0_pin, "btn0");
    if (ret) {
        pr_err("mytraffic: Failed to request GPIO %d (btn0): %d\n", btn0_pin, ret);
        gpiod_put(btn0_pin);
        return ret;
    }
    
    ret = gpio_request(btn1_pin, "btn1");
    if (ret) {
        pr_err("mytraffic: Failed to request GPIO %d (btn1): %d\n", btn1_pin, ret);
        gpiod_put(btn1);
        return ret;
    }
    // return value for red, yellow and green
    int ret_red, ret_yellow, ret_green, ret_btn0, ret_btn1 = 0;
    
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

    /*========= Setup output and imput =========*/
    // Red light
    gpio_direction(red, true, 0); 

    //yellow light
    gpio_direction(yellow, true, 0); 


    //Green light 
    gpio_direction(green, true, 0); 

    //Button 0
    gpio_direction(btn0, false, 0); 

    //Button 1
    gpio_direction(btn1, false, 0); 


    return 0;
}

// Clean GPIO / free pin 
static void free_gpio_pins(void){
    // Turn off all LEDs
    gpiod_set_value(red, 0);
    gpiod_set_value(yellow, 0);
    gpiod_set_value(green, 0);
    
    // Release all GPIOs
    gpiod_put(red);
    gpiod_put(yellow);
    gpiod_put(green);
    gpiod_put(btn0);
    gpiod_put(btn1);
}

// Press Button 0
static irqreturn_t btn0_handler(int irq, void *dev_id)
{
    current_mode = (current_mode + 1) % 3;
    count_cycle = 0;
    return IRQ_HANDLED;
}

// Press Button 1
static irqreturn_t btn0_handler(int irq, void *dev_id)
{
    if (current_mode == 0 && !pedestrian_press) {
        pedestrian_present = true;
    }
    
    return IRQ_HANDLED;
}



static int mytraffic_init(void)
{
	int result = 0;

	/* Registering device */
	result = register_chrdev(major_number, "mytraffic", &mytraffic_fops);
	if (result < 0)
	{
		printk(KERN_ALERT
			"mytimer: cannot obtain major number %d\n", mytimer_major);
		return result;
	}
	
	printk(KERN_INFO "mytimer: module loaded\n");

	return 0;

fail: 
	mytimer_exit(); 
	return result;
}

static int my_pdrv_remove(struct platform_device *pdev)
{
    free_irq(irq, NULL);
    gpiod_put(red);
    gpiod_put(green);
    gpiod_put(btn1);
    gpiod_put(btn2);
    pr_info("good bye reader!\n");
    return 0;
}

static struct platform_driver mypdrv = {
    .probe      = my_pdrv_probe,
    .remove     = my_pdrv_remove,
    .driver     = {
        .name     = "gpio_descriptor_sample",
        .of_match_table = of_match_ptr(gpiod_dt_ids),
        .owner    = THIS_MODULE,
    },
};

module_platform_driver(mypdrv);
MODULE_AUTHOR("John Madieu <john.madieu@gmail.com>");
MODULE_LICENSE("GPL");

/* Function to init and exit functions */
module_init(mytimer_init);
module_exit(mytimer_exit);
