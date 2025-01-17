/*
 * pps_gen_gpio.c -- kernel GPIO PPS signal generator
 *
 * Copyright (C)  2009   Alexander Gordeev <lasaine@lvk.cs.msu.su>
 *                2018   Juan Solano <jsm@jsolano.com>
 * 				  2020	 Ken Lu <bluewish.ken.lu@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/time.h>
#include <linux/hrtimer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/version.h>

#define DRVDESC "GPIO PPS signal generator"
#define DRVNAME "pps_gen_gpio"

MODULE_AUTHOR("Ken Lu <bluewish.ken.lu@gmail.com>");
MODULE_DESCRIPTION(DRVDESC);
MODULE_LICENSE("GPL");

#define GPIO_PULSE_WIDTH_DEF_NS (30 * NSEC_PER_USEC)    /* 30us */
#define GPIO_PULSE_WIDTH_MAX_NS (100 * NSEC_PER_USEC)   /* 100us */
#define SAFETY_INTERVAL_NS      (10 * NSEC_PER_USEC)    /* 10us */

enum pps_gen_gpio_level {
	PPS_GPIO_LOW = 0,
	PPS_GPIO_HIGH
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
#define TIMESPECTYPE timespec
#define TIMEVALTYPE  timeval
#define TIMESPEC_ADD(x,y)   timespec_add((x),(y))
#define TIMESPEC_SUB(x,y)   timespec_sub((x),(y))
#define TIMESPEC_TO_NS(x)   timespec_to_ns(x)
#define GETNSTIMEOFDAY(x)   getnstimeofday(x)
#define KTIME_TO_TIMESPEC(kt)   ktime_to_timespec(kt)
#else
#define TIMESPECTYPE timespec64
#define TIMEVALTYPE  timespec64
#define TIMESPEC_ADD(x,y)   timespec64_add((x),(y))
#define TIMESPEC_SUB(x,y)   timespec64_sub((x),(y))
#define TIMESPEC_TO_NS(x)   timespec64_to_ns(x)
#define GETNSTIMEOFDAY(x)   ktime_get_real_ts64(x)
#define KTIME_TO_TIMESPEC(kt)   ktime_to_timespec64(kt)
#endif

/* Module parameters. */
static unsigned int gpio_pulse_width_ns = GPIO_PULSE_WIDTH_DEF_NS;
MODULE_PARM_DESC(width, "Delay between setting and dropping the signal (ns)");
module_param_named(width, gpio_pulse_width_ns, uint, 0000);

/* Device private data structure. */
struct pps_gen_gpio_devdata {
	struct gpio_desc *pps_gpio;     /* GPIO port descriptor */
	struct hrtimer timer;
	long gpio_instr_time;           /* measured port write time (ns) */
};

/* Average of hrtimer interrupt latency. */
static long hrtimer_avg_latency = SAFETY_INTERVAL_NS;

/* hrtimer event callback */
static enum hrtimer_restart hrtimer_callback(struct hrtimer *timer)
{
	unsigned long irq_flags;
	long hrtimer_latency;
	struct pps_gen_gpio_devdata *devdata =
		container_of(timer, struct pps_gen_gpio_devdata, timer);
	const long time_gpio_deassert_ns =
		NSEC_PER_SEC - devdata->gpio_instr_time;
	const long time_gpio_assert_ns =
		time_gpio_deassert_ns - gpio_pulse_width_ns;
	struct TIMESPECTYPE ts_expire_req, ts_expire_real, ts_gpio_instr_time,
			ts_hrtimer_latency, ts1, ts2;

	/* We have to disable interrupts here. The idea is to prevent
	 * other interrupts on the same processor to introduce random
	 * lags while polling the clock; GETNSTIMEOFDAY() takes <1us on
	 * most machines while other interrupt handlers can take much
	 * more potentially.
	 *
	 * Note: approximate time with blocked interrupts =
	 * gpio_pulse_width_ns + SAFETY_INTERVAL_NS + average hrtimer latency
	 */
	local_irq_save(irq_flags);

