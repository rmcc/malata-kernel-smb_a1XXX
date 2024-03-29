/*
 * drivers/input/touchscreen/tegra_odm.c
 *
 * Touchscreen class input driver for platforms using NVIDIA's Tegra ODM kit
 * driver interface
 *
 * Copyright (c) 2009, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <linux/module.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/freezer.h>
//#include <linux/tegra_devices.h>
#include <nvodm_services.h>
#include <nvodm_touch.h>

//#define __TEGRA_ODM_DEBUG

#ifdef __TEGRA_ODM_DEBUG
#define TEGRA_ODM_PRINTF(x)	printk x;
#else
#define TEGRA_ODM_PRINTF(x)
#endif

#define PEN_RELEASE		0
#define PEN_DOWN 		1
#define TOUCH_INTERVAL		35

struct tegra_touch_driver_data
{
	struct input_dev	*input_dev;
	struct task_struct	*task;
	NvOdmOsSemaphoreHandle	semaphore;
	NvOdmTouchDeviceHandle	hTouchDevice;
	NvBool			bPollingMode;
	NvU32			pollingIntervalMS;
	NvOdmTouchCapabilities	caps;
	NvU32			MaxX;
	NvU32			MinX;
	NvU32			MaxY;
	NvU32			MinY;
	int			shutdown;
	struct early_suspend	early_suspend;
	bool pen_state;
	NvU32	Version;
	NvBool	bBurnBootloader;
	NvBool bIsSuspended;
};

#define NVODM_TOUCH_NAME "nvodm_touch"

#define swapv(x, y) do { typeof(x) z = x; x = y; y = z; } while (0)

//For burn touchscreen bootloader
static ssize_t tegra_touch_burn_bootloader(struct device *dev, struct device_attribute *attr, char *buf, size_t count)
{	
	printk(("==tegra_touch_burn_bootloader, begin == \n"));		
	struct tegra_touch_driver_data *touch = (struct tegra_touch_driver_data *)dev_get_drvdata(dev);	
	touch->bBurnBootloader = NV_TRUE;
	if(NvOdmTouchBurnBootloader(touch->hTouchDevice))	
	{		touch->bBurnBootloader = NV_FALSE;		
			printk("==tegra_touch_burn_bootloader, NvOdmTouchBurnBootloader OK ! == \n");		
	}	
	else	
	{		
			touch->bBurnBootloader = NV_FALSE;		
			printk("==tegra_touch_burn_bootloader, NvOdmTouchBurnBootloader fail ! == \n");	
	}	
	return count;
}
//burn bootloader interface
static DEVICE_ATTR(burnbootloader, S_IRWXUGO, NULL, tegra_touch_burn_bootloader);

static ssize_t tegra_touch_set_calibration(struct device *dev, struct device_attribute *attr, char *buf, size_t count)
{
	TEGRA_ODM_PRINTF(("==tegra_touch_set_calibration, please don't touch your panel ! == \n"));	
	struct tegra_touch_driver_data *touch = (struct tegra_touch_driver_data *)dev_get_drvdata(dev);
	
	NvOdmTouchSetCalibration(touch->hTouchDevice);

	TEGRA_ODM_PRINTF(("==tegra_touch_set_calibration, over ! == \n"));	
	return count;
}


//set calibration interface
static DEVICE_ATTR(calibration, S_IRWXUGO, NULL, tegra_touch_set_calibration);

static ssize_t tegra_touch_show_version(struct device *dev, struct device_attribute *attr, char *buf)
{
	TEGRA_ODM_PRINTF(("==tegra_touch_show_version begin ! == \n"));	
	struct tegra_touch_driver_data *touch = (struct tegra_touch_driver_data *)dev_get_drvdata(dev);
	
	return sprintf(buf, "%x\n", touch->Version);
}
//set Version interface
static DEVICE_ATTR(version, S_IRWXUGO, tegra_touch_show_version, NULL);

extern NvU32 NvOdmTouchDriverIndex(NvU32* pindex);
static int tegra_touch_thread(void *pdata);


#ifdef CONFIG_HAS_EARLYSUSPEND
static void tegra_touch_early_suspend(struct early_suspend *es)
{
	struct tegra_touch_driver_data *touch;
	touch = container_of(es, struct tegra_touch_driver_data, early_suspend);
	if (touch && touch->hTouchDevice) {
		if (!touch->bIsSuspended) {
			NvOdmTouchPowerOnOff(touch->hTouchDevice, NV_FALSE);
			touch->bIsSuspended = NV_TRUE;
		}
	}
	else {
		pr_err("tegra_touch_early_suspend: NULL handles passed\n");
	}
}

static void tegra_touch_late_resume(struct early_suspend *es)
{
	struct tegra_touch_driver_data *touch;
	touch = container_of(es, struct tegra_touch_driver_data, early_suspend);
	if (touch && touch->hTouchDevice) {
		if (touch->bIsSuspended) {
			NvOdmTouchPowerOnOff(touch->hTouchDevice, NV_TRUE);
			touch->bIsSuspended = NV_FALSE;
		}
	}
	else {
		pr_err("tegra_touch_late_resume: NULL handles passed\n");
	}
	//touch->bSuspended=NV_FALSE;
}

#endif

static int tegra_touch_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct tegra_touch_driver_data *touch = platform_get_drvdata(pdev);
		
	if (touch && touch->hTouchDevice) {
		if (!touch->bIsSuspended) {
			NvOdmTouchPowerOnOff(touch->hTouchDevice, NV_FALSE);
			touch->bIsSuspended = NV_TRUE;
			return 0;
		}
		else {
			// device is already suspended
			return 0;
		}
	}
	pr_err("tegra_touch_suspend: NULL handles passed\n");
	return -1;
}

static int tegra_touch_resume(struct platform_device *pdev)
{
	struct tegra_touch_driver_data *touch = platform_get_drvdata(pdev);
	if (touch && touch->hTouchDevice) {
		if (touch->bIsSuspended) {
			NvOdmTouchPowerOnOff(touch->hTouchDevice, NV_TRUE);
			touch->bIsSuspended = NV_FALSE;
			return 0;
		}
		else {
			return 0;
		}
	}
	pr_err("tegra_touch_resume: NULL handles passed\n");
	return -1;
}

//ak4183
static void pen_release_down(struct tegra_touch_driver_data *touch, int x, int y, bool release_down) {
	if (touch->pen_state != release_down) {
		input_report_abs(touch->input_dev, ABS_X, x);
		input_report_abs(touch->input_dev, ABS_Y, y);
		input_report_key(touch->input_dev, BTN_TOUCH, release_down);
		touch->pen_state = release_down;
	}
}
static void pen_release(struct tegra_touch_driver_data *touch, int x, int y)
{
	if (touch->pen_state != PEN_RELEASE) {
		input_report_abs(touch->input_dev, ABS_X, x);
		input_report_abs(touch->input_dev, ABS_Y, y);
		input_report_key(touch->input_dev, BTN_TOUCH, PEN_RELEASE);
		touch->pen_state = PEN_RELEASE;
		input_sync(touch->input_dev);
	}
	//pr_err("pen_release\n");
    TEGRA_ODM_PRINTF(("pen_release\n"));
}
static void pen_down(struct tegra_touch_driver_data *touch, int x, int y) 
{
		input_report_abs(touch->input_dev, ABS_X, x);
		input_report_abs(touch->input_dev, ABS_Y, y);
		input_report_key(touch->input_dev, BTN_TOUCH, PEN_DOWN);
		touch->pen_state = PEN_DOWN;
		input_sync(touch->input_dev);
		//pr_err("pen_down");
        TEGRA_ODM_PRINTF(("pen_down\n"));
}

static int tegra_touch_thread_ak4183(void *pdata)
{
	NvU32 x, y;
	NvBool bKeepReadingSample = NV_TRUE;
	NvOdmTouchCoordinateInfo coord = {0};	
	struct tegra_touch_driver_data *touch =
		(struct tegra_touch_driver_data*)pdata;	
	NvOdmTouchCapabilities *caps = &touch->caps;
	
	for (;;) {
		//pr_err("wating for touchscreen samephore...\n");
        TEGRA_ODM_PRINTF(("wating for touchscreen samephore...\n"));
		NvOdmOsSemaphoreWait(touch->semaphore);
		bKeepReadingSample = NV_TRUE;
		while(bKeepReadingSample) {
			if (NvOdmTouchReadCoordinate(touch->hTouchDevice, &coord)) {
				switch(coord.additionalInfo.Fingers) {
					case 1:
						x = coord.xcoord;
						y = coord.ycoord;
						if (caps->Orientation & NvOdmTouchOrientation_H_FLIP) {
							x = caps->XMaxPosition + caps->XMinPosition - x; 
						}
						if (caps->Orientation & NvOdmTouchOrientation_V_FLIP) {
							y = caps->YMaxPosition + caps->YMinPosition - y;
						}
						pen_down(touch, x, y);
						msleep(TOUCH_INTERVAL); 
						break;
					case 0:
					default:
						break;
				}
			}
			if (NvOdmTouchHandleInterrupt(touch->hTouchDevice)) {
				pen_release(touch, x, y);
				msleep(TOUCH_INTERVAL);
				bKeepReadingSample = NV_FALSE;
			}
		}
	}
}

static int __init tegra_touch_probe_ak4183(struct platform_device *pdev)
{
	struct tegra_touch_driver_data *touch = NULL;
	struct input_dev *input_dev = NULL;
	int err, i = 0;
	NvOdmTouchCapabilities *caps;

	touch = kzalloc(sizeof(struct tegra_touch_driver_data), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (input_dev == NULL || touch == NULL) {
		input_free_device(input_dev);
		kfree(touch);
		err = -ENOMEM;
		pr_err("tegra_touch_probe: Failed to allocate input device\n");
		return err;
	}
	touch->semaphore = NvOdmOsSemaphoreCreate(0);
	if (!touch->semaphore) {
		err = -1;
		pr_err("tegra_touch_probe: Semaphore creation failed\n");
		goto err_semaphore_create_failed;
	}

	touch->pen_state = PEN_RELEASE;
	
	if (!NvOdmTouchDeviceOpen(&touch->hTouchDevice)) {
		err = -1;
		pr_err("tegra_touch_probe: NvOdmTouchDeviceOpen failed\n");
		goto err_open_failed;
	}
	touch->bPollingMode = NV_FALSE;
	if (!NvOdmTouchEnableInterrupt(touch->hTouchDevice, touch->semaphore)) {
		err = -1;
		pr_err("tegra_touch_probe: Interrupt failed, polling mode\n");
		touch->bPollingMode = NV_TRUE;
		touch->pollingIntervalMS = 10;
	}

	touch->task =
		kthread_create(tegra_touch_thread, touch, "tegra_touch_thread");

	if(touch->task == NULL) {
		err = -1;
		goto err_kthread_create_failed;
	}
	wake_up_process( touch->task );

	touch->input_dev = input_dev;
	touch->input_dev->name = NVODM_TOUCH_NAME;

	/* Will generate sync at the end of all input */
	set_bit(EV_SYN, touch->input_dev->evbit);
	/* Event is key input type */
	set_bit(EV_KEY, touch->input_dev->evbit);
	/* virtual key is BTN_TOUCH */
	set_bit(BTN_TOUCH, touch->input_dev->keybit);
	/* Input values are in absoulte values */
	set_bit(EV_ABS, touch->input_dev->evbit);

	NvOdmTouchDeviceGetCapabilities(touch->hTouchDevice, &touch->caps);

	caps = &touch->caps;

	/* supported virtual keys */
	set_bit(BTN_TOUCH, touch->input_dev->keybit);
	for (i = 0; i < (caps->MaxNumberOfFingerCoordReported - 1); i++) {
		set_bit(BTN_2 + i, touch->input_dev->keybit);
	}
	touch->bIsSuspended = NV_FALSE;
	/* expose multi-touch capabilities */
	set_bit(ABS_MT_TOUCH_MAJOR, touch->input_dev->keybit);
	set_bit(ABS_MT_POSITION_X, touch->input_dev->keybit);
	set_bit(ABS_MT_POSITION_Y, touch->input_dev->keybit);
	set_bit(ABS_X, touch->input_dev->keybit);
	set_bit(ABS_Y, touch->input_dev->keybit);

	if (caps->Orientation & NvOdmTouchOrientation_XY_SWAP) {
		touch->MaxY = caps->XMaxPosition;
		touch->MinY = caps->XMinPosition;
		touch->MaxX = caps->YMaxPosition;
		touch->MinX = caps->YMinPosition;

	} else {
		touch->MaxX = caps->XMaxPosition;
		touch->MinX = caps->XMinPosition;
		touch->MaxY = caps->YMaxPosition;
		touch->MinY = caps->YMinPosition;
	}

	pr_err("-------------------------\n");
	pr_err("XMax=%d,XMin=%d\n", touch->MaxX, touch->MinX);
	pr_err("YMax=%d,YMin=%d\n", touch->MaxY, touch->MinY);
	pr_err("-------------------------\n");

	input_set_abs_params(touch->input_dev, ABS_X, touch->MinX,
		touch->MaxX, 0, 0);
	input_set_abs_params(touch->input_dev, ABS_Y, touch->MinY,
		touch->MaxY, 0, 0);
	input_set_abs_params(touch->input_dev, ABS_HAT0X, touch->MinX,
		touch->MaxX, 0, 0);
	input_set_abs_params(touch->input_dev, ABS_HAT0Y, touch->MinY,
		touch->MaxY, 0, 0);

	if (caps->IsPressureSupported)
		input_set_abs_params(touch->input_dev, ABS_PRESSURE, 0, 
			caps->MaxNumberOfPressureReported, 0, 0);
	if (caps->IsWidthSupported)
		input_set_abs_params(touch->input_dev, ABS_TOOL_WIDTH, 0, 
			caps->MaxNumberOfWidthReported, 0, 0);

	platform_set_drvdata(pdev, touch);

	err = input_register_device(input_dev);
	if (err)
	{
		pr_err("tegra_touch_probe: Unable to register input device\n");
		goto err_input_register_device_failed;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
        touch->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
        touch->early_suspend.suspend = tegra_touch_early_suspend;
        touch->early_suspend.resume = tegra_touch_late_resume;
        register_early_suspend(&touch->early_suspend);
#endif
	printk(KERN_INFO NVODM_TOUCH_NAME 
		": Successfully registered the ODM touch driver %x\n", touch->hTouchDevice);
	return 0;

err_input_register_device_failed:
	NvOdmTouchDeviceClose(touch->hTouchDevice);
err_kthread_create_failed:
	/* FIXME How to destroy the thread? Maybe we should use workqueues? */
err_open_failed:
	NvOdmOsSemaphoreDestroy(touch->semaphore);
err_semaphore_create_failed:
	kfree(touch);
	input_free_device(input_dev);
	return err;
}
//end ak4183