	/* Get current timestamp and requested time to check if we are late. */
	GETNSTIMEOFDAY(&ts_expire_real);
	ts_expire_req = KTIME_TO_TIMESPEC(hrtimer_get_softexpires(timer));
	if (ts_expire_real.tv_sec > ts_expire_req.tv_sec) {
		/* At begining of bootime, the time might not correct. */
		hrtimer_set_expires(timer,
			    ktime_set(ts_expire_real.tv_sec + 1,
				      time_gpio_assert_ns
				      - hrtimer_avg_latency
				      - SAFETY_INTERVAL_NS));
		return HRTIMER_RESTART;
	} else if (ts_expire_req.tv_sec != ts_expire_real.tv_sec || \
			   ts_expire_real.tv_nsec > time_gpio_assert_ns) {
		local_irq_restore(irq_flags);
		pr_err(DRVNAME "We are late this time req:[%lld.%09ld] real:[%lld.%09ld]\n",
		        ts_expire_req.tv_sec, ts_expire_req.tv_nsec,
				ts_expire_real.tv_sec, ts_expire_real.tv_nsec);
		goto done;
	}

	pr_info(DRVNAME " - GPIO event ...\n");

	/* Busy loop until the time is right for a GPIO assert. */
	do
		GETNSTIMEOFDAY(&ts1);
	while (ts_expire_req.tv_sec == ts1.tv_sec
	       && ts1.tv_nsec < time_gpio_assert_ns);

	/* Assert PPS GPIO. */
	gpiod_set_value(devdata->pps_gpio, PPS_GPIO_HIGH);

	/* Busy loop until the time is right for a GPIO deassert. */
	do
		GETNSTIMEOFDAY(&ts1);
	while (ts_expire_req.tv_sec == ts1.tv_sec
	       && ts1.tv_nsec < time_gpio_deassert_ns);

	/* Deassert PPS GPIO. */
	gpiod_set_value(devdata->pps_gpio, PPS_GPIO_LOW);

	GETNSTIMEOFDAY(&ts2);
	local_irq_restore(irq_flags);

	/* Update the calibrated GPIO set instruction time. */
	ts_gpio_instr_time = TIMESPEC_SUB(ts2, ts1);
	devdata->gpio_instr_time = (devdata->gpio_instr_time
				    + TIMESPEC_TO_NS(&ts_gpio_instr_time)) / 2;

done:
	/* Update the average hrtimer latency. */
	ts_hrtimer_latency = TIMESPEC_SUB(ts_expire_real, ts_expire_req);
	hrtimer_latency = TIMESPEC_TO_NS(&ts_hrtimer_latency);

	/* If the new latency value is bigger then the old, use the new
	 * value, if not then slowly move towards the new value. This
	 * way it should be safe in bad conditions and efficient in
	 * good conditions.
	 */
	if (hrtimer_latency > hrtimer_avg_latency)
		hrtimer_avg_latency = hrtimer_latency;
	else
		hrtimer_avg_latency =
			(3 * hrtimer_avg_latency + hrtimer_latency) / 4;

	/* Update the hrtimer expire time. */
	hrtimer_set_expires(timer,
			    ktime_set(ts_expire_req.tv_sec + 1,
				      time_gpio_assert_ns
				      - hrtimer_avg_latency
				      - SAFETY_INTERVAL_NS));

	return HRTIMER_RESTART;
}

/* Initial calibration of GPIO set instruction time. */
#define PPS_GEN_CALIBRATE_LOOPS 100
static void pps_gen_calibrate(struct pps_gen_gpio_devdata *devdata)
{
	int i;
	long time_acc = 0;

	for (i = 0; i < PPS_GEN_CALIBRATE_LOOPS; i++) {
		struct TIMESPECTYPE ts1, ts2, ts_delta;
		unsigned long irq_flags;

		local_irq_save(irq_flags);
		GETNSTIMEOFDAY(&ts1);
		gpiod_set_value(devdata->pps_gpio, PPS_GPIO_LOW);
		GETNSTIMEOFDAY(&ts2);
		local_irq_restore(irq_flags);

		ts_delta = TIMESPEC_SUB(ts2, ts1);
		time_acc += TIMESPEC_TO_NS(&ts_delta);
	}

	devdata->gpio_instr_time = time_acc / PPS_GEN_CALIBRATE_LOOPS;
	pr_info(DRVNAME " PPS GPIO set takes %ldns\n", devdata->gpio_instr_time);
}

static ktime_t pps_gen_first_timer_event(struct pps_gen_gpio_devdata *devdata)
{
	struct TIMESPECTYPE ts;

	GETNSTIMEOFDAY(&ts);
	/* First timer callback will be triggered between 1 and 2 seconds from
	 * now, synchronized to the tv_sec increment of the wall-clock time.
	 */
	return ktime_set(ts.tv_sec + 1,
			 NSEC_PER_SEC - gpio_pulse_width_ns
			 - devdata->gpio_instr_time - 3 * SAFETY_INTERVAL_NS);
}

static int pps_gen_gpio_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct pps_gen_gpio_devdata *devdata;

	pr_info(DRVNAME "probe \n");

	/* Allocate space for device info. */
	devdata = devm_kzalloc(dev,
			       sizeof(struct pps_gen_gpio_devdata),
			       GFP_KERNEL);
	if (!devdata) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	/* There should be a single PPS generator GPIO pin defined in DT. */
	if (of_gpio_named_count(dev->of_node, "pps-gen-gpios") != 1) {
		dev_err(dev, "There should be exactly one pps-gen GPIO defined in DT\n");
		ret = -EINVAL;
		goto err_dt;
	}

	devdata->pps_gpio = devm_gpiod_get(dev, "pps-gen", GPIOD_OUT_LOW);
	if (IS_ERR(devdata->pps_gpio)) {
		ret = PTR_ERR(devdata->pps_gpio);
		dev_err(dev, "Cannot get PPS GPIO [%d]\n", ret);
		goto err_gpio_get;
	}

	platform_set_drvdata(pdev, devdata);

	ret = gpiod_direction_output(devdata->pps_gpio, PPS_GPIO_HIGH);
	if (ret < 0) {
		dev_err(dev, "Cannot configure PPS GPIO\n");
		goto err_gpio_dir;
	}

	pps_gen_calibrate(devdata);
	hrtimer_init(&devdata->timer, CLOCK_REALTIME, HRTIMER_MODE_ABS);
	devdata->timer.function = hrtimer_callback;
	hrtimer_start(&devdata->timer,
		      pps_gen_first_timer_event(devdata),
		      HRTIMER_MODE_ABS);
	return 0;

err_gpio_dir:
	devm_gpiod_put(dev, devdata->pps_gpio);
err_gpio_get:
err_dt:
	devm_kfree(dev, devdata);
err_alloc:
	return ret;
}

static int pps_gen_gpio_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pps_gen_gpio_devdata *devdata = platform_get_drvdata(pdev);

	devm_gpiod_put(dev, devdata->pps_gpio);
	hrtimer_cancel(&devdata->timer);
	return 0;
}

/* The compatible property here defined is searched for in the DT */
static const struct of_device_id pps_gen_gpio_dt_ids[] = {
	{ .compatible = "pps-gen-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pps_gen_gpio_dt_ids);

static struct platform_driver pps_gen_gpio_driver = {
	.driver			= {
		.name		= DRVNAME,
		.owner		= THIS_MODULE,
		.of_match_table = of_match_ptr(pps_gen_gpio_dt_ids),
	},
	.probe			= pps_gen_gpio_probe,
	.remove			= pps_gen_gpio_remove,
};

static int __init pps_gen_gpio_init(void)
{
	if (gpio_pulse_width_ns > GPIO_PULSE_WIDTH_MAX_NS) {
		pr_err(DRVNAME "width value should be not greater than %ldns\n",
		       GPIO_PULSE_WIDTH_MAX_NS);
		return -EINVAL;
	}
	platform_driver_register(&pps_gen_gpio_driver);
	return 0;
}

static void __exit pps_gen_gpio_exit(void)
{
	pr_info(DRVNAME "hrtimer average latency is %ldns\n",
		hrtimer_avg_latency);
	platform_driver_unregister(&pps_gen_gpio_driver);
}

module_init(pps_gen_gpio_init);
module_exit(pps_gen_gpio_exit);