//at168 
static void tegra_touch_fingers_dealwith(struct tegra_touch_driver_data *touch, NvOdmTouchCoordinateInfo coord, NvU8 fingers)
{
	if(0 == fingers)
	{
		input_report_abs(touch->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(touch->input_dev, ABS_MT_WIDTH_MAJOR, 0);
		input_report_key(touch->input_dev, BTN_TOUCH, 0);
		input_mt_sync(touch->input_dev);
		input_sync(touch->input_dev);
	}

	if(1 == fingers)
	{
		input_report_abs(touch->input_dev, ABS_MT_POSITION_X, coord.additionalInfo.multi_XYCoords[0][0]);	
		input_report_abs(touch->input_dev, ABS_MT_POSITION_Y, coord.additionalInfo.multi_XYCoords[0][1]);
		input_report_abs(touch->input_dev, ABS_MT_TOUCH_MAJOR, 10);
		input_report_abs(touch->input_dev, ABS_MT_WIDTH_MAJOR, 20);
		input_report_key(touch->input_dev, BTN_TOUCH, 1);
		input_mt_sync(touch->input_dev);
		input_sync(touch->input_dev);
	}

	if(2 == fingers)
	{
		input_report_abs(touch->input_dev, ABS_MT_POSITION_X, coord.additionalInfo.multi_XYCoords[0][0]);	
		input_report_abs(touch->input_dev, ABS_MT_POSITION_Y, coord.additionalInfo.multi_XYCoords[0][1]);
		input_report_abs(touch->input_dev, ABS_MT_TOUCH_MAJOR, 10);
		input_report_abs(touch->input_dev, ABS_MT_WIDTH_MAJOR, 20);
		input_mt_sync(touch->input_dev);

		input_report_abs(touch->input_dev, ABS_MT_POSITION_X, coord.additionalInfo.multi_XYCoords[1][0]);
		input_report_abs(touch->input_dev, ABS_MT_POSITION_Y, coord.additionalInfo.multi_XYCoords[1][1]);
		input_report_abs(touch->input_dev, ABS_MT_TOUCH_MAJOR, 10);
		input_report_abs(touch->input_dev, ABS_MT_WIDTH_MAJOR, 20);
		input_mt_sync(touch->input_dev);
	}
	input_sync(touch->input_dev);
}

static int tegra_touch_thread_at168(void *pdata)
{
	struct tegra_touch_driver_data *touch =
		(struct tegra_touch_driver_data*)pdata;
	NvOdmTouchCoordinateInfo coord = {0};
	NvU16 i = 0;
	NvBool bKeepReadingSamples = NV_TRUE;
	NvOdmTouchCapabilities *caps = &touch->caps;

	/* touch event thread should be frozen before suspend */
	//set_freezable_with_signal();
	
	for (;;) 
	{
		NvOdmOsSemaphoreWait(touch->semaphore);

InBurnBootloader:
		if(NV_TRUE == touch->bBurnBootloader)
		{
			TEGRA_ODM_PRINTF(("==tegra_touch_thread_at168  in burnbootloader, thread stop ===\n"));
			msleep(1000);
			goto InBurnBootloader;
		}
		
		bKeepReadingSamples = NV_TRUE;
		//while (bKeepReadingSamples&&(!touch->bSuspended))
		while (bKeepReadingSamples)
		{
			if (!NvOdmTouchReadCoordinate(touch->hTouchDevice, &coord))
			{
				pr_err("Couldn't read touch coord \n");
				goto HandleInterrupt;
			}

			if (coord.fingerstate & NvOdmTouchSampleIgnore) //coordinate overflow (Min x y or Max x y)
				goto HandleInterrupt;
			
			/* transformation from touch to screen orientation */
			if (caps->Orientation & NvOdmTouchOrientation_V_FLIP) 
			{
				for (i = 0; i < coord.additionalInfo.Fingers; i++)
				{
					coord.additionalInfo.multi_XYCoords[i][1]
						= caps->YMaxPosition + caps->YMinPosition - coord.additionalInfo.multi_XYCoords[i][1];
				}
			}
			if (caps->Orientation & NvOdmTouchOrientation_H_FLIP)
			{
				for (i = 0; i < coord.additionalInfo.Fingers; i++)
				{
					coord.additionalInfo.multi_XYCoords[i][0] 
						= caps->XMaxPosition + caps->XMinPosition - coord.additionalInfo.multi_XYCoords[i][0];
				}
			}
			if (caps->Orientation & NvOdmTouchOrientation_XY_SWAP)
			{
				for (i = 0; i < coord.additionalInfo.Fingers; i++)
					swapv(coord.additionalInfo.multi_XYCoords[i][0], coord.additionalInfo.multi_XYCoords[i][1]);
			}
			
			#ifdef __TEGRA_ODM_DEBUG
			TEGRA_ODM_PRINTF(("==AT168 Read coord---FingerNum = %d  x[0]=%d y[0]=%d x[1]=%d y[1]=%d ===\n", 
				coord.additionalInfo.Fingers,
				coord.additionalInfo.multi_XYCoords[0][0], coord.additionalInfo.multi_XYCoords[0][1], 
				coord.additionalInfo.multi_XYCoords[1][0], coord.additionalInfo.multi_XYCoords[1][1]));
			#endif

			switch(coord.additionalInfo.Fingers)
			{
				#if 0
				case 0:
					tegra_touch_fingers_dealwith(touch, coord, 0); //fingers = 0

					msleep(20);
					break;
				#endif
				case 0:
					break;
				
				case 1:
					tegra_touch_fingers_dealwith(touch, coord, 1); //fingers = 1
					msleep(10);
					break;
				case 2:
					tegra_touch_fingers_dealwith(touch, coord, 2); //fingers = 2
					msleep(10);
					break;
				default:
					break;
			}

HandleInterrupt:
			if(NvOdmTouchHandleInterrupt(touch->hTouchDevice)) 
			{
				//if(!touch->bSuspended){
				tegra_touch_fingers_dealwith(touch, coord, 0); //fingers = 0
				msleep(30);
				//}
				bKeepReadingSamples = NV_FALSE;
			}
		}
	}

	return 0;
}

static int __init tegra_touch_probe_at168(struct platform_device *pdev)
{
    struct tegra_touch_driver_data *touch = NULL;
    struct input_dev *input_dev = NULL;
    int err;
    NvOdmTouchCapabilities *caps;
	//pr_err("tegra_touch_probe:at1688888888888888888888888888888\n");
    touch = kzalloc(sizeof(struct tegra_touch_driver_data), GFP_KERNEL);
    input_dev = input_allocate_device();
    if (input_dev == NULL || touch == NULL) {
        input_free_device(input_dev);
        kfree(touch);
        err = -ENOMEM;
        pr_err("tegra_touch_probe: Failed to allocate input device\n");
        return err;
    }
    touch->semaphore = NvOdmOsSemaphoreCreate(0);
    if (!touch->semaphore) {
        err = -1;
        pr_err("tegra_touch_probe: Semaphore creation failed\n");
        goto err_semaphore_create_failed;
    }

    if (!NvOdmTouchDeviceOpen(&touch->hTouchDevice)) {
        err = -1;
        pr_err("tegra_touch_probe: NvOdmTouchDeviceOpen failed\n");
        goto err_open_failed;
    }
    touch->bPollingMode = NV_TRUE;
	touch->bIsSuspended = NV_FALSE;

    if (!NvOdmTouchEnableInterrupt(touch->hTouchDevice, touch->semaphore)) {
        err = -1;
        pr_err("tegra_touch_probe: Interrupt failed, polling mode\n");
        touch->bPollingMode = NV_TRUE;
        touch->pollingIntervalMS = 10;
    }

    touch->task =
        kthread_create(tegra_touch_thread, touch, "tegra_touch_thread");

    if(touch->task == NULL) {
        err = -1;
        goto err_kthread_create_failed;
    }

    wake_up_process( touch->task );

    touch->input_dev = input_dev;
    touch->input_dev->name = NVODM_TOUCH_NAME;

    /* Will generate sync at the end of all input */
    set_bit(EV_SYN, touch->input_dev->evbit);

    /* Event is key input type */
    set_bit(EV_KEY, touch->input_dev->evbit);
    /* virtual key is BTN_TOUCH */
    set_bit(BTN_TOUCH, touch->input_dev->keybit);
    /* virtual key is BTN_2 */
    set_bit(BTN_2, touch->input_dev->keybit);
    /* Input values are in absoulte values */
    set_bit(EV_ABS, touch->input_dev->evbit);
    
    NvOdmTouchDeviceGetCapabilities(touch->hTouchDevice, &touch->caps);

    caps = &touch->caps;

    if (caps->Orientation & NvOdmTouchOrientation_XY_SWAP) {
        touch->MaxY = caps->XMaxPosition;
        touch->MinY = caps->XMinPosition;
        touch->MaxX = caps->YMaxPosition;
        touch->MinX = caps->YMinPosition;

    } else {
        touch->MaxX = caps->XMaxPosition;
        touch->MinX = caps->XMinPosition;
        touch->MaxY = caps->YMaxPosition;
        touch->MinY = caps->YMinPosition;
    }

    input_set_abs_params(touch->input_dev, ABS_X, touch->MinX, touch->MaxX, 0, 0);
    input_set_abs_params(touch->input_dev, ABS_Y, touch->MinY, touch->MaxY, 0, 0);
    input_set_abs_params(touch->input_dev, ABS_HAT0X, touch->MinX, touch->MaxX, 0, 0);
    input_set_abs_params(touch->input_dev, ABS_HAT0Y, touch->MinY, touch->MaxY, 0, 0);

    if (caps->IsPressureSupported)
        input_set_abs_params(touch->input_dev, ABS_PRESSURE, 0, caps->MaxNumberOfPressureReported, 0, 0);
    if (caps->IsWidthSupported)
        input_set_abs_params(touch->input_dev, ABS_TOOL_WIDTH, 0, caps->MaxNumberOfWidthReported, 0, 0);

    //*******************************************************************************//
    //Add it for Multi-touch
    input_set_abs_params(touch->input_dev, ABS_MT_POSITION_X, touch->MinX, touch->MaxX, 0, 0);
    input_set_abs_params(touch->input_dev, ABS_MT_POSITION_Y, touch->MinY, touch->MaxY, 0, 0);
    input_set_abs_params(touch->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
    input_set_abs_params(touch->input_dev, ABS_MT_WIDTH_MAJOR, 0, 200, 0, 0);
    //*******************************************************************************//
    //Add the version of touchscreen
    touch->Version = caps->Version;
    //touch->bSuspended=NV_FALSE;
    //*******************************************************************************//
    platform_set_drvdata(pdev, touch);

    err = input_register_device(input_dev);
    if (err)
    {
        pr_err("tegra_touch_probe: Unable to register input device\n");
        goto err_input_register_device_failed;
    }

 //*************************************************//
    //create calibration file,  path  ->   /sys/devices/platform/tegra_touch/calibration
    err = device_create_file(&pdev->dev, &dev_attr_calibration);
    if (err) {
		pr_err("tegra_touch_probe : device_create_file calibration failed\n");
		goto err_input_register_device_failed;
	}
    //create version file,  path  ->   /sys/devices/platform/tegra_touch/version
    err = device_create_file(&pdev->dev, &dev_attr_version);
    if (err) {
		pr_err("tegra_touch_probe : device_create_file version failed\n");
		goto err_input_register_device_failed;
	}
    //create bootloader file,  path  ->   /sys/devices/platform/tegra_touch/burnbootloader    
   err = device_create_file(&pdev->dev, &dev_attr_burnbootloader);    
   if (err) {		
   		pr_err("tegra_touch_probe : device_create_file burnbootloader failed\n");		
		goto err_input_register_device_failed;	
	}
//*************************************************//

#ifdef CONFIG_HAS_EARLYSUSPEND
       touch->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
       touch->early_suspend.suspend = tegra_touch_early_suspend;
       touch->early_suspend.resume = tegra_touch_late_resume;
       register_early_suspend(&touch->early_suspend);
#endif
    printk(KERN_INFO NVODM_TOUCH_NAME 
        ": Successfully registered the ODM touch driver %x\n", touch->hTouchDevice);
    return 0;

err_input_register_device_failed:
    NvOdmTouchDeviceClose(touch->hTouchDevice);
err_kthread_create_failed:
    /* FIXME How to destroy the thread? Maybe we should use workqueues? */
err_open_failed:
    NvOdmOsSemaphoreDestroy(touch->semaphore);
err_semaphore_create_failed:
    kfree(touch);
    input_free_device(input_dev);
    return err;
}

//end at168

//tpk
static int tegra_touch_thread_tpk(void *pdata)
{
	struct tegra_touch_driver_data *touch =
		(struct tegra_touch_driver_data*)pdata;
	NvOdmTouchCoordinateInfo c = {0};
	NvU32 x[2] = {0}, y[2] = {0}, i = 0;
	NvBool bKeepReadingSamples;
	NvU32 fingers = 0;
	NvBool ToolDown[2] = {NV_FALSE, NV_FALSE};
	NvOdmTouchCapabilities *caps = &touch->caps;
	NvU32 max_fingers = caps->MaxNumberOfFingerCoordReported;

	/* touch event thread should be frozen before suspend */
	set_freezable_with_signal();

	for (;;) {
		if (touch->bPollingMode)
			msleep(touch->pollingIntervalMS); 
		else
			NvOdmOsSemaphoreWait(touch->semaphore);

		bKeepReadingSamples = NV_TRUE;
		while (bKeepReadingSamples) {
			if (!NvOdmTouchReadCoordinate(touch->hTouchDevice, &c)){
				pr_err("Couldn't read touch sample\n");
				bKeepReadingSamples = NV_FALSE;
				continue;
			}

			if (c.fingerstate & NvOdmTouchSampleIgnore)
				goto DoneWithSample;

			fingers = c.additionalInfo.Fingers;

			switch (fingers) {
			case 0:
				for (i=0; i<max_fingers; i++) {
					ToolDown[i] = NV_FALSE;
					bKeepReadingSamples = NV_FALSE;
				}
				break;
			case 1:
				pr_err("ts_tegra_odm: one finger!\n");
				ToolDown[0] = NV_TRUE;
				ToolDown[1] = NV_FALSE;
				break;
			case 2:
				for (i=0; i<max_fingers; i++) {
					ToolDown[i] = NV_TRUE;
				}
				break;
			default:
				// can occur because of sensor errors
				c.fingerstate = NvOdmTouchSampleIgnore;
				goto DoneWithSample;
			}

			if (fingers == 1) {
				x[0] = c.xcoord;
				y[0] = c.ycoord;
			}
			else {
				for (i = 0; i < fingers; i++) {
					x[i] = c.additionalInfo.multi_XYCoords[i][0];
					y[i] = c.additionalInfo.multi_XYCoords[i][1];
				}
			}
#if 0
			/* transformation from touch to screen orientation */
			if (caps->Orientation & NvOdmTouchOrientation_V_FLIP) {
				y[0] = caps->YMaxPosition +
					caps->YMinPosition - y[0];
				y[1] = caps->YMaxPosition +
					caps->YMinPosition - y[1];
			}
			if (caps->Orientation & NvOdmTouchOrientation_H_FLIP) {
				x[0] = caps->XMaxPosition +
					caps->XMinPosition - x[0];
				x[1] = caps->XMaxPosition +
					caps->XMinPosition - x[1];
			}

			if (caps->Orientation & NvOdmTouchOrientation_XY_SWAP) {
				for (i = 0; i < max_fingers; i++)
					swapv(x[i],y[i]);
			}
#endif
			if (c.fingerstate & NvOdmTouchSampleValidFlag) {
				pr_err("report_abs_x_y/n");
				input_report_abs(touch->input_dev, ABS_X, x[0]);
				input_report_abs(touch->input_dev, ABS_Y, y[0]);
			}

			pr_err("ABS_X=%d,ABS_Y=%d\n", x[0], y[0]);
#if 0
			if (caps->IsPressureSupported) {
				input_report_abs(touch->input_dev,
					ABS_PRESSURE, 
					c.additionalInfo.Pressure[0]);
			}
			if (caps->IsWidthSupported) {
				input_report_abs(touch->input_dev,
					ABS_TOOL_WIDTH, 
					c.additionalInfo.width[0]);
			}
#endif
			/* Report down or up flag */
			input_report_key(touch->input_dev,
					BTN_TOUCH, ToolDown[0]);
#if 0
			// report co-ordinates for the 2nd finger
			if (fingers == 2) {
				input_report_abs(touch->input_dev,
					ABS_HAT0X, x[1]); // x
				input_report_abs(touch->input_dev,
					ABS_HAT0Y, y[1]); // y
				input_report_key(touch->input_dev,
					BTN_2, ToolDown[1]);
			}
#endif
			input_sync(touch->input_dev);

DoneWithSample:
			NvOdmTouchHandleInterrupt(touch->hTouchDevice);
#if 0
			bKeepReadingSamples = NV_FALSE;
			if (!touch->bPollingMode && 
				!NvOdmTouchHandleInterrupt(touch->hTouchDevice)) {
				pr_err("ts_tegra_odm: Keep looping\n");
				/* Some more data to read keep going */
				bKeepReadingSamples = NV_TRUE;
			} 
#endif
		}
	}

	return 0;
}

static int __init tegra_touch_probe_tpk(struct platform_device *pdev)
{
    return -1;
}
//end tpk





static int tegra_touch_thread(void *pdata)
{
   if(NULL==pdata)
   {
	pr_err("tegra_touch_thread NULL==pdata \n");
	return 0;
   }
   msleep(8000);
   switch(NvOdmTouchDriverIndex(NULL))
   {
     case 1:
        tegra_touch_thread_tpk(pdata);
        break;
     case 2:
        tegra_touch_thread_at168(pdata);
        break;
     case 4:
        tegra_touch_thread_ak4183(pdata);
        break;
     default:
        break;
   }
   return 0;
}


static int __init tegra_touch_probe(struct platform_device *pdev)
{
    int err=-1;
    NvU32 index=0;

    #if defined(NV_TOUCH_TPK)
    if(err) 
	{ 
        index=1;
        NvOdmTouchDriverIndex(&index);
		err=tegra_touch_probe_tpk(pdev);
         pr_err("tegra_touch_probe_tpk: err=%d\n",err);
         
	}
   
    #endif

#if defined(CONFIG_TOUCHSCREEN_TEGRA_ODM_AT168)
   if(err) 
	{ 
        index=2;
        NvOdmTouchDriverIndex(&index);
		err=tegra_touch_probe_at168(pdev);
         pr_err("tegra_touch_probe_at168: err=%d\n",err);
	}
#endif

#if defined(CONFIG_TOUCHSCREEN_TEGRA_ODM_AK4183)
    if(err) 
	{ 
         index=4;
        NvOdmTouchDriverIndex(&index);
		err=tegra_touch_probe_ak4183(pdev);
         pr_err("tegra_touch_probe_ak4183: err=%d\n",err);
	}
#endif

  if(err) { index=0; NvOdmTouchDriverIndex(&index);  }
    //pr_err("NvOdmTouchDriverIndex()=%d\n",NvOdmTouchDriverIndex(NULL));
    pr_err("tegra_touch_probe: err=%d\n",err);
   
        
   return err;
}


static int tegra_touch_remove(struct platform_device *pdev)
{
	struct tegra_touch_driver_data *touch = platform_get_drvdata(pdev);

#ifdef CONFIG_HAS_EARLYSUSPEND
        unregister_early_suspend(&touch->early_suspend);
#endif
#if defined(CONFIG_TOUCHSCREEN_TEGRA_ODM_AT168)
	//remove the calibration file
	device_remove_file(&pdev->dev, &dev_attr_calibration);
	//remove the version file
	device_remove_file(&pdev->dev, &dev_attr_version);
#endif
        touch->shutdown = 1;
	/* FIXME How to destroy the thread? Maybe we should use workqueues? */
	input_unregister_device(touch->input_dev);
	/* NvOsSemaphoreDestroy(touch->semaphore); */
	kfree(touch);
	return 0;
}

static struct platform_driver tegra_touch_driver = {
	.probe	  = tegra_touch_probe,
	.remove	 = tegra_touch_remove,
	.suspend = tegra_touch_suspend,
	.resume	 = tegra_touch_resume,
	.driver	 = {
		.name   = "tegra_touch",
	},
};

static int __devinit tegra_touch_init(void)
{
	return platform_driver_register(&tegra_touch_driver);
}

static void __exit tegra_touch_exit(void)
{
	platform_driver_unregister(&tegra_touch_driver);
}

module_init(tegra_touch_init);
module_exit(tegra_touch_exit);

MODULE_DESCRIPTION("Tegra ODM touch driver");


